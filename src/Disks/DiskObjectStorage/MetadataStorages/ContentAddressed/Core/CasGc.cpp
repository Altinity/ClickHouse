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

Gc::Gc(StorePtr store_, UInt128 gc_id_, std::function<uint64_t()> now_ms_fn_)
    : store(std::move(store_))
    , gc_id(gc_id_)
    , now_ms_fn(std::move(now_ms_fn_))
{
    if (!store)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: store must not be null");
    if (gc_id == UInt128(0))
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Cas::Gc: gc_id must not be 0 (reserved for 'lease never held')");
    if (!now_ms_fn)
        now_ms_fn = []() -> uint64_t
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        };
}

RoundReport Gc::runRegularRound()
{
    RoundReport report;
    GcState state;
    Token state_token;
    report.acquired_lease = acquireOrRenewLease(state, state_token);
    if (!report.acquired_lease)
        return report;

    /// ONE-PASS ack-floor round (spec 2026-07-02 + Task-9 amendment). There is no crash-resume step
    /// anymore: the round commits everything in the SINGLE gc/state CAS at the end, so a crashed pass
    /// leaves only attempt-scoped debris that is never adopted (retention prunes it), and every
    /// destructive PRE-CAS action below is justified by PREVIOUSLY PUBLISHED durable state only
    /// (delete_pending entries), so replay under a fresh attempt is idempotent.

    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    const uint64_t new_round = state.round + 1;

    /// R1: the heartbeat ack floor + token-guarded fence-out of expired mounts. The ONLY clock in the
    /// round (the inherited lease-expiry contract); margin = ttl/2 (poll granularity + wall skew).
    /// ORDER INVARIANT (CaGcAckFloorZombie): the floor MUST be latched BEFORE the fold cut. A floor
    /// (re-)read after folding would see acks advertised by writers whose in-flight commits landed
    /// AFTER the cut — invisible to this pass's in-degrees — and a fresh graduation could go pending
    /// over a live reference. Never move this call below fold, never refresh the floor mid-pass.
    const uint64_t skew_margin_ms =
        static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count()) / 2;
    const HeartbeatFloor floor = computeHeartbeatFloor(backend, layout, now_ms_fn(), skew_margin_ms);
    /// B170: the round's floor — the fence's successor record (what gates this round's graduations).
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFence;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = new_round;
        e.gen = state.snap_generation;
        e.outcome = "floor";
        e.reason = "R1: heartbeat ack floor (min over live + expired-unfenced observed_gc_round)";
        e.detail = {{"min_ack", floor.min_ack == UINT64_MAX ? String("inf") : std::to_string(floor.min_ack)},
                    {"live", std::to_string(floor.live)},
                    {"terminated", std::to_string(floor.terminated)},
                    {"fenced_now", std::to_string(floor.fenced_now)},
                    {"already_fenced", std::to_string(floor.already_fenced)}};
    });

    /// B170: fold begins — the round's single pass.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldBegin;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = state.round;
        e.gen = state.snap_generation;
        e.reason = "R2: one-pass fold (edges x deltas x retired) into a new durable generation";
    });

    /// R2: the pass — discovery, windows, and the three-cursor merge (spare / graduate / condemn).
    FoldResult folded = fold(state, state_token, report, floor.min_ack);

    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::GcFoldEnd;
        e.object_kind = CasEventObjectKind::Snap;
        e.round = state.round;
        e.gen = state.snap_generation;
        e.outcome = "ok";
        e.reason = "R2 complete";
        e.detail = {{"shards", std::to_string(folded.root_shards.size())},
                    {"anomalies", std::to_string(report.anomalies.size())}};
    });

    const uint64_t generation = state.snap_generation;   /// set in-memory by fold; committed below
    const uint64_t attempt = state.snap_attempt;

    /// R3: PRE-CAS deletes — ONLY entries the PREVIOUS pass published as delete_pending (justified by
    /// durable state, safe at any leader staleness — Task-9 amendment), plus outcome bookkeeping for
    /// every settled entry. THE SINGLE CONTENT-DELETE SITE.
    std::map<uint64_t, OutcomeLog> outcomes;
    for (uint64_t shard = 0; shard < folded.retired_merge.size(); ++shard)
    {
        RetiredMergeResult & merge = folded.retired_merge[shard];
        for (const RetiredEntry & entry : merge.redelete)
        {
            const DeleteOutcome del = backend.deleteExact(blobKeyOf(layout, entry.hash), entry.token);
            if (del.created_delete_marker)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS gc: delete of blob {} created a delete marker — versioning is enabled "
                    "on the pool (mis-provisioned; the capability probe must reject this)", u128ToHex(entry.hash));
            OutcomeEntry outcome{.kind = entry.kind, .hash = entry.hash, .token = entry.token,
                                 .outcome = del.kind == DeleteOutcome::Kind::Deleted ? OutcomeKind::Deleted
                                          : del.kind == DeleteOutcome::Kind::NotFound ? OutcomeKind::Absent
                                          : OutcomeKind::Replaced};
            const String del_outcome = outcome.outcome == OutcomeKind::Deleted ? "deleted"
                                     : outcome.outcome == OutcomeKind::Absent ? "absent" : "replaced";
            /// B170: the single content-delete site — attributable per row. TokenMismatch (a writer
            /// recreated the incarnation) is terminal-OK: the fresh incarnation is a live object.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::BlobDelete;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(entry.hash);
                e.token = entry.token.value;
                e.round = new_round;
                e.gen = generation;
                e.outcome = del_outcome;
                e.reason = "delete_pending published by a prior pass; exact-token delete (pre-CAS)";
                e.detail = {{"condemn_round", std::to_string(entry.condemn_round)},
                            {"key", blobKeyOf(layout, entry.hash)}};
            });
            outcomes[shard].entries.push_back(std::move(outcome));
        }
        for (const RetiredEntry & entry : merge.spared)
        {
            if (entry.delete_pending)
                LOG_WARNING(getLogger("CasGc"),
                    "CAS gc: delete_pending blob {} recovered in-degree — structurally impossible under "
                    "the ack floor (spared anyway, fail-closed); investigate",
                    u128ToHex(entry.hash));
            /// B170: the spare verdict — a publish re-pinned the candidate before graduation.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcRecheckVerdict;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(entry.hash);
                e.token = entry.token.value;
                e.round = new_round;
                e.gen = generation;
                e.outcome = "spared";
                e.reason = "in-degree recovered in the pass merge; entry dropped";
            });
            outcomes[shard].entries.push_back(OutcomeEntry{.kind = entry.kind, .hash = entry.hash,
                                                           .token = entry.token, .outcome = OutcomeKind::Spared});
        }
        for (const RetiredEntry & entry : merge.graduated)
        {
            /// B170: floor-passed — republished pending; the NEXT pass executes the delete.
            EventEmitter{*store}.emit([&](CasEvent & e)
            {
                e.type = CasEventType::GcRecheckVerdict;
                e.object_kind = CasEventObjectKind::Blob;
                e.object_hash = u128ToHex(entry.hash);
                e.token = entry.token.value;
                e.round = new_round;
                e.gen = generation;
                e.outcome = "pending";
                e.reason = "condemn_round < min_ack; published delete_pending (two-phase graduation)";
                e.detail = {{"condemn_round", std::to_string(entry.condemn_round)}};
            });
        }
    }

    /// Outcome logs: write-once + byte-adopt (observation-bearing HEAD tokens — never the
    /// deterministic-artifact path). Tally the report from the FINAL durable logs.
    for (auto & [shard, log] : outcomes)
    {
        const String key = layout.outcomesKey(generation, attempt, new_round, shard);
        const String body = encodeOutcomeLog(log);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc: outcome log at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try { log = decodeOutcomeLog(existing->bytes); }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc: undecodable outcome log at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
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

    /// R4: PUBLISH ORDER — the new current retired list (per gc-shard, ALWAYS written so the refs in
    /// gc/state always resolve) is durable BEFORE the CAS that publishes the round. Observation-bearing
    /// (HEAD tokens) => write-once + byte-adopt, attempt-scoped keys.
    std::map<uint64_t, String> new_refs;
    for (uint64_t shard = 0; shard < folded.retired_merge.size(); ++shard)
    {
        RetiredSet set;
        set.entries = std::move(folded.retired_merge[shard].still_retired);
        const String key = layout.retiredKey(generation, attempt, new_round, shard);
        const String body = encodeRetiredSet(set);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc: retired list at {} vanished between putIfAbsent and read", key);
            /// A byte-divergent occupant under OUR OWN attempt key is a replay that observed different
            /// HEAD tokens; adopt it (first-durable-write-wins, exactly like the old retire sets).
        }
        new_refs[shard] = key;
    }

    /// R5: the SINGLE round CAS — round, adopted (generation, attempt), retired refs, retention cursor.
    GcState next = state;
    next.round = new_round;
    next.retired_refs = std::move(new_refs);
    pruneSupersededGenerations(generation, attempt, next);
    const CasResult res = backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc round: gc/state moved during the round (another leader advanced it); retry next round");
    state = std::move(next);
    state_token = res.token;
    report.round = state.round;
    report.candidates += 0;   /// candidates are counted at condemn time (head_blob, inside the fold)

    /// R6 post-CAS: owner-removed manifest bodies — deleted ONLY now, after their decrements were
    /// ADOPTED by the round CAS (delete-after-sealed-decrements, control #11). Best-effort: a crash
    /// here leaks bodies to the orphan sweep, never a dangle.
    for (const auto & [id, token] : folded.mf_cleanup)
    {
        const DeleteOutcome mdel = backend.deleteExact(layout.manifestKey(id), token);   /// NotFound/TokenMismatch tolerated
        if (mdel.kind == DeleteOutcome::Kind::Deleted)
            ++report.manifests_deleted;
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::TreeDelete;
            e.namespace_ = id.root_namespace.string();
            e.object_kind = CasEventObjectKind::Tree;
            e.object_hash = manifestRefDebugString(id.ref);
            e.token = token.value;
            e.round = new_round;
            e.gen = generation;
            e.outcome = mdel.kind == DeleteOutcome::Kind::Deleted ? "deleted"
                      : mdel.kind == DeleteOutcome::Kind::NotFound ? "absent" : "replaced";
            e.reason = "owner-removed manifest body; exact-token delete after decrements adopted";
        });
    }

    /// Task 6: reclaim empty+tombstoned+fully-folded ref-shard objects. Must run BEFORE trim so the
    /// tombstone event is still present in the journal.
    reclaimDroppedShards(folded);

    /// Trim journals below the sealed fold cursor.
    if (trim_enabled)
        trim(folded, state.round);

    /// Bounded orphan-manifest backstop: cleanup-only cursor progress; never fails the round.
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

Gc::FoldResult Gc::fold(GcState & state, Token & /*state_token*/, RoundReport & report, uint64_t min_ack)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();
    FoldResult result;

    /// 1. Discover the present (namespace, shard) pairs via LIST(cas/refs/) (Task 4 LIST-based discovery).
    result.root_shards = discoverUniverse();

    /// Parent cursors — the per-(ns,shard) cursors a prior round sealed. One-pass round: read them from
    /// the fold seal at the adopted (snap_generation, snap_attempt) (the fold seal IS the coverage record).
    /// Absent => fresh pool (cursor 0). A folded event must never be re-folded from 0 (that double-counts
    /// blob in-degree => silent over-pin/leak).
    const std::map<String, ShardCoverage> parent_cursors = readSealedCursors(state.snap_generation, state.snap_attempt);

    /// Ack-floor: load the CURRENT retired list (published by the previous round's CAS). A ref that
    /// does not resolve is integrity loss of destructive bookkeeping — fail closed (this is GC's own
    /// metadata, not a data-plane 404; losing entries would silently reset condemnation pipelines and
    /// leak pending deletes).
    const uint64_t condemn_round = state.round + 1;
    std::vector<std::vector<RetiredEntry>> prior_retired(state.gc_shards);
    for (const auto & [retired_shard, retired_key] : state.retired_refs)
    {
        if (retired_shard >= state.gc_shards)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc fold: retired ref for shard {} exceeds gc_shards {}", retired_shard, state.gc_shards);
        const auto got = backend.get(retired_key);
        if (!got)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc fold: current retired list at {} (shard {}) is missing — refusing the round",
                retired_key, retired_shard);
        RetiredSet set = decodeRetiredSet(got->bytes);
        prior_retired[retired_shard] = std::move(set.entries);
    }
    result.retired_merge.resize(state.gc_shards);

    /// Condemn-time observation: ONE HEAD per new zero-transition captures the exact incarnation token
    /// the eventual delete carries (absent => a prior landed delete => nothing to condemn). Emits the
    /// B170 candidate trail (IndegZero / GcRetireObserve / BlobRetire) exactly where the decision is made.
    const auto head_blob = [&](const UInt128 & hash) -> std::optional<HeadResult>
    {
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::IndegZero;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = u128ToHex(hash);
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.reason = "last folded owner edge dropped; in-degree reached 0";
        });
        const HeadResult observed = backend.head(blobKeyOf(layout, hash));
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcRetireObserve;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = u128ToHex(hash);
            e.token = observed.exists ? observed.token.value : "";
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.outcome = observed.exists ? "present" : "absent";
            e.reason = "zero-in-degree candidate; HEAD-observe the current token";
        });
        if (!observed.exists)
            return std::nullopt;
        ++report.candidates;
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::BlobRetire;
            e.object_kind = CasEventObjectKind::Blob;
            e.object_hash = u128ToHex(hash);
            e.token = observed.token.value;
            e.round = condemn_round;
            e.gen = state.snap_generation + 1;
            e.outcome = "retired";
            e.reason = "condemned zero-in-degree candidate; entering the current retired list";
        });
        HeadResult adjusted = observed;
        adjusted.size = retiredLogicalSize(ObjectKind::Blob, observed.size, store->poolMeta().blob_header_len);
        return adjusted;
    };

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
    if (const auto fold_seal = readFoldSeal(state.snap_generation, state.snap_attempt))
        discover_ref_seal = *fold_seal;
    /// else: leave discover_ref_seal empty (fresh pool / no adopted seal) => all Read. (The one-pass
    /// round writes only fold seals; completion seals are a retired concept. `folded_token` is now the
    /// FOLD-time shard token — conservative: any later write changes it => Read next round.)
    const std::map<String, DiscoverDecision> discover_decisions =
        computeDiscoverDecisions(discover_ref_seal);

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
        /// PrecommitRemove event is folded IN THIS SAME ROUND — its `-1` lands in this round's deltas and the
        /// fold cursor advances to cover it. (Appending it AFTER sealing the cursor would leave an event above
        /// the sealed cursor, which the next round's fold would apply — a double-counted `-1`.) The reclaim
        /// PIGGYBACKS on this per-shard visit: it reads the shard
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
        /// Clamped coverage (4) is load-bearing for the token-diff: a barrier/anomaly clamp leaves
        /// unfolded events that a manifest-body arrival can make foldable WITHOUT touching the shard
        /// (token unchanged) — Skip would park them forever.
        cov.classification = clamped ? 4 : (shard_changed ? 2 : 1);
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
                                     {}, result.fold_seal.blob_target_runs,
                                     prior_retired[0], min_ack, condemn_round, head_blob,
                                     &result.retired_merge[0]);
        }
        else
        {
            foldDeltasIntoGeneration(backend, layout, state.snap_generation, state.snap_attempt,
                                     new_generation, attempt, /*shard*/0,
                                     std::move(deltas), result.fold_seal.blob_target_runs,
                                     prior_retired[0], min_ack, condemn_round, head_blob,
                                     &result.retired_merge[0]);
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
                               std::move(buckets[shard]),
                               prior_retired[shard], min_ack, condemn_round, head_blob,
                               &result.retired_merge[shard]);
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

    /// One-pass round: the fold NO LONGER CASes gc/state. (new_generation, attempt) are adopted
    /// in-memory here and committed — together with the round, the retired refs, and the retention
    /// cursor — by the SINGLE round CAS in runRegularRound. A deposed leader's whole pass therefore
    /// evaporates at that one CAS; its attempt-scoped artifacts are never adopted.
    state.snap_generation = new_generation;
    state.snap_attempt = attempt;
    return result;
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

std::map<String, ShardCoverage> Gc::readSealedCursors(uint64_t generation, uint64_t attempt)
{
    /// One-pass round: the fold seal at the adopted (generation, attempt) IS the coverage record
    /// (completion seals are a retired concept — pre-cutover pools are unsupported, pre-release).
    /// Absent => empty (fresh pool, cursor 0).
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

std::map<String, Gc::DiscoverDecision> Gc::computeDiscoverDecisions(const CasFoldSeal & sealed)
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

        /// CLAMP guard: a barrier/anomaly-clamped fold (classification 4) left unfolded events that can
        /// become foldable with NO shard write (e.g. the missing precommit body arrives) — the token
        /// comparison is blind to that, so a clamped shard is forced Read.
        if (sealed_it->second.classification == 4)
            continue;   /// clamped => Read (already defaulted)

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
        return computeDiscoverDecisions(CasFoldSeal{});

    const GcState state = decodeGcState(state_bytes->bytes);

    /// One-pass round: the adopted fold seal IS the reference (fold-time tokens; conservative — any
    /// later shard write changes the token => Read). Mirrors `fold`'s reference-seal resolution.
    CasFoldSeal ref_seal;
    if (const auto fold = readFoldSeal(state.snap_generation, state.snap_attempt))
        ref_seal = *fold;
    /// else: empty ref_seal (fresh pool / no adopted seal) => all Read.

    return computeDiscoverDecisions(ref_seal);
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

/// The build-watermark floor now rides the per-server mount lease (ack-floor merge, spec 2026-07-02).
/// A namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain
/// slashes. Try namespace prefixes from longest to shortest and accept the first durable mount body.
/// `{writer_epoch, min_active}` are the same durable facts the old watermark's `{epoch, min_active}`
/// carried on the writable path (CasStore.cpp "THE BRIDGE").
std::optional<MountLease> floorForNamespace(Store & store, const RootNamespace & ns)
{
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
            if (const auto got = store.backend().get(store.layout().mountKey(server_root_id)))
                return decodeMountLease(got->bytes);
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

    const auto floor = floorForNamespace(*store, ns);
    if (!floor)
        return;   /// no durable fact => not dead (conservative)
    const MountLease & w = *floor;

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
        if (binding.manifest_ref.writer_epoch > w.writer_epoch)
            continue;
        const bool retired_sentinel = w.min_active == std::numeric_limits<uint64_t>::max();
        const bool is_dead = binding.manifest_ref.writer_epoch < w.writer_epoch
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
