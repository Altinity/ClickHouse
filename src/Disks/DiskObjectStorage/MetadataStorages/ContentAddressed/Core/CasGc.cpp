#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <base/defines.h>
#include <city.h>
#include <algorithm>
#include <limits>
#include <set>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

UInt128 cityHash128(const String & bytes)
{
    const auto h = CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

/// The blob object key for a content hash.
String blobKeyOf(const Layout & layout, const UInt128 & hash)
{
    return layout.blobKey(BlobId(u128ToHex(hash)));
}

}

uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len)
{
    if (kind != ObjectKind::Blob)
        return object_size;
    if (object_size < blob_header_len)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc retire: blob object of {} bytes is smaller than the pool's fixed blob header ({} bytes)",
            object_size, blob_header_len);
    return object_size - blob_header_len;
}

Gc::Gc(StorePtr store_, UInt128 gc_id_)
    : store(std::move(store_))
    , gc_id(gc_id_)
{
    if (!store)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: store must not be null");
    if (gc_id == UInt128(0))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: gc_id must not be 0 (reserved for 'lease never held')");
}

RoundReport Gc::runRegularRound()
{
    RoundReport report;
    GcState state;
    Token state_token;
    report.acquired_lease = acquireOrRenewLease(state, state_token);
    if (!report.acquired_lease)
        return report;

    /// CRASH-RESUME first: an incomplete prior round (its fold_seal durable, completion_seal absent,
    /// retired sets present) is finished from durable state before any new round starts.
    if (tryResumeIncompleteRound(state, state_token, report))
        return report;

    /// B170: fold begins — the round's R1. round here is state.round (pre-retire); generation is the
    /// authoritative one being folded.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldBegin;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = state.round;
        e.gen = state.snap_generation;
        e.reason = "R1: fold the RootOwnerEvent journals into a new durable blob in-degree generation";
    });

    /// R1: fold the ONE ordered RootOwnerEvent journal into blob deltas, seal them into a write-once
    /// blob in-degree generation (the fold cursor advances only on activation/removal — the barrier).
    FoldResult folded = fold(state, state_token, report);

    /// B170: fold ended — the new sealed generation (advanced past every clamp-free shard).
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldEnd;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = state.round;
        e.gen = state.snap_generation;
        e.outcome = "ok";
        e.reason = "R1 complete";
        e.detail = {{"shards", std::to_string(folded.root_shards.size())},
                    {"anomalies", std::to_string(report.anomalies.size())}};
    });

    /// R2: HEAD-observe each zero-in-degree blob's current token, write the round's retire sets, advance
    /// .round. Threaded (state, token) — never re-read (zombie-steal protection).
    const RetireResult retired = retire(state, state_token, folded, report);
    report.round = state.round;
    for (const auto & [shard, set] : retired.blobs)
        report.candidates += set.entries.size();

    /// R3: fence every present shard (LIST-discovered); record fence positions into the completion seal.
    fence(state, state_token, folded);

    /// R4: fold-through-fence recheck + the single content-delete site + exact-token manifest deletes;
    /// seal the completion generation; drop the round's retired sets.
    recheck(state, state_token, folded, retired, report);

    /// Task 6: reclaim empty+tombstoned+fully-folded ref-shard objects. Must run BEFORE trim so the
    /// tombstone event is still present in the journal (trim removes events at/below the fold cursor,
    /// which includes the tombstone). The fully-folded guard compares the fold cursor against the
    /// tombstone's transition_version (not root.shard_version, which fence may have bumped). Never throws
    /// (per-shard error isolation: a bad shard logs and continues, never aborts the whole reclaim pass
    /// or lets trim prematurely erase a pending tombstone). Best-effort: a missed reclaim retries next round.
    reclaimDroppedShards(folded);

    /// Trim journals below the sealed fold cursor.
    if (trim_enabled)
        trim(folded, state.round);

    /// Bounded orphan-manifest backstop (spec §Orphan sweep): cleanup-only cursor progress. A failed
    /// sweep must not fail the already-completed reachability round.
    try
    {
        runManifestSweepCursorPass(state, state_token);
    }
    catch (const Exception & e)
    {
        LOG_WARNING(getLogger("CasGc"), "CAS gc orphan sweep skipped this round: {}", e.message());
    }

    return report;
}

bool Gc::foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    const String key = layout.manifestKey(id);
    const HeadResult head = backend.head(key);
    if (!head.exists)
        return false;   /// absent body: caller decides (missing-body precommit OK; committed => fail closed)

    const auto got = backend.get(key);
    if (!got)
        return false;   /// raced delete between HEAD and GET — record-and-continue (never throw on a 404)

    const PartManifest body = decodePartManifest(got->bytes);
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body ref mismatch at {} (refMatchesBody fail-closed)", key);
    if (!manifestNamespaceMatches(id.root_namespace, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body namespace mismatch at {} (manifestNamespaceMatches fail-closed)", key);

    for (const ManifestEntry & entry : body.entries)
        if (entry.placement == EntryPlacement::Blob)
        {
            deltas.push_back(BlobDelta{
                .blob_hash = entry.blob_hash,
                .source_id = sourceEdgeId(id, entry.path),
                .remove = (sign < 0)});
            /// B170: a folded owner edge over this blob (the manifest-model analog of the old tree
            /// RootAdd/TreeExpand). +1 = the manifest's owner activated this blob's reference; -1 =
            /// the owner was removed, dropping the reference. Reconstructs WHY a blob's in-degree moved.
            EventEmitter{*store}.emit([&](CasEvent & ev)
            {
                ev.type = sign > 0 ? CasEventType::RootAdd : CasEventType::RootRemove;
                ev.namespace_ = id.root_namespace.string();
                ev.object_kind = CasEventObjectKind::Blob;
                ev.object_hash = u128ToHex(entry.blob_hash);
                ev.outcome = sign > 0 ? "edge_added" : "edge_removed";
                ev.reason = sign > 0
                    ? "fold: manifest owner activated; +1 blob edge"
                    : "fold: manifest owner removed; -1 blob edge";
                ev.detail = {{"manifest_ref_instance", manifestRefDebugString(id.ref)},
                             {"path", entry.path}};
            });
        }

    if (sign < 0)
        mf_cleanup.emplace(id, got->token);   /// owner removed: defer exact-token body delete to recheck
    return true;
}

Gc::FoldResult Gc::fold(GcState & state, Token & state_token, RoundReport & report)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();
    FoldResult result;

    /// 1. Discover the present (namespace, shard) pairs via LIST(cas/refs/) (Task 4 LIST-based discovery).
    result.root_shards = discoverUniverse();

    /// Parent cursors — the per-(ns,shard) cursors a prior round sealed. M1: read them from the LATEST
    /// seal at snap_generation. After a COMPLETED round the snap_generation pointer is the completion
    /// generation (whose fold_seal lives at the PARENT generation, so readFoldSeal(snap_generation) is
    /// nullopt); the cursors were carried forward into that completion seal's folded_cursors. Mid-round
    /// (fold sealed, completion not yet) the fold seal at snap_generation carries them. Absent both =>
    /// fresh pool (cursor 0). This is independent of trim — a folded-but-untrimmed event must never be
    /// re-folded from 0 (that double-counts blob in-degree => silent over-pin/leak).
    const std::map<String, ShardCoverage> parent_cursors = readSealedCursors(state.snap_generation, state.snap_attempt);

    const uint64_t new_generation = state.snap_generation + 1;
    /// The fold mints THIS round's attempt id from `lease.seq` (the renew/steal paths bump it every
    /// round, so it is a fresh monotonic per-round id). EVERY fold-artifact WRITE below lands under this
    /// attempt; the PARENT-generation READS keep using `state.snap_attempt` (the attempt the prior round
    /// adopted). The fold-adopt CAS #1 then commits `(new_generation, attempt)` together — a deposed
    /// leader's fold lands under its own unadopted attempt and is invisible to every reader.
    const uint64_t attempt = state.lease.seq;
    result.fold_seal.generation = new_generation;
    result.fold_seal.parent_generation = state.snap_generation;

    std::vector<BlobDelta> deltas;
    bool folded_any = false;

    /// Phase-2 token-diff: compute the per-shard discover decisions ONCE before the shard loop. A Skip
    /// means the LIST-observed shard token equals the sealed post-fence `folded_token`, so the shard body
    /// is unchanged since the prior fold and the durable state already covers it — we carry the parent
    /// coverage forward unchanged and elide the `readShard` body GET. A Read uses the existing fold path
    /// unchanged. The fence still runs globally on EVERY shard regardless — this skip elides ONLY the body
    /// read / re-fold, never the fence.
    ///
    /// The reference (post-fence tokens + fence positions) comes from the PARENT generation's completion
    /// seal (recorded by `recheck`). If no completion seal exists (mid-round: fold sealed, completion not
    /// yet advanced) we read the fold seal at the SAME adopted pair; if neither exists (fresh pool) the
    /// empty seal yields all Read.
    ///
    /// VERIFIED-SAFE (B2): the reference seal always resolves at the ADOPTED `(snap_generation,
    /// snap_attempt)` — a completed round leaves the completion seal there; a mid-round state has
    /// `snap_generation == G_f` with its fold seal under `snap_attempt`; a fresh pool has neither. The old
    /// `for g = snap_generation downto 1` back-scan was UNSOUND with a single stored `snap_attempt`: for
    /// `g < snap_generation` the adopted attempt was a DIFFERENT `lease.seq`, recorded nowhere, so
    /// `readFoldSeal(g, snap_attempt)` is the wrong key (nullopt only by accident). A direct read at the
    /// adopted pair (else empty) is conservative — fail-closed to all-Read, never an older generation.
    CasFoldSeal discover_ref_seal;
    std::map<String, uint64_t> discover_fence_positions;
    if (const auto completion = readCompletionSeal(state.snap_generation, state.snap_attempt))
    {
        discover_ref_seal.generation = state.snap_generation;
        discover_ref_seal.per_ns_shard = completion->folded_cursors;
        discover_fence_positions = completion->fence_positions;
    }
    else if (const auto fold = readFoldSeal(state.snap_generation, state.snap_attempt))
        discover_ref_seal = *fold;
    /// else: leave discover_ref_seal empty (fresh pool / no adopted seal) => all Read.
    const std::map<String, DiscoverDecision> discover_decisions =
        computeDiscoverDecisions(discover_ref_seal, discover_fence_positions);

    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const String cursor_key = cursorKey(ns, root_shard);

        /// Phase-2 token-diff skip: carry the parent coverage forward (classification 1 = Unchanged,
        /// same folded_token + folded_cursor) and skip the body GET. The default below is Read, so a
        /// missing decision or missing parent coverage falls through to the existing read path (fail
        /// closed — the spec, not a hidden fallback).
        {
            const auto dec_it = discover_decisions.find(cursor_key);
            if (dec_it != discover_decisions.end() && dec_it->second == DiscoverDecision::Skip)
            {
                const auto parent_it = parent_cursors.find(cursor_key);
                if (parent_it != parent_cursors.end())
                {
                    ShardCoverage carried = parent_it->second;
                    carried.classification = 1;   /// Unchanged: token matched persisted; body read skipped
                    result.fold_seal.per_ns_shard[cursor_key] = carried;
                    continue;
                }
                /// Skip decided but parent coverage absent (should not happen — `computeDiscoverDecisions`
                /// requires prior coverage for Skip). Fail closed: fall through to the Read path below.
            }
        }

        /// B8: reclaim ABANDONED precommits BEFORE reading the shard for the fold, so the reclaim's
        /// PrecommitRemove event is folded IN THIS SAME ROUND — its `-1` lands in this round's deltas, the
        /// fold cursor advances to cover it, and the fence_version stays consistent with the fold cursor.
        /// (Appending it AFTER sealing the cursor would leave an event below the fence but above the sealed
        /// cursor, which the round's recheck fold-through-fence AND the next round's fold would both apply
        /// — a double-counted `-1`.) The reclaim PIGGYBACKS on this per-shard visit: it reads the shard
        /// already being visited, no extra LIST / no separate enumeration stage. It judges build-death from
        /// each live `OwnerKind::Precommit` binding's `manifest_ref` (server + build_seq) via the watermark.
        reclaimAbandonedPrecommit(ns, root_shard, state.round + 1);

        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const auto cursor_it = parent_cursors.find(cursor_key);

        /// ABA-proof cursor: if the sealed incarnation differs from the live shard's incarnation
        /// (the shard was deleted and recreated at the same path), the prior cursor is stale — it
        /// was sealed against a different object. Reset to 0 so we re-fold the new shard's full
        /// journal from scratch.
        ///
        /// Within a single fold() call, no deltas for this shard have been accumulated in `deltas`
        /// at the point we detect the mismatch (the mismatch is detected before the journal loop
        /// for this shard begins). Stale source-edges already baked into the parent generation's
        /// in-degree run from prior rounds cannot be removed here — the old shard's data is gone;
        /// they will be abandoned-orphaned (unreachable source-ids with no live manifest body)
        /// and are harmless: they prevent a blob from being GC-deleted at most one extra round.
        /// The cursor reset is the ABA-correctness fix: without it, new events on the recreated
        /// shard would be skipped by the stale cursor, creating a durable in-degree under-count.
        const bool incarnation_mismatch = cursor_it != parent_cursors.end()
            && cursor_it->second.incarnation != root.incarnation;
        if (incarnation_mismatch)
            LOG_DEBUG(getLogger("CasGc"),
                "CAS GC fold: incarnation mismatch for {}/{} "
                "(sealed={{{},{}}}, live={{{},{}}}); resetting fold cursor to 0",
                ns.string(), root_shard,
                cursor_it->second.incarnation.writer_epoch,
                cursor_it->second.incarnation.build_sequence,
                root.incarnation.writer_epoch,
                root.incarnation.build_sequence);

        const uint64_t cursor = (incarnation_mismatch || cursor_it == parent_cursors.end())
            ? 0
            : cursor_it->second.folded_cursor;

        ShardCoverage cov;
        cov.folded_token = manifest_token.value_or(Token{});
        cov.classification = 0;

        bool shard_changed = false;
        /// resolved_through is the cursor we commit. Two conditions CLAMP it just below an event (never
        /// advancing past it), record an anomaly, and stop folding THIS shard — while OTHER shards still
        /// fold (GC never wedges the round on a 404; feedback_ca_gc_never_throw_on_404):
        ///   (a) a true-removal whose edge-bearing old body is missing at removal-fold (control #11);
        ///   (b) the FOLD BARRIER (control #23): a live create-precommit whose body is not yet present.
        uint64_t resolved_through = root.shard_version;
        bool clamped = false;
        auto clampBefore = [&](uint64_t at_version, const ManifestId & id, const char * what)
        {
            report.recordAnomaly(ns, root_shard, id, what);
            resolved_through = at_version > 0 ? at_version - 1 : 0;
            clamped = true;
        };

        /// ONE ordered RootOwnerEvent stream, transition_version order. Dispatch each event by comparing
        /// old_binding.manifest_ref vs new_binding.manifest_ref.
        for (const RootOwnerEvent & e : root.journal)
        {
            if (clamped)
                break;
            if (e.transition_version <= cursor || e.transition_version > root.shard_version)
                continue;

            const bool has_old = e.old_binding.has_value();
            const bool has_new = e.new_binding.has_value();
            if (has_old && has_new && e.old_binding->manifest_ref == e.new_binding->manifest_ref)
            {
                /// OWNER MOVE (promote precommit -> committed): same manifest_ref, no delta, no cleanup.
                /// The activating +1 was folded earlier — the barrier guarantees the create-precommit was
                /// activated (body present) before this move could be reached.
                shard_changed = true;
                continue;
            }

            if (has_old)
            {
                /// True removal: mirror ONLY edges actually emitted at activation. A precommit never
                /// activated (missing body) emitted none, so a missing old body simply contributes no -1.
                const ManifestId old_id{ns, e.old_binding->manifest_ref};
                const bool was_precommit = e.old_binding->owner_kind == OwnerKind::Precommit;
                if (!foldManifestEdges(old_id, -1, deltas, result.mf_cleanup))
                {
                    if (!was_precommit)
                    {
                        /// A committed owner-removal whose edge-bearing body is gone at removal-fold: the
                        /// matching -1 is unresolvable. Do NOT skip silently, do NOT emit a partial -1.
                        clampBefore(e.transition_version, old_id,
                            "owner-removal: edge-bearing committed body missing at removal-fold");
                        break;
                    }
                    /// A removed precommit whose body is absent emitted no edges — nothing to mirror.
                }
            }

            if (has_new)
            {
                const ManifestId id{ns, e.new_binding->manifest_ref};
                const bool is_precommit = e.new_binding->owner_kind == OwnerKind::Precommit;
                if (!foldManifestEdges(id, +1, deltas, result.mf_cleanup))
                {
                    if (is_precommit)
                        /// FOLD BARRIER (control #23): a live create-precommit whose body is not yet
                        /// present is NON-ACTIVATING. Clamp below it; it activates (+1) when the body
                        /// appears, or a later removal event drops it. Never throw/wedge.
                        clampBefore(e.transition_version, id,
                            "fold barrier: live precommit body not yet present (non-activating)");
                    else
                        /// A committed/promoted new-binding naming a missing body is fail-closed FOR THIS
                        /// DECISION: a committed owner is never treated as zero-edge (INV_NO_DANGLE).
                        clampBefore(e.transition_version, id,
                            "committed/promoted ref names a missing manifest body");
                    break;
                }
            }
            shard_changed = true;
        }

        cov.folded_cursor = resolved_through;
        cov.incarnation = root.incarnation;   /// stamp live incarnation; next round detects mismatch on ABA
        cov.classification = shard_changed ? 2 : 1;
        result.fold_seal.per_ns_shard[cursor_key] = cov;
        if (shard_changed)
            folded_any = true;
    }

    if (state.gc_shards == 1)
    {
        /// SINGLE-SHARD PATH (gc_shards == 1) — UNCHANGED from Phase 1d. Every blob routes to shard 0,
        /// so the entire delta stream folds into one `blobTargetRunKey(new_generation, 0, 0)` run. Task 7
        /// asserts this path reproduces Phase 1d byte-for-byte; keep it isolated and untouched.
        if (!folded_any)
        {
            /// Nothing new this round; still seal the (empty-delta) generation so the cursor coverage is
            /// durable and the resume rule has a fold_seal to key off. Reuse the prior generation's blob run
            /// (no delta) by sealing a fresh generation whose in-degree equals the parent.
            foldDeltasIntoGeneration(backend, layout, state.snap_generation, state.snap_attempt,
                                     new_generation, attempt, /*shard*/0,
                                     {}, result.fold_seal.blob_target_runs);
        }
        else
        {
            foldDeltasIntoGeneration(backend, layout, state.snap_generation, state.snap_attempt,
                                     new_generation, attempt, /*shard*/0,
                                     std::move(deltas), result.fold_seal.blob_target_runs);
        }
    }
    else
    {
        /// SHARDED PATH (gc_shards > 1) — target-sharded reducers (spec §Sharding Model). Each blob's
        /// `BlobDelta` carries its full signed edge stream; `blobShard(blob_hash, gc_shards)` partitions
        /// the stream into `gc_shards` disjoint buckets. Each bucket folds via its own `ShardReducer`
        /// into `blobTargetRunKey(new_generation, shard, 0)`. The `RootOwnerEvent`'s paired old/new
        /// bindings produced the `-1`/`+1` deltas above, so a promote that displaces a blob's owner
        /// emits BOTH the `-1` (old binding) and the `+1` (new binding) at the SAME source event. This
        /// is why cross-shard displacement needs no special handling: each delta routes independently and
        /// deterministically to whichever target shard owns its blob; the old/new pair is solved at the
        /// source, not by a cross-shard fixup.
        ///
        /// Every shard is sealed (even with an empty bucket) so the generation has a complete per-shard
        /// run set for `zeroInDegree`/`retire` consumers; an empty bucket reuses the parent in-degree.
        std::vector<std::vector<BlobDelta>> buckets(state.gc_shards);
        for (BlobDelta & d : deltas)
            buckets[blobShard(d.blob_hash, state.gc_shards)].push_back(std::move(d));

        for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
        {
            /// A reducer owns exactly one disjoint shard. Two replicas may run reducers for DIFFERENT
            /// shards concurrently (CasGcScheduler ownership); their run-key namespaces never collide.
            ShardReducer reducer{shard, state.gc_shards};
            std::vector<RunRef> shard_runs =
                reducer.reduce(backend, layout, state.snap_generation, state.snap_attempt,
                               new_generation, attempt,
                               std::move(buckets[shard]));
            for (RunRef & r : shard_runs)
                result.fold_seal.blob_target_runs.push_back(std::move(r));
        }
    }
    writePartManifestCleanupBundle(new_generation, attempt, /*owner_shard*/0, result.mf_cleanup,
                                   result.fold_seal.part_manifest_cleanup);

    /// Write-once CasFoldSeal: its existence marks fold complete. The fold seal is DETERMINISTIC (same
    /// fold inputs => byte-identical seal), so it goes through `putDeterministicArtifact`: a byte-equal
    /// occupant is our own crash/deterministic replay (adopt, no-op); divergent bytes are impossible
    /// under correct operation and fail closed with `CORRUPTED_DATA`. A deposed leader writes under its
    /// own unadopted attempt so it never collides with the adopted seal — the occupant here is only ever
    /// our own prior attempt-scoped write.
    putDeterministicArtifact(backend, layout.foldSealKey(new_generation, attempt),
                             encodeFoldSeal(result.fold_seal));

    /// Fold-adopt CAS #1: commit (snap_generation, snap_attempt) together under the lease token. After
    /// this, the whole round operates at `attempt` (== this leader's lease.seq); in-round readers
    /// (retire/recheck) and the next round's parent-generation reads resolve through `state.snap_attempt`.
    state.snap_generation = new_generation;
    state.snap_attempt = attempt;
    const CasResult fold_res = backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token);
    if (fold_res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (lease lost / another leader advanced); retry next round");
    state_token = fold_res.token;
    return result;
}

Gc::RetireResult Gc::retire(GcState & state, Token & state_token, const FoldResult & folded, RoundReport &)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    chassert(state.lease.owner == gc_id);
    const uint64_t round = state.round + 1;

    RetireResult result;
    result.mf_cleanup = folded.mf_cleanup;   /// tokens captured during fold; deferred to recheck

    /// Blob candidates: zero-in-degree in the sealed fold generation (blob-only). ONE HEAD per candidate
    /// observes the current token; an absent object is SKIPPED (a prior round's landed delete — never
    /// fabricate a token, never GET a condemned body).
    /// Scan EVERY blob-target shard's sealed run, not just shard 0: `fold`/`ShardReducer` write one run
    /// per `state.gc_shards`, so a zero-in-degree blob owned by shard 1..N would never be retired if we
    /// only scanned shard 0 (it would leak forever under `gc_shards > 1`). Each candidate is owned by the
    /// shard whose run it came from, so it goes into that shard's retired set.
    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
    {
        for (const BlobCandidate & cand : zeroInDegree(backend, layout, folded.fold_seal.generation, state.snap_attempt, shard))
        {
            /// B170: the blob's in-degree transitioned to 0 in this generation — the moment it became a
            /// retire candidate. The cause is the fold's last -1 edge (its RootRemove row above for the same
            /// object_hash), so a blob's "why did it become collectable" is a row-level join.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::IndegZero;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(cand.hash);
                e.round = round;
                e.gen = folded.fold_seal.generation;
                e.reason = "last folded owner edge dropped; in-degree reached 0";
            });

            /// DESIGN NOTE (B14 / Phase-5 retire-token; decided 2026-06-27 = keep the HEAD, "variant c").
            /// One `HEAD` per zero-in-degree candidate, here, fetches the CURRENT incarnation token that the
            /// eventual exact-token `deleteExact` will carry. This HEAD CAN be eliminated by instead sourcing
            /// the token from sealed generation state captured at fold time (the `EnableRetireTokenSource`
            /// stage of `CaGcRootLocalPartManifestCore` proves that variant safe). We deliberately keep the
            /// HEAD because:
            ///   + (this path) zero schema change; the token is always fresh, so few wasted delete attempts.
            ///   - (the alternative) the fold only sees a blob's CONTENT hash, not its storage token, so the
            ///     writer would have to record the ETag into the manifest body (`ManifestEntry`) — an on-disk
            ///     schema change that couples the content plane to storage incarnations and grows manifests;
            ///   - a fold-time token is staler than this HEAD: a re-incarnation between fold and delete makes
            ///     it stale, the exact-token delete then misses (`TokenMismatch`) and the blob is SPARED and
            ///     retried next round — safe, but it delays reclamation. So the win is only partial.
            /// SAFETY is independent of the token SOURCE: `deleteExact` is the guarantee — a stale/foreign
            /// token fails the exact match and the blob is spared, NEVER over-deleted (proved by the Phase-5
            /// model: `stage5_retiretoken` HOLDs `INV_NO_RETURN`/`INV_NO_LOSS` + `RetireTokenSourceComplete`,
            /// while `sab_staletokenoverdelete` shows that bypassing exactness violates `INV_NO_LOSS`). Concurrent
            /// writers / repeated re-incarnations never threaten safety — the model advances a blob's token
            /// freely (`WUploadBlob`, no in-degree guard) and the whole suite stays green. Revisit the
            /// stored-token variant only if profiling shows these per-candidate HEADs are a hot-path cost.
            const HeadResult observed = backend.head(blobKeyOf(layout, cand.hash));
            /// B170: HEAD-observe — the current incarnation token, the only token the eventual exact-token
            /// delete may carry (absent => skipped: a prior round's landed delete).
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcRetireObserve;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(cand.hash);
                e.token = observed.exists ? observed.token.value : "";
                e.round = round;
                e.gen = folded.fold_seal.generation;
                e.outcome = observed.exists ? "present" : "absent";
                e.reason = "zero-in-degree candidate; HEAD-observe the current token";
            });
            if (!observed.exists)
                continue;
            RetiredEntry entry;
            entry.kind = ObjectKind::Blob;
            entry.hash = cand.hash;
            entry.token = observed.token;
            entry.size = retiredLogicalSize(ObjectKind::Blob, observed.size, store->poolMeta().blob_header_len);
            /// B170: the retire entry written for this incarnation (per RetiredEntry).
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::BlobRetire;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(cand.hash);
                e.token = observed.token.value;
                e.round = round;
                e.gen = folded.fold_seal.generation;
                e.outcome = "retired";
                e.reason = "condemned zero-in-degree candidate written to the round's retired set";
                e.detail = {{"size", std::to_string(entry.size)}};
            });
            result.blobs[shard].entries.push_back(std::move(entry));
        }
    }

    /// Write each shard's retired set write-once (adopt a byte-equal occupant as our crash-replay).
    for (auto & [shard, set] : result.blobs)
    {
        const String key = layout.retiredKey(folded.fold_seal.generation, state.snap_attempt, round, shard);
        const String body = encodeRetiredSet(set);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc retire: retired set at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try { set = decodeRetiredSet(existing->bytes); }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc retire: undecodable occupant at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
    }

    /// ONE gc/state CAS advances .round — the durable "retire phase complete" marker (ViewableRound).
    GcState next = state;
    next.round = round;
    const CasResult res = backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc retire: gc/state moved during retire (another leader advanced it); retry next round");
    state = std::move(next);
    state_token = res.token;
    return result;
}

void Gc::fence(GcState & state, Token & state_token, FoldResult & folded)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    chassert(state.lease.owner == gc_id);

    const uint64_t round = state.round;

    /// Ensure this round's fence_version vector EXISTS even when the pool holds no shards (fresh pool /
    /// every namespace dropped). Previously the always-present registry fence guaranteed one entry per
    /// round; after Task 4 an empty universe would leave `fence_version[round]` unset and `recheck`
    /// would fail closed ("no fence_version recorded"). An empty (but present) vector says "the fence
    /// ran this round and covered zero shards" — the correct meaning for an empty pool.
    state.fence_version[round];

    /// Fence only the PRESENT shards (LIST-discovered, Task 4): `discoverUniverse()` returns
    /// the (ns, shard) pairs visible under `cas/refs/`. Absent shards are not fenced — a first
    /// publish into a brand-new namespace creates the shard and stamps incarnation; the shard-
    /// fence path handles the ordering from there.
    const std::vector<std::pair<RootNamespace, uint64_t>> present_shards = discoverUniverse();
    for (const auto & [ns, shard] : present_shards)
    {
        uint64_t committed = 0;
        store->mutateShard(ns, shard, [&](RootShard & root)
        {
            root.fence_round = std::max(root.fence_round, round);
        }, &committed, RootMutationOrigin::Gc, RootMutationKind::Fence);
        state.fence_version[round][cursorKey(ns, shard)] = committed;
        folded.completion_seal.fence_positions[cursorKey(ns, shard)] = committed;
    }

    /// ONE gc/state CAS persists the whole fence_version[round] vector.
    const CasResult fence_res = backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token);
    if (fence_res.outcome == CasOutcome::Committed)
    {
        state_token = fence_res.token;
        /// B170: the monotone fence committed — the durable point the recheck folds through.
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcFence;
            e.object_kind = CasEventObjectKind::Snap;
            e.round = round;
            e.gen = state.snap_generation;
            e.outcome = "fenced";
            e.reason = "R3: fenced every present shard (LIST-discovered, Task 4)";
            e.detail = {{"fence_seq", std::to_string(state.fence_seq)},
                        {"shards", std::to_string(present_shards.size())}};
        });
        return;
    }

    const auto current = backend.get(layout.gcStateKey());
    if (!current)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc fence: gc/state vanished during the fence");
    const GcState observed = decodeGcState(current->bytes);
    if (observed.lease.owner != gc_id)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fence: lease lost during fence (stolen by {}); retry next round",
            u128ToHex(observed.lease.owner));
    throw Exception(ErrorCodes::ABORTED,
        "CAS gc fence: gc/state moved during the fence while the lease is still ours; retry next round");
}

void Gc::recheck(GcState & state, Token & state_token, FoldResult & folded, const RetireResult & retired,
                 RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    chassert(state.lease.owner == gc_id);
    const uint64_t round = state.round;

    const auto fence_it = state.fence_version.find(round);
    if (fence_it == state.fence_version.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS gc recheck: no fence_version recorded for round {} — refusing to delete", round);

    /// 1. FOLD-THROUGH-FENCE (FoldedThroughFence; defends SabotageCutOverclaim #12). Re-stream every
    /// fenced shard's owner transitions in (sealed_cursor, fence_version] and emit deltas into a
    /// completion generation merged on top of the fold generation. The window starts at the (possibly
    /// fold-clamped) folded_cursor, so an unresolved anomaly the fold surfaced is NOT re-deleted here.
    /// recheck only adds spare-side +/-1, so a missing edge can only spare, never over-delete — never
    /// throw on a 404 (record-and-continue). mf_cleanup additions here are spare-side only.
    std::vector<BlobDelta> window;
    std::map<ManifestId, Token> recheck_cleanup;
    /// Phase-2 token-diff: capture each shard's POST-FENCE token (recheck runs AFTER fence in the
    /// canonical round order, so `readShard` below observes the token the fence's per-shard `mutateShard`
    /// produced). This is recorded into the completion seal's `folded_cursors[ck].folded_token` so the
    /// NEXT round's discover can compare a listed token against it and skip an unchanged shard. It is the
    /// post-fence (NOT post-trim) token by design: trim runs after recheck, so a shard trim mutates this
    /// round is conservatively Read next round (a 1-round settle); see `computeDiscoverDecisions`.
    std::map<String, Token> post_fence_tokens;
    for (const auto & [cursor_key, fence_version] : fence_it->second)
    {
        const auto [ns, shard] = parseCursorKey(cursor_key);
        const auto [root, tok] = store->readShard(ns, shard);
        if (tok.has_value())
            post_fence_tokens[cursor_key] = *tok;
        const auto cov_it = folded.fold_seal.per_ns_shard.find(cursor_key);
        const uint64_t lo = cov_it != folded.fold_seal.per_ns_shard.end() ? cov_it->second.folded_cursor : 0;
        for (const RootOwnerEvent & e : root.journal)
        {
            if (e.transition_version <= lo || e.transition_version > fence_version)
                continue;
            const bool has_old = e.old_binding.has_value();
            const bool has_new = e.new_binding.has_value();
            if (has_old && has_new && e.old_binding->manifest_ref == e.new_binding->manifest_ref)
                continue;   /// owner move: no edge change
            if (has_old)
                foldManifestEdges(ManifestId{ns, e.old_binding->manifest_ref}, -1, window, recheck_cleanup);
            if (has_new)
                foldManifestEdges(ManifestId{ns, e.new_binding->manifest_ref}, +1, window, recheck_cleanup);
        }
    }
    const uint64_t completion_generation = state.snap_generation + 1;
    /// Task 3 placeholder: recheck INHERITS the adopted attempt (never mints a new one). `snap_attempt`
    /// is 0 pre-Task-5, so reads/writes here use the same attempt as the fold/retire — no behavior change.
    const uint64_t attempt = state.snap_attempt;
    if (state.gc_shards == 1)
    {
        /// SINGLE-SHARD PATH: all deltas fold into shard 0 of the completion generation.
        foldDeltasIntoGeneration(backend, layout, state.snap_generation, attempt,
                                 completion_generation, attempt, /*shard*/0,
                                 std::move(window), folded.completion_seal.blob_target_runs);
    }
    else
    {
        /// SHARDED PATH (gc_shards > 1): scatter window deltas by blob hash, then fold each shard
        /// into its own target run (blobTargetRunKey(completion_generation, shard, 0)).
        /// Mirrors the sharded path in fold() — each shard's run is independent and keyed by shard
        /// number, so two replicas reducing disjoint shards never collide.
        std::vector<std::vector<BlobDelta>> buckets(state.gc_shards);
        for (BlobDelta & d : window)
            buckets[blobShard(d.blob_hash, state.gc_shards)].push_back(std::move(d));
        for (uint64_t sh = 0; sh < state.gc_shards; ++sh)
        {
            ShardReducer reducer{sh, state.gc_shards};
            std::vector<RunRef> shard_runs =
                reducer.reduce(backend, layout, state.snap_generation, attempt,
                               completion_generation, attempt,
                               std::move(buckets[sh]));
            for (RunRef & r : shard_runs)
                folded.completion_seal.blob_target_runs.push_back(std::move(r));
        }
    }

    /// 2. Per retired blob: spare if in-degree > 0 in the completion generation, else exact-token delete.
    std::map<uint64_t, OutcomeLog> computed;
    for (const auto & [shard, set] : retired.blobs)
    {
        for (const RetiredEntry & entry : set.entries)
        {
            OutcomeEntry outcome{.kind = entry.kind, .hash = entry.hash, .token = entry.token,
                                 .outcome = OutcomeKind::Spared};
            /// Route to the correct target shard — mirrors blobShard routing in fold() and the
            /// sharded path above. For gc_shards==1 this always produces 0 (identity).
            const uint64_t entry_shard = blobShard(entry.hash, state.gc_shards);
            const int64_t indeg_at_recheck =
                inDegreeInGeneration(backend, layout, completion_generation, attempt, entry_shard, entry.hash);
            if (indeg_at_recheck > 0)
            {
                /// B170: a fold-through-fence publish re-pinned this candidate — record the verdict +
                /// the in-degree it found, so "why wasn't it deleted" is answerable from the rows.
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    e.type = CasEventType::GcRecheckVerdict;
                    e.object_kind = CasEventObjectKind::Blob;
                    e.object_hash = u128ToHex(entry.hash);
                    e.token = entry.token.value;
                    e.round = round;
                    e.gen = completion_generation;
                    e.outcome = "spared";
                    e.reason = "in-degree > 0 after fold-through-fence; a publish re-pinned it";
                    e.detail = {{"indeg_at_recheck", std::to_string(indeg_at_recheck)},
                                {"fence_seq", std::to_string(state.fence_seq)}};
                });
            }
            else
            {
                /// ==================== THE SINGLE CONTENT-DELETE SITE (blob) ====================
                const DeleteOutcome del = backend.deleteExact(blobKeyOf(layout, entry.hash), entry.token);
                if (del.created_delete_marker)
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "CAS gc recheck: delete of blob {} created a delete marker — versioning is enabled "
                        "on the pool (mis-provisioned; the capability probe must reject this)", u128ToHex(entry.hash));
                outcome.outcome = del.kind == DeleteOutcome::Kind::Deleted ? OutcomeKind::Deleted
                                : del.kind == DeleteOutcome::Kind::NotFound ? OutcomeKind::Absent
                                : OutcomeKind::Replaced;
                const String del_outcome = outcome.outcome == OutcomeKind::Deleted ? "deleted"
                                         : outcome.outcome == OutcomeKind::Absent ? "absent" : "replaced";
                /// B170: the single content-delete site. This is the ONLY place Gc deletes a content
                /// object, so a `blob_delete` row makes any "reachable object MISSING" finding
                /// attributable. Carries the full decision context + the exact-token verdict.
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    e.type = CasEventType::BlobDelete;
                    e.object_kind = CasEventObjectKind::Blob;
                    e.object_hash = u128ToHex(entry.hash);
                    e.token = entry.token.value;
                    e.round = round;
                    e.gen = completion_generation;
                    e.outcome = del_outcome;
                    e.reason = "in-degree 0 through the fence; recheck confirmed 0; exact-token delete";
                    e.detail = {{"fence_seq", std::to_string(state.fence_seq)},
                                {"indeg_at_recheck", "0"},
                                {"token_outcome", del_outcome},
                                {"key", blobKeyOf(layout, entry.hash)}};
                });
                /// B170: the recheck verdict for the delete arm; pairs with the blob_delete above so a
                /// verdict exists for every candidate.
                EventEmitter{*store}.emit([&](CasEvent & e)
                {
                    e.type = CasEventType::GcRecheckVerdict;
                    e.object_kind = CasEventObjectKind::Blob;
                    e.object_hash = u128ToHex(entry.hash);
                    e.token = entry.token.value;
                    e.round = round;
                    e.gen = completion_generation;
                    e.outcome = del_outcome;
                    e.reason = "in-degree 0 through fold-through-fence; exact-token delete issued";
                });
            }
            computed[shard].entries.push_back(std::move(outcome));
        }
    }

    /// Write outcome logs write-once (adopt a byte-equal occupant), then tally from the FINAL logs.
    for (auto & [shard, log] : computed)
    {
        const String key = layout.outcomesKey(completion_generation, attempt, round, shard);
        const String body = encodeOutcomeLog(log);
        /// The DURABLE bytes that actually live at `key`: our own `body` on a clean win, or the adopted
        /// occupant's bytes on PreconditionFailed. The completion seal's `delete_outcomes` RunRef must
        /// checksum THESE bytes (not a fresh re-encode of `log`), so the seal references exactly what is
        /// durable — a re-encode could differ from a byte-divergent-but-decodable adopted occupant.
        String sealed_body = body;
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc recheck: outcome log at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try { log = decodeOutcomeLog(existing->bytes); }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc recheck: undecodable outcome log at {} cannot be adopted: {}", key, e.message());
                }
            }
            sealed_body = existing->bytes;
        }
        folded.completion_seal.delete_outcomes.push_back(
            RunRef{.key = key, .checksum = cityHash128(sealed_body)});
        for (const OutcomeEntry & o : log.entries)
        {
            switch (o.outcome)
            {
                case OutcomeKind::Deleted: ++report.deleted; break;
                case OutcomeKind::Absent: ++report.absent; break;
                case OutcomeKind::Replaced: ++report.replaced; break;
                case OutcomeKind::Spared: ++report.spared; break;
            }
        }
    }

    /// OUTCOME-COVERAGE GATE (CAS #2 precondition): the completion-advance below MUST NOT commit unless
    /// EVERY retired entry of this round has a recorded outcome in the FINAL (post-adopt) durable outcome
    /// logs keyed under the accepted `(completion_generation, attempt)`. By construction `computed` collects
    /// one outcome per retired entry (the per-entry loops above push exactly one, unconditionally), so
    /// coverage holds by construction in the steady state. The outcome log is OBSERVATION-BEARING (first-
    /// durable-write-wins): the adopt path may have REPLACED a shard's `log` with another observer's durable
    /// log. That winner observed the SAME accepted retired set under the SAME adopted attempt, so it MUST
    /// cover the same entries. If it does not, the durable record is incomplete and sealing the completion
    /// generation here would persist a "rechecked + done" marker over an outcome log that does not account
    /// for every retired blob — an integrity violation. FAIL-CLOSE with `CORRUPTED_DATA` BEFORE the
    /// completion seal / CAS #2 rather than completing the round on a partial durable log (a deposed-leader
    /// divergent occupant or a genuine corruption); the round retries. `chassert` is a no-op in release, so
    /// the gate is enforced by this throw, not by the assertion.
    for (const auto & [shard, set] : retired.blobs)
    {
        const OutcomeLog & log = computed[shard];
        for (const RetiredEntry & entry : set.entries)
        {
            const bool covered = std::any_of(log.entries.begin(), log.entries.end(),
                [&](const OutcomeEntry & o)
                { return o.kind == entry.kind && o.hash == entry.hash && o.token == entry.token; });
            if (!covered)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc recheck: durable outcome log for shard {} under (gen {}, attempt {}) does not "
                    "cover retired blob {} (token {}) — refusing to seal the completion generation over an "
                    "incomplete outcome log; the round will retry", shard, completion_generation, attempt,
                    u128ToHex(entry.hash), entry.token.value);
        }
    }

    /// 3. Manifest exact-token deletes — ONLY now, after the owner-removal decrements are sealed into the
    /// fold generation (control #11). recheck never READS the body to recompute decrements (they were
    /// produced at fold from the present body); it deletes by the token captured at fold. A manifest the
    /// fold-through-fence re-pinned (its owner restored) is kept.
    for (const auto & [id, token] : folded.mf_cleanup)
    {
        if (recheck_cleanup.count(id))
            continue;   /// re-removed in the fence window too: SKIP the body delete this round (the
                        /// fence-window removal's own decrements aren't sealed into this fold generation,
                        /// so a later round folds them and deletes the body then — control #11 ordering)
        const DeleteOutcome mdel = backend.deleteExact(layout.manifestKey(id), token);   /// NotFound/TokenMismatch tolerated
        /// B11: count manifest-body deletes separately from blob deletes in the round summary.
        if (mdel.kind == DeleteOutcome::Kind::Deleted)
            ++report.manifests_deleted;
        /// B170: the owner-removed manifest body delete (the tree-delete analog in the manifest model) —
        /// deleted ONLY after its blob decrements were sealed into the fold generation (control #11).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::TreeDelete;
            e.namespace_ = id.root_namespace.string();
            e.object_kind = CasEventObjectKind::Tree;
            e.object_hash = manifestRefDebugString(id.ref);
            e.token = token.value;
            e.round = round;
            e.gen = completion_generation;
            e.outcome = mdel.kind == DeleteOutcome::Kind::Deleted ? "deleted"
                      : mdel.kind == DeleteOutcome::Kind::NotFound ? "absent" : "replaced";
            e.reason = "recheck: owner-removed manifest body; exact-token delete after decrements sealed";
        });
    }

    /// 4. Seal the completion generation (write-once) + advance the pointer + drop the round's retired
    /// sets. WHICH seal exists IS the durable "rechecked + deleted + done" marker (the resume rule).
    folded.completion_seal.generation = completion_generation;
    folded.completion_seal.adoptable = true;
    /// Fence positions are taken from the DURABLE `state.fence_version[round]` (the authoritative copy
    /// persisted by `fence`'s gc/state CAS), NOT from whatever `fence` happened to populate into the
    /// in-memory `folded.completion_seal` this process. On the RESUME path `fence` is skipped (the round
    /// already fenced), so the in-memory seal's `fence_positions` would be empty — making the recomputed
    /// completion seal diverge from the original leader's and trip `putDeterministicArtifact`'s
    /// byte-equal guard. Sourcing them here from durable state makes the seal byte-identical whether
    /// recheck is reached fresh or via resume (the determinism the strict put depends on).
    folded.completion_seal.fence_positions = fence_it->second;
    /// M1: carry the round's fold cursor coverage forward into the completion seal. After this round the
    /// snap_generation pointer is the COMPLETION generation, whose fold_seal lives at the parent (fold)
    /// generation — so readFoldSeal(snap_generation) is nullopt and the next fold's sealedCursorOf would
    /// reset every per-shard cursor to 0. Persisting the coverage HERE lets the next round recover the
    /// exact per-(ns,shard) folded_cursor from the latest seal at snap_generation, with NO dependence on
    /// trim having run (a folded-but-untrimmed event must not be re-folded => no blob in-degree double-count).
    folded.completion_seal.folded_cursors = folded.fold_seal.per_ns_shard;
    /// Phase-2 token-diff: overwrite each shard's `folded_token` with the POST-FENCE token observed at
    /// recheck time (captured above). The fold-seal token is the FOLD-time token; the fence (which runs
    /// between fold and recheck) bumped it via `mutateShard`. Recording the post-fence token here is the
    /// reference the next round's `discoverDecisionsForTest`/`computeDiscoverDecisions` compares the
    /// listed token against. A shard whose token did not change since this point (no writer publish and
    /// no trim mutation) is safely skippable next round.
    for (auto & [ck, cov] : folded.completion_seal.folded_cursors)
    {
        const auto tok_it = post_fence_tokens.find(ck);
        if (tok_it != post_fence_tokens.end())
            cov.folded_token = tok_it->second;
    }
    /// The completion seal is DETERMINISTIC (same recheck inputs => byte-identical seal); route it through
    /// `putDeterministicArtifact` (byte-equal replay adopts, divergent bytes => `CORRUPTED_DATA`). The
    /// retired set and outcome log above stay observation-bearing (first-durable-write-wins) — they carry
    /// HEAD-observed tokens two observers may legitimately differ on, so they must NOT go through here.
    putDeterministicArtifact(backend, layout.completionSealKey(completion_generation, attempt),
                             encodeCompletionSeal(folded.completion_seal));

    GcState next = state;
    next.snap_generation = completion_generation;
    std::erase_if(next.fence_version, [&](const auto & kv) { return kv.first <= round; });

    /// B9: prune superseded generations. The fold/recheck read ONLY the latest seal at snap_generation
    /// (M1) and its blob-target runs, so any generation below the retention floor is unreferenced. A
    /// leader more than `keep` generations behind has lost its lease (its round-commit CAS fails), so
    /// keeping `keep` generations covers any in-flight/resuming leader. Walk forward from the durable
    /// cursor, bounded per round; fold snap_pruned_through into the SAME gc/state CAS below. If a stale
    /// leader pruned and then loses the CAS, the deletes were still below the winner's even-higher floor.
    pruneSupersededGenerations(completion_generation, attempt, next);

    const CasResult res = backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc recheck: gc/state moved during the recheck persist (another leader advanced it); retry");
    state = std::move(next);
    state_token = res.token;

    /// Drop the round's retired-set objects on their confirmed outcomes (a GC-metadata delete).
    for (const auto & [shard, set] : retired.blobs)
    {
        const String key = layout.retiredKey(folded.fold_seal.generation, attempt, round, shard);
        if (const auto got = backend.get(key))
        {
            const DeleteOutcome dropped = backend.deleteExact(key, got->token);
            if (dropped.kind == DeleteOutcome::Kind::TokenMismatch)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS gc recheck: retired set at {} changed under us (token mismatch on drop)", key);
        }
    }
}

void Gc::runManifestSweepCursorPass(GcState & state, Token & state_token)
{
    const uint64_t list_budget = store->poolConfig().manifest_sweep_list_budget_keys;
    if (list_budget == 0)
        return;

    const uint64_t delete_budget = store->poolConfig().manifest_sweep_delete_budget_keys;
    const ManifestSweepResult result = sweepManifestCursorPage(
        *store, state.manifest_sweep_cursor, list_budget, delete_budget);

    if (result.next_cursor == state.manifest_sweep_cursor)
        return;

    GcState next = state;
    next.manifest_sweep_cursor = result.next_cursor;
    const CasResult res = store->backend().casPut(store->layout().gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome == CasOutcome::Committed)
    {
        state = std::move(next);
        state_token = res.token;
        return;
    }

    LOG_DEBUG(getLogger("CasGc"),
        "CAS gc orphan sweep cursor progress discarded because gc/state moved (listed {}, deleted {}, skipped {})",
        result.listed, result.deleted, result.skipped);
}

void Gc::trim(FoldResult & folded, uint64_t /*round*/)
{
    const uint64_t trim_min_events = store->poolConfig().gc_trim_min_events;
    const uint64_t trim_body_soft_limit = store->poolConfig().gc_trim_body_soft_limit;

    /// B12: consume (and reset) the maintenance-trim flag exactly once per round.
    const bool is_maintenance = maintenance_trim;
    maintenance_trim = false;

    for (const auto & [ns, shard] : folded.root_shards)
    {
        const String cursor_key = cursorKey(ns, shard);
        /// INV-JOURNAL-COVERAGE: source the trim cursor exclusively from the sealed CasFoldSeal
        /// per-shard coverage. A shard absent from the sealed coverage has no provably-folded
        /// records yet — trim nothing for it (no fallback to a looser cursor, per the invariant).
        const auto cov_it = folded.fold_seal.per_ns_shard.find(cursor_key);
        if (cov_it == folded.fold_seal.per_ns_shard.end())
            continue;
        const uint64_t cursor = cov_it->second.folded_cursor;
        if (cursor == 0)
            continue;

        /// Peek: read the shard once to inspect the journal and encoded body size.
        const auto [peek, tok] = store->readShard(ns, shard);

        /// Count events at/below the sealed cursor (these are the trimmable ones).
        uint64_t trimmable_count = 0;
        for (const RootOwnerEvent & e : peek.journal)
            if (e.transition_version <= cursor)
                ++trimmable_count;

        if (trimmable_count == 0)
            continue;   /// nothing to trim — no pointless version bump (same as before)

        /// B12 lazy-trim gate: compact only when a threshold is met.
        ///   (a) event-count batch gate: trimmable_count >= gc_trim_min_events (0 = eager, always compact).
        ///   (b) body soft-limit gate: the encoded shard body is at/above gc_trim_body_soft_limit.
        ///   (c) maintenance mode: explicit one-round full-compaction bypass.
        /// Gate (b) guarantees bounded journal growth even when (a) never fires (unbounded build-up
        /// is impossible once the encoded body exceeds the soft limit — a hard cap on inert events).
        const bool count_gate = (trim_min_events == 0) || (trimmable_count >= trim_min_events);
        const String encoded_peek = encodeRootShard(peek);
        const bool size_gate = (trim_body_soft_limit > 0) && (encoded_peek.size() >= trim_body_soft_limit);
        if (!count_gate && !size_gate && !is_maintenance)
            continue;   /// B12: skip this shard — inert events; token stays stable for the next discover Skip

        /// Determine the trigger reason for the B170 audit log.
        const char * trim_reason = is_maintenance ? "maintenance"
                                 : size_gate       ? "soft-limit"
                                                   : "threshold";

        store->mutateShard(ns, shard, [&](RootShard & fresh)
        {
            std::erase_if(fresh.journal,
                [&](const RootOwnerEvent & e) { return e.transition_version <= cursor; });
        }, nullptr, RootMutationOrigin::Gc, RootMutationKind::Trim);
        /// Record the sealed cursor used for this shard's trim into the in-round completion seal
        /// audit log. The completion seal is already written write-once before trim runs, so this
        /// populates the in-memory context only — it is an audit annotation, not a decision input.
        folded.completion_seal.trim_cursors[cursor_key] = cursor;
        /// B170: the journal trim for this shard (events provably folded into the durable generation).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcTrim;
            e.namespace_ = ns.string();
            e.object_kind = CasEventObjectKind::Root;
            e.outcome = "trimmed";
            e.reason = "INV-JOURNAL-COVERAGE: trimmed owner events at or below the sealed fold cursor";
            e.detail = {{"shard", std::to_string(shard)},
                        {"sealed_fold_cursor", std::to_string(cursor)},
                        {"trimmed_count", std::to_string(trimmable_count)},
                        {"trim_trigger", trim_reason}};
        });
    }
}

void Gc::writePartManifestCleanupBundle(uint64_t generation, uint64_t attempt, uint64_t owner_shard,
                                        const std::map<ManifestId, Token> & cleanup, std::vector<RunRef> & out)
{
    if (cleanup.empty())
        return;
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    /// One write-once run: key = manifestKey, payload = the token to exact-token-delete with. The map is
    /// ordered by ManifestId; manifestKey is monotone in that ordering for a fixed namespace, but across
    /// namespaces it is not, so sort the produced rows by key for a byte-reproducible run (OQ5).
    std::vector<std::pair<String, String>> rows;
    rows.reserve(cleanup.size());
    for (const auto & [id, token] : cleanup)
        rows.emplace_back(layout.manifestKey(id), token.value);
    std::sort(rows.begin(), rows.end(), [](const auto & a, const auto & b) { return a.first < b.first; });

    DB::WriteBufferFromOwnString buf;
    RunHeader header;
    header.kind = RunKind::ManifestEntries;
    header.key_schema = 1;
    RunFileWriter writer(buf, header);
    for (const auto & [key, value] : rows)
        writer.append(key, value);
    writer.finish();
    const String run_bytes = buf.str();

    const String run_key = layout.partManifestCleanupKey(generation, attempt, owner_shard, 0);
    /// The cleanup bundle is a DETERMINISTIC artifact (same cleanup map => byte-reproducible run, OQ5), in
    /// the same artifact class as the in-degree runs and the fold/completion seals: a byte-equal occupant
    /// is our own deterministic replay (adopt, no-op) and divergent bytes fail closed with CORRUPTED_DATA
    /// rather than letting a divergent bundle disagree with the adopted snapshot.
    putDeterministicArtifact(backend, run_key, run_bytes);
    out.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes)});
}

namespace
{
/// GC-metadata wholesale delete of every object under `prefix`. Returns the number of objects deleted.
/// `bounded_remaining` caps how many objects this call may delete (0 => stop immediately, deleting none).
///
/// Token source: the in-memory and S3 backends surface a per-key token through `list`
/// (`supportsListTokens()`), so `deleteExact` straight from the listed token; otherwise HEAD first.
///
/// 404 / NotFound is FAIL-OPEN: an object that vanished between LIST and delete (a concurrent crashed
/// attempt, or a racing prune) is already reclaimed — never throw on a benign missing GC-internal object
/// during a prune (it would only wedge GC; feedback_ca_gc_never_throw_on_404). A genuine TokenMismatch is
/// likewise tolerated here: the object was rewritten under us (another attempt is live at this key) — the
/// safe direction during a best-effort prune is to leave it for a later round, never to force-delete.
uint64_t deletePrefixWholesale(Backend & backend, const String & prefix, uint64_t bounded_remaining)
{
    static constexpr size_t kListPageLimit = 1000;
    uint64_t deleted = 0;
    String cursor;
    while (deleted < bounded_remaining)
    {
        ListPage page = backend.list(prefix, cursor, kListPageLimit);
        for (const auto & listed : page.keys)
        {
            if (deleted >= bounded_remaining)
                return deleted;
            if (listed.token.has_value())
            {
                /// deleteExact tolerates NotFound (returns Kind::NotFound) and TokenMismatch — both are
                /// benign here (already gone / rewritten by a live attempt); do not throw.
                backend.deleteExact(listed.key, *listed.token);
            }
            else if (const auto head = backend.head(listed.key); head.exists)
            {
                backend.deleteExact(listed.key, head.token);
            }
            ++deleted;
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return deleted;
}
}

void Gc::pruneSupersededGenerations(uint64_t adopted_generation, uint64_t attempt, GcState & next)
{
    const uint64_t keep = store->poolConfig().gc_snap_generations_to_keep;
    if (keep == 0)
        return;   /// keep ALL (debug/forensics — replay GC's in-degree view as-of a past round)

    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    static constexpr uint64_t kMaxPrunePerRound = 64;   /// bound the per-round prune burst

    /// (1) WHOLESALE generation-retention (correctness). A single generation may hold artifacts under
    /// MULTIPLE attempts: every round mints a fresh `lease.seq` (= attempt), and a deposed leader writes
    /// its fold_seal/runs/cleanup AND its attempt-scoped retired/outcomes sets under its OWN unadopted
    /// attempt before its CAS fails. The old per-key single-attempt prune (keyed on the final
    /// snap_attempt) therefore leaked every non-adopted attempt's debris. Instead, LIST the whole
    /// `gc/gen/<g>/` prefix and delete every listed object — reclaiming ALL attempts of `g`, including
    /// the retired/ and outcomes/ sets that now live under `gc/gen/<g>/attempt/<a>/`. Bounded per round;
    /// fail-open on 404. snap_pruned_through only advances over generations fully reclaimed this round.
    if (adopted_generation > keep)
    {
        const uint64_t prune_floor = adopted_generation - keep;   /// prune generations <= prune_floor
        uint64_t g = next.snap_pruned_through + 1;
        uint64_t pruned = 0;
        for (; g <= prune_floor && pruned < kMaxPrunePerRound; ++g, ++pruned)
            deletePrefixWholesale(backend, layout.gcGenPrefix(g), std::numeric_limits<uint64_t>::max());
        next.snap_pruned_through = g - 1;   /// highest generation fully processed this round
    }

    /// (2) NO per-round current-generation attempt-sweep (KISS). A previous revision LISTed the FOLD
    /// generation's `gc/gen/<G_f>/` prefix EVERY completed round to delete non-adopted attempts with
    /// `a < snap_attempt` — debris a deposed leader of the just-completed round left under its own
    /// (unadopted) `lease.seq`. That per-round LIST was steady-state S3 budget spent for the RARE case
    /// of a concurrent-leader collision (the GC-DISCOVERY-LIST-QUADRATIC concern), so it is removed.
    ///
    /// The wholesale generation-retention prune in (1) is now the SOLE reclaimer of ALL attempt debris,
    /// including a deposed leader's: every artifact of generation `g` — across every attempt — lives
    /// under `gc/gen/<g>/`, and the prefix-delete in (1) reclaims the whole subtree once `g` ages past
    /// `keep`. Deposed-leader current-generation debris is therefore BOUNDED space (one collision leaves
    /// at most a handful of small objects per generation) that waits at most `keep` completion-advances
    /// to be reclaimed. This trades ~`keep` rounds of reclaim latency on (rare) concurrent-leader
    /// collisions for eliminating a per-round LIST on the common (single-leader) path. When `keep == 0`
    /// (keep-all / forensics mode) nothing is reclaimed by design — same as before.
    (void)attempt;
}

std::optional<CasFoldSeal> Gc::readFoldSeal(uint64_t generation, uint64_t attempt)
{
    if (const auto got = store->backend().get(store->layout().foldSealKey(generation, attempt)))
        return decodeFoldSeal(got->bytes);
    return std::nullopt;
}

std::optional<CasCompletionSeal> Gc::readCompletionSeal(uint64_t generation, uint64_t attempt)
{
    if (const auto got = store->backend().get(store->layout().completionSealKey(generation, attempt)))
        return decodeCompletionSeal(got->bytes);
    return std::nullopt;
}

std::map<String, ShardCoverage> Gc::readSealedCursors(uint64_t generation, uint64_t attempt)
{
    /// M1: the per-(ns,shard) fold cursor coverage as of `generation`, from the LATEST seal there: the
    /// completion seal if the round that produced this generation finished (it overwrote snap_generation
    /// with its completion generation and carried the cursors into folded_cursors), else the fold seal
    /// (a mid-round fold sealed but not yet completed). Absent both => empty (fresh pool, cursor 0).
    if (const auto completion = readCompletionSeal(generation, attempt))
        return completion->folded_cursors;
    if (const auto fold = readFoldSeal(generation, attempt))
        return fold->per_ns_shard;
    return {};
}

std::vector<std::pair<RootNamespace, uint64_t>> Gc::discoverUniverse()
{
    /// LIST-based namespace discovery (Task 4): the discovery authority rests on LIST(cas/refs/).
    /// Consistency requirement: the backend must give read-your-writes LIST enumeration.
    /// InMemoryBackend: guaranteed (in-memory map). S3: strongly consistent since 2021.
    /// RustFS: to confirm in soak.
    std::vector<std::pair<RootNamespace, uint64_t>> universe;
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    const String prefix = layout.casRefsPrefix();
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(prefix, cursor, /*limit=*/1000);
        for (const ListedKey & lk : page.keys)
        {
            if (!lk.key.starts_with(prefix))
                continue;
            const std::string_view rest(lk.key.data() + prefix.size(), lk.key.size() - prefix.size());
            const size_t slash = rest.rfind('/');
            if (slash == std::string_view::npos || slash + 1 == rest.size())
                continue;
            const std::string_view shard_sv = rest.substr(slash + 1);
            uint64_t shard = 0;
            bool valid = !shard_sv.empty();
            for (const char c : shard_sv)
            {
                if (c < '0' || c > '9')
                {
                    valid = false;
                    break;
                }
                shard = shard * 10 + static_cast<uint64_t>(c - '0');
            }
            if (!valid)
                continue;
            universe.emplace_back(RootNamespace{String(rest.substr(0, slash))}, shard);
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return universe;
}

std::map<String, Token> Gc::listRootShardTokens(std::set<String> & ambiguous_keys)
{
    std::map<String, Token> result;
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    const String prefix = layout.casRefsPrefix();
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(prefix, cursor, /*limit=*/1000);
        for (const ListedKey & lk : page.keys)
        {
            if (!lk.token.has_value())
                continue;
            /// AMBIGUITY DETECTION: a key observed more than once across pages is ambiguous (a backend
            /// anomaly or a racing write that landed between pages). Ambiguous keys are forced Read in
            /// `computeDiscoverDecisions` — a shard we cannot unambiguously identify must never be
            /// skipped (fail closed; the spec, not a hidden fallback).
            if (result.contains(lk.key))
                ambiguous_keys.insert(lk.key);
            else
                result[lk.key] = *lk.token;
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return result;
}

std::map<String, Gc::DiscoverDecision> Gc::computeDiscoverDecisions(
    const CasFoldSeal & sealed,
    const std::map<String, uint64_t> & fence_positions)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    /// Universe is the LIST-discovered present-shard set (Task 4: `discoverUniverse` LISTs `cas/refs/`).
    /// The token sweep below is only an accelerator on top of it — it decides Read-vs-Skip per shard, never
    /// removes a shard from the set. Default decision is Read (fail closed; the spec, not a fallback).
    std::map<String, DiscoverDecision> decisions;
    const std::vector<std::pair<RootNamespace, uint64_t>> universe = discoverUniverse();
    for (const auto & [ns, shard] : universe)
        decisions[cursorKey(ns, shard)] = DiscoverDecision::Read;

    if (!backend.supportsListTokens())
        /// No token information available from LIST — every shard must be read (already defaulted to Read).
        return decisions;

    /// ONE LIST sweep — the accelerator. Map the full backend keys to cursor-key form ("ns/shard") by
    /// stripping the refs prefix (`rootShardKey` == casRefsPrefix() + "ns/shard"), so they line up with
    /// `per_ns_shard`. After the Phase 1 relocation, `cas/refs/` holds ONLY ref shards (the manifest
    /// backlog and verbatim files stay under `roots/`), so no non-shard key is even listed here.
    ///
    /// AMBIGUITY: `listRootShardTokens` also reports keys it observed MORE THAN ONCE across all pages.
    /// An ambiguous full key is mapped to cursor-key form and added to `ambiguous_cursor_keys` so the
    /// Skip guard below can force those shards to Read (fail closed; an unambiguous listed token is
    /// required for Skip — the spec, not a hidden fallback).
    std::set<String> ambiguous_full_keys;
    const std::map<String, Token> listed = listRootShardTokens(ambiguous_full_keys);
    const String refs_prefix = layout.casRefsPrefix();
    std::map<String, Token> listed_by_cursor_key;
    std::set<String> ambiguous_cursor_keys;
    for (const auto & [full_key, token] : listed)
    {
        if (full_key.starts_with(refs_prefix))
            listed_by_cursor_key[full_key.substr(refs_prefix.size())] = token;
    }
    for (const auto & full_key : ambiguous_full_keys)
    {
        if (full_key.starts_with(refs_prefix))
            ambiguous_cursor_keys.insert(full_key.substr(refs_prefix.size()));
    }

    for (const auto & [ns, shard] : universe)
    {
        const String ck = cursorKey(ns, shard);

        const auto sealed_it = sealed.per_ns_shard.find(ck);
        if (sealed_it == sealed.per_ns_shard.end())
            continue;   /// no prior coverage (new shard since last round) => Read (already defaulted)

        /// CLAMP guard: if the previous fold was barrier-clamped, the cursor sits below the fence-time
        /// shard_version and unfolded events may exist (or may become foldable). `fence_position` is the
        /// shard_version AFTER fence (+1 over the fold position), so fully-folded => cursor == fence_pos - 1
        /// and clamped => cursor + 1 < fence_pos. A clamped shard is forced Read.
        const auto fence_it = fence_positions.find(ck);
        if (fence_it != fence_positions.end() && fence_it->second > 0)
        {
            const uint64_t fence_pos = fence_it->second;
            if (sealed_it->second.folded_cursor + 1 < fence_pos)
                continue;   /// clamped => Read (already defaulted)
        }

        const auto listed_it = listed_by_cursor_key.find(ck);
        if (listed_it == listed_by_cursor_key.end())
            continue;   /// shard not visible in LIST (absent / key format differs) => Read (defaulted)

        /// AMBIGUITY guard: a shard key seen more than once in the LIST sweep is ambiguous — we cannot
        /// unambiguously identify its current token. Forced Read (fail closed; the spec, not a fallback).
        if (ambiguous_cursor_keys.contains(ck))
            continue;   /// ambiguous listed key => Read (already defaulted)

        /// Skip IFF the listed token equals the sealed post-fence `folded_token` exactly.
        /// INCARNATION EQUALITY: `RootShard::incarnation` is serialised into the shard body, so
        /// token equality implies incarnation equality — a recreated shard produces different bytes
        /// and therefore a different token. The spec's incarnation-equality conjunct is therefore
        /// subsumed by the token check here. For the rare backend that issues non-content-based
        /// tokens, `fold()`'s Read path detects the mismatch after `readShard` and resets the
        /// cursor there (a one-round lag, bounded and safe because the cursor undercount is caught
        /// before any delete decision — see the ABA-proof cursor comment in `fold()`).
        if (listed_it->second == sealed_it->second.folded_token)
            decisions[ck] = DiscoverDecision::Skip;
    }

    return decisions;
}

std::map<String, Gc::DiscoverDecision> Gc::discoverDecisionsForTest()
{
    /// WRITE-FREE: load the reference tokens from durable state and return the decisions the next round's
    /// discover would make. No CAS, no delete, no fold.
    const auto state_bytes = store->backend().get(store->layout().gcStateKey());
    if (!state_bytes)
        /// Fresh pool: no sealed state. `computeDiscoverDecisions` over an empty seal yields all Read.
        return computeDiscoverDecisions(CasFoldSeal{}, {});

    const GcState state = decodeGcState(state_bytes->bytes);

    /// Prefer the parent generation's completion seal — its `folded_cursors` carry the post-fence tokens
    /// (recorded by `recheck`) that the next round's LIST compares against, plus `fence_positions` for the
    /// clamp guard. Fall back to the latest fold seal for a mid-round state (a Skip cannot fire there:
    /// the fold seal carries fold-time tokens and no fence positions, so the token comparison fails Read).
    /// Mirror `fold`'s reference-seal resolution (B2): read directly at the adopted `(snap_generation,
    /// snap_attempt)` — completion seal else fold seal else empty. No back-scan: a prior generation's
    /// adopted attempt was a different `lease.seq`, unreachable via the single stored `snap_attempt`.
    CasFoldSeal ref_seal;
    std::map<String, uint64_t> fence_positions;
    if (const auto completion = readCompletionSeal(state.snap_generation, state.snap_attempt))
    {
        ref_seal.generation = state.snap_generation;
        ref_seal.per_ns_shard = completion->folded_cursors;
        fence_positions = completion->fence_positions;
    }
    else if (const auto fold = readFoldSeal(state.snap_generation, state.snap_attempt))
        ref_seal = *fold;
    /// else: empty ref_seal (fresh pool / no adopted seal) => all Read.

    return computeDiscoverDecisions(ref_seal, fence_positions);
}

std::vector<Gc::PreviewEntry> Gc::previewDeletes()
{
    std::vector<PreviewEntry> out;

    const auto state_bytes = store->backend().get(store->layout().gcStateKey());
    if (!state_bytes)
        return out;
    const GcState state = decodeGcState(state_bytes->bytes);

    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    /// Scan every blob-target shard (see `retire`): a preview that only looked at shard 0 would miss the
    /// zero-in-degree candidates owned by shards 1..N under `gc_shards > 1`.
    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
    {
        for (const BlobCandidate & cand : zeroInDegree(backend, layout, state.snap_generation, state.snap_attempt, shard))
        {
            const HeadResult observed = backend.head(blobKeyOf(layout, cand.hash));
            if (!observed.exists)
                continue;
            PreviewEntry e;
            e.kind = ObjectKind::Blob;
            e.hash = cand.hash;
            e.key = blobKeyOf(layout, cand.hash);
            e.size = observed.size;
            e.reason = "unreachable";
            out.push_back(std::move(e));
        }
    }
    return out;
}

bool Gc::tryResumeIncompleteRound(GcState & state, Token & state_token, RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    const uint64_t round = state.round;
    if (round == 0)
        return false;

    /// Detect an incomplete round from durable state: retired sets still present at
    /// (snap_generation, snap_attempt, round). They drop only at the very end of a completed round
    /// (recheck step 4), so their presence is the
    /// durable incompleteness signal. The fold for this round already committed (retire's CAS advanced
    /// .round AFTER the fold's CAS), so a fold_seal exists for the current generation.
    RetireResult retired;
    for (uint64_t shard = 0; shard < state.gc_shards; ++shard)
        if (const auto got = backend.get(layout.retiredKey(state.snap_generation, state.snap_attempt, round, shard)))
            retired.blobs.emplace(shard, decodeRetiredSet(got->bytes));
    if (retired.blobs.empty())
        return false;

    report.round = round;
    for (const auto & [shard, set] : retired.blobs)
        report.candidates += set.entries.size();

    /// Reconstruct the FoldResult tail from the durable fold seal of the current generation. The seal
    /// carries the per-shard cursors the recheck folds through; root_shards is re-discovered; the
    /// part-manifest cleanup is re-read from the durable bundle so the manifest deletes re-issue (recheck
    /// deletes by the bundle's token, never the deleted body).
    FoldResult folded;
    if (const auto seal = readFoldSeal(state.snap_generation, state.snap_attempt))
        folded.fold_seal = *seal;
    else
        /// No fold seal but retired sets present is out-of-model; fail closed rather than guess.
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc resume: retired sets present at round {} but no fold seal for generation {}",
            round, state.snap_generation);
    folded.root_shards = discoverUniverse();

    /// Re-read the cleanup bundle (manifestKey -> token) so recheck re-issues the deferred deletes.
    for (uint64_t seq = 0; ; ++seq)
    {
        const auto got = backend.get(layout.partManifestCleanupKey(state.snap_generation, state.snap_attempt, /*owner_shard*/0, seq));
        if (!got)
            break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        String key;
        String value;
        while (r.next(key, value))
        {
            /// The bundle stores the object key directly; recheck issues deleteExact on it. We cannot
            /// reconstruct the ManifestId from the key cheaply, so resume issues the deletes here
            /// directly (idempotent: NotFound/TokenMismatch tolerated), keeping the recheck path simple.
            backend.deleteExact(key, Token{value});
        }
    }

    /// Re-fence when the durable state lacks the round's vector (a crash between retire and the fence's
    /// gc/state CAS). The monotone max makes re-fencing idempotent; higher replayed versions are more
    /// conservative coverage.
    if (!state.fence_version.contains(round))
        fence(state, state_token, folded);

    /// Re-run recheck (deletes are exact-token idempotent) and trim. recheck folds the fence window into
    /// a completion generation, deletes/spares the durable retired blobs, seals completion, drops the sets.
    recheck(state, state_token, folded, retired, report);
    if (trim_enabled)
        trim(folded, state.round);
    /// NOTE: `reclaimDroppedShards` is deliberately NOT called here. Shard reclaim is best-effort —
    /// skipping it on a crash-resume is safe because it will be retried in the next full regular round.
    /// Calling it here would require re-reading shards already processed, adding complexity with no
    /// correctness benefit.
    return true;
}

void Gc::rememberObservation(const GcLease & lease)
{
    has_observation = true;
    last_seen_owner = lease.owner;
    last_seen_seq = lease.seq;
}

void Gc::pulseHeartbeat(Store & store, UInt128 gc_id)
{
    const String key = store.layout().gcHbKey();
    const auto got = store.backend().get(key);
    GcHeartbeat hb;
    std::optional<Token> expected;
    if (got)
    {
        hb = decodeGcHeartbeat(got->bytes);
        expected = got->token;
    }
    hb.owner = gc_id;
    ++hb.hb_seq;
    store.backend().casPut(key, encodeGcHeartbeat(hb), expected);
}

namespace
{

std::optional<ServerWatermark> watermarkForNamespace(Store & store, const RootNamespace & ns)
{
    /// A namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain
    /// slashes. Try namespace prefixes from longest to shortest and accept the first durable watermark.
    const String & value = ns.string();
    size_t pos = value.size();
    while (true)
    {
        pos = value.rfind('/', pos == 0 ? 0 : pos - 1);
        if (pos == String::npos)
            break;

        const String server_root_id = value.substr(0, pos);
        if (!server_root_id.empty())
        {
            if (const auto got = store.backend().get(store.layout().serverRootWatermarkKey(server_root_id)))
                return decodeServerWatermark(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}

}

void Gc::reclaimAbandonedPrecommit(const RootNamespace & ns, uint64_t shard, uint64_t /*round*/)
{
    /// B8: reclaim ABANDONED precommits in the CONVERGED-SHARD model. `precommitAdd` writes a precommit
    /// owner binding into the FUTURE COMMITTED REF's OWN table shard (keyed by `final_ref_name`, kind
    /// `OwnerKind::Precommit`) — there is no `_precommits` namespace. So while the fold visits a shard, this
    /// enumerates the LIVE precommit bindings from the journal owner-state replay (the same
    /// accumulate-adds/subtract-removals the fold and `Build::promote` do), then judges build-death via
    /// the namespace's per-server-root watermark (identical to the orphan sweep). It runs BEFORE the fold reads this shard so its
    /// PrecommitRemove is folded in the SAME round (no double-counted `-1`).

    /// Read the shard being visited (the fold re-reads it right after, picking up any removal we append).
    const RootShard root = store->readShard(ns, shard).first;

    /// Owner-state replay: a precommit `new_binding` not later removed (or moved off its manifest_ref) is
    /// LIVE. Keep the FULL binding so the removal event names EXACTLY it (the encoding `Build::promote`
    /// re-proves against; a partial binding would not match and the fold/promote guards would mis-judge).
    std::vector<OwnerBinding> live;
    for (const RootOwnerEvent & e : root.journal)
    {
        if (e.old_binding)
            std::erase(live, *e.old_binding);
        if (e.new_binding)
            live.push_back(*e.new_binding);
    }

    const auto watermark = watermarkForNamespace(*store, ns);
    if (!watermark)
        return;   /// no durable fact => not dead (conservative)
    const ServerWatermark & w = *watermark;

    struct DeadPrecommit { OwnerBinding binding; bool retired_sentinel; };
    std::vector<DeadPrecommit> dead;
    for (const OwnerBinding & binding : live)
    {
        if (binding.owner_kind != OwnerKind::Precommit)
            continue;

        /// CONSERVATIVE death judgment, identical to the orphan sweep's `prefixEligible` — a DURABLE
        /// watermark FACT only, never a frozen-seq / judged-dead guess (control #9). A build is provably
        /// DEAD iff the server has a watermark AND either the farewell/retired sentinel
        /// (`min_active == UINT64_MAX`, every seq retired), its `writer_epoch` is older than the live
        /// watermark epoch, or its `build_sequence` is below the live floor (`min_active > build_sequence`).
        /// A missing watermark, a future epoch, or a build at/above the floor is NOT dead — the precommit
        /// is spared. The K=2 frozen-seq crash detector is deliberately NOT consulted here:
        /// it is a liveness heuristic, unsafe as a reclaim trigger (a live but slow-renewing build must
        /// never have its in-flight precommit reclaimed). A wrongful reclaim would still be caught by the
        /// promote guard (fail closed), but conservatism keeps live builds from being needlessly aborted.
        if (binding.manifest_ref.writer_epoch > w.epoch)
            continue;
        const bool retired_sentinel = w.min_active == std::numeric_limits<uint64_t>::max();
        const bool is_dead = binding.manifest_ref.writer_epoch < w.epoch
            || retired_sentinel
            || w.min_active > binding.manifest_ref.build_sequence;
        if (is_dead)
            dead.push_back(DeadPrecommit{binding, retired_sentinel});
    }

    if (dead.empty())
        return;

    /// Reclaim: append a removal RootOwnerEvent per dead precommit on the SAME shard (old = the precommit
    /// binding, new = none — the encoding shared by `Build::abandon` / `Store::dropRef`). The fold (which
    /// re-reads this shard immediately after) then folds the removal IN THIS ROUND, releasing the edges so
    /// the closure's blobs become zero-in-degree candidates. ONE mutateShard CAS removes all dead bindings;
    /// each gets a distinct, contiguous transition_version above the shard's current version (mutateShard
    /// bumps shard_version by one, so the LAST appended event aligns with the committed shard_version).
    /// `mutateShard` does a single `++shard_version` AFTER this callback, so the LAST event we append must
    /// carry `shard_version + dead.size()` and we pre-advance shard_version to one below that; the trailing
    /// `++` lands it exactly on the last event's transition_version (no gap, contiguous versions).
    store->mutateShard(ns, shard, [&](RootShard & fresh)
    {
        const uint64_t base = fresh.shard_version;
        for (size_t i = 0; i < dead.size(); ++i)
            fresh.journal.push_back(RootOwnerEvent{
                .transition_version = base + i + 1,
                .old_binding = dead[i].binding,
                .new_binding = std::nullopt});
        fresh.shard_version = base + dead.size() - 1;
    }, nullptr, RootMutationOrigin::Gc, RootMutationKind::ReclaimPrecommit);

    /// B170: each abandoned precommit reclaimed (its owner edge removed; the next fold releases its blob
    /// edges). Records WHY the build was judged dead — the soak's leak/dangle attribution.
    for (const DeadPrecommit & dp : dead)
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::PrecommitReclaim;
            e.namespace_ = ns.string();
            e.ref_name = dp.binding.ref_name;
            e.object_kind = CasEventObjectKind::Root;
            e.outcome = "reclaimed";
            e.reason = dp.retired_sentinel
                ? "precommit reclaim: owning server posted the farewell/retired sentinel (build gone)"
                : "precommit reclaim: build_seq below the server's min_active floor (retired build)";
            e.detail = {{"writer_epoch", std::to_string(dp.binding.manifest_ref.writer_epoch)},
                        {"build_seq", std::to_string(dp.binding.manifest_ref.build_sequence)}};
        });
}

bool Gc::acquireOrRenewLease(GcState & state, Token & state_token)
{
    const String key = store->layout().gcStateKey();

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto got = store->backend().get(key);

        if (!got)
        {
            if (has_observation)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state vanished after being observed (owner {}, seq {})",
                    u128ToHex(last_seen_owner), last_seen_seq);

            GcState fresh;
            fresh.lease = GcLease{gc_id, 1};
            /// Creation-time only: gc_shards is set ONCE on first-ever acquire; subsequent rounds read
            /// the authoritative value from the persisted GcState (pool is authoritative on reopen,
            /// like root_shards). PoolConfig carries the configured value from the disk XML.
            fresh.gc_shards = store->poolConfig().gc_shards;
            const CasResult acquire_res = store->backend().casPut(key, encodeGcState(fresh), std::nullopt);
            if (acquire_res.outcome == CasOutcome::Committed)
            {
                rememberObservation(fresh.lease);
                state = std::move(fresh);
                state_token = acquire_res.token;
                return true;
            }
            continue;
        }

        GcState current = decodeGcState(got->bytes);

        if (current.lease.owner == gc_id)
        {
            GcState next = current;
            ++next.lease.seq;
            const CasResult renew_res = store->backend().casPut(key, encodeGcState(next), got->token);
            if (renew_res.outcome == CasOutcome::Committed)
            {
                rememberObservation(next.lease);
                state = std::move(next);
                state_token = renew_res.token;
                return true;
            }
            continue;
        }

        GcHeartbeat hb;
        if (const auto hb_got = store->backend().get(store->layout().gcHbKey()))
            hb = decodeGcHeartbeat(hb_got->bytes);
        const bool hb_alive = has_observation
            && hb.owner == current.lease.owner
            && hb.hb_seq > last_seen_hb_seq;

        const bool incumbent_renewed = !has_observation
            || current.lease.owner != last_seen_owner
            || current.lease.seq != last_seen_seq;
        if (incumbent_renewed || hb_alive)
        {
            rememberObservation(current.lease);
            last_seen_hb_owner = hb.owner;
            last_seen_hb_seq = hb.hb_seq;
            return false;
        }

        GcState next = current;
        next.lease.owner = gc_id;
        ++next.lease.seq;
        ++next.fence_seq;
        const CasResult steal_res = store->backend().casPut(key, encodeGcState(next), got->token);
        if (steal_res.outcome == CasOutcome::Committed)
        {
            rememberObservation(next.lease);
            state = std::move(next);
            state_token = steal_res.token;
            return true;
        }

        if (const auto reread = store->backend().get(key))
            rememberObservation(decodeGcState(reread->bytes).lease);
        return false;
    }

    return false;
}

void Gc::reclaimDroppedShards(const FoldResult & folded)
{
    /// Task 6: reclaim ref-shard objects that are empty + tombstoned + fully folded (soak S30).
    ///
    /// CORRECTNESS CONSTRAINT — FULLY-FOLDED-BEFORE-RECLAIM (not optional):
    /// The fold cursor MUST cover the tombstone and every prior -1 removal event before we delete
    /// the shard object. Deleting before the removal events are folded leaves phantom in-degree on
    /// the blobs those refs named — a silent blob-retention leak. We enforce this by requiring
    /// `cov.folded_cursor >= tombstone_version` (the tombstone's `transition_version`).
    ///
    /// TOKEN: We use the token from the eligibility GET (`readShard`) for `deleteExact`. The fence
    /// step modifies every present shard each round (bumps `fence_round` via `mutateShard`), so the
    /// LIST token from the discovery sweep is always stale by the time we reach this step. The GET is
    /// already required for eligibility (tombstone present, refs empty), so no extra round-trip is
    /// incurred for the token.
    ///
    /// CONCURRENT REVIVE: a writer appending to the shard after the tombstone changes its body =>
    /// different token from GET => `deleteExact` returns `TokenMismatch` => shard survives (fail-closed).
    /// The writer's new publication has a strictly greater incarnation (Task 2), and the next round folds
    /// from cursor 0 (Task 3 ABA reset).
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    for (const auto & [ns, shard] : folded.root_shards)
    {
        try
        {
            const String ck = cursorKey(ns, shard);

            /// Need fold coverage to verify fully-folded-at-current-incarnation.
            const auto cov_it = folded.fold_seal.per_ns_shard.find(ck);
            if (cov_it == folded.fold_seal.per_ns_shard.end())
                continue;   /// shard not covered in this fold (Skipped — cannot verify fully folded)
            const ShardCoverage & cov = cov_it->second;

            /// Read the shard to check: refs empty, last event is tombstone. This GET also gives us the
            /// current token for the deleteExact — avoids a separate HEAD/GET for the token.
            /// IMPORTANT: This must run BEFORE trim() (see runRegularRound): trim removes events at/below
            /// the fold cursor, which includes the tombstone. After trim the journal is empty and this guard
            /// would fail. The ordering is trim runs AFTER reclaimDroppedShards.
            const auto [root, shard_tok] = store->readShard(ns, shard);

            /// Guard 1: refs must be empty (all dropped — the tombstone comes after all -1 events).
            if (!root.refs.empty())
                continue;

            /// Guard 2: last journal event must be the tombstone.
            if (root.journal.empty() || !root.journal.back().is_tombstone)
                continue;
            const uint64_t tombstone_version = root.journal.back().transition_version;

            /// Guard 3: FULLY-FOLDED-BEFORE-RECLAIM (the binding correctness constraint, not optional).
            /// `cov.folded_cursor` is the journal position the fold reached this round.
            /// `tombstone_version` is the transition_version of the tombstone event. We compare against
            /// the tombstone's version (not root.shard_version) because fence() may have bumped shard_version
            /// beyond the tombstone's version without adding a journal entry.
            /// Fully folded <=> cursor >= tombstone_version AND incarnations match (no ABA).
            if (cov.incarnation != root.incarnation)
                continue;   /// ABA: shard was recreated between fold and here — not safe to reclaim
            if (cov.folded_cursor < tombstone_version)
                continue;   /// tombstone (or prior events) not yet folded — wait for a later round

            /// Guard 4: NO LIVE OWNER BINDINGS (guards against the activated-precommit blob-leak).
            ///
            /// `refs.empty()` (Guard 1) only covers COMMITTED bindings (the promoted refs). A
            /// PRECOMMIT binding lives solely in the journal, never in `refs` — it is only moved to
            /// `refs` at `promote`. So a shard that was dropped while a same-namespace build had an
            /// ACTIVATED (body-present) but unpromoted precommit passes Guards 1-3: `refs` is empty,
            /// a tombstone was appended, and the fold advanced past the tombstone (the body being
            /// present meant the fold barrier did NOT clamp the cursor below the precommit event).
            /// If we reclaim such a shard, the precommit's +1 edge is already folded into the sealed
            /// generation's in-degree, but the matching -1 (from a future abandon / PrecommitRemove)
            /// can never land — the shard object is gone. The referenced blob's in-degree never drains
            /// → permanent blob leak.
            ///
            /// Guard: replay the journal's owner-state exactly as `reclaimAbandonedPrecommit` does
            /// (accumulate `new_binding`, erase on `old_binding`) and refuse reclaim if ANY live owner
            /// binding remains (Committed OR Precommit). In the normal fully-dropped case all bindings
            /// have been removed before the tombstone, so the set is empty and reclaim proceeds.
            ///
            /// NOTE: the body-ABSENT precommit sub-case is already safe (the fold barrier clamps
            /// `folded_cursor` below the live precommit, so Guard 3 blocks it). This guard
            /// additionally covers the body-PRESENT (activated) sub-case.
            {
                std::vector<OwnerBinding> live_bindings;
                for (const RootOwnerEvent & ev : root.journal)
                {
                    if (ev.old_binding)
                        std::erase(live_bindings, *ev.old_binding);
                    if (ev.new_binding)
                        live_bindings.push_back(*ev.new_binding);
                }
                if (!live_bindings.empty())
                    continue;   /// live owner binding present — skip reclaim this round
            }

            /// All guards pass. Delete using the token from the GET above (not an extra round-trip;
            /// the GET was already needed for tombstone + empty-refs eligibility). The fence step
            /// runs between the fold's LIST sweep and here, so the LIST token would be stale.
            if (!shard_tok)
                continue;   /// shard vanished between the GET and the delete — concurrent reclaim; skip
            const String key = layout.rootShardKey(ns, shard);
            const DeleteOutcome del = backend.deleteExact(key, *shard_tok);
            /// Tolerate NotFound (raced delete by another leader) and TokenMismatch (concurrent revive:
            /// a writer appended after the tombstone — the object survives, which is correct).
            /// Never throw on 404 (feedback_ca_gc_never_throw_on_404).
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcShardReclaim;
                e.namespace_ = ns.string();
                e.object_kind = CasEventObjectKind::Root;
                e.outcome = del.kind == DeleteOutcome::Kind::Deleted   ? "deleted"
                          : del.kind == DeleteOutcome::Kind::NotFound  ? "absent"
                                                                       : "replaced";
                e.reason = "Task 6: reclaim empty+tombstoned+fully-folded ref-shard object (S30)";
                e.detail = {{"shard", std::to_string(shard)},
                            {"key", key},
                            {"fold_cursor", std::to_string(cov.folded_cursor)},
                            {"shard_version", std::to_string(root.shard_version)}};
            });
        }
        catch (const Exception & e)
        {
            /// Per-shard error isolation: a bad shard must not abort the rest or let trim erase
            /// the tombstone before this shard is reclaimed. Log and continue (record-and-continue
            /// per feedback_ca_gc_never_throw_on_404). The shard is retried next round.
            LOG_WARNING(getLogger("CasGc"),
                "CAS gc shard reclaim: error for {}/{}, skipping this shard this round: {}",
                ns.string(), shard, e.message());
        }
    }
}

}
