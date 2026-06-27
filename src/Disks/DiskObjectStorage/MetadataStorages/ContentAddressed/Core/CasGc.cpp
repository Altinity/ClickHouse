#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <base/defines.h>
#include <city.h>
#include <algorithm>
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

    /// R3: global registry + all-shard fence; record fence positions into the completion seal.
    fence(state, state_token, folded);

    /// R4: fold-through-fence recheck + the single content-delete site + exact-token manifest deletes;
    /// seal the completion generation; drop the round's retired sets.
    recheck(state, state_token, folded, retired, report);

    /// Trim journals below the sealed fold cursor.
    trim(folded, state.round);

    /// Bounded orphan-manifest backstop (spec §Orphan sweep): at most one namespace + one eligible
    /// prefix per round. Records-and-continues on a 404; never throws (feedback_ca_gc_never_throw_on_404).
    try
    {
        if (auto target = pickOneSweepTarget(*store))
            sweepNamespace(*store, target->ns, target->prefix);
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
            deltas.push_back(BlobDelta{.blob_hash = entry.blob_hash, .delta = sign});
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
                ev.detail = {{"manifest_ref_instance", u128ToHex(id.ref.manifest_instance_id)},
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

    /// B171 precommit reclaim uses the per-round watermark caches the K=2 detector accumulates.
    beginWatermarkRound();

    /// 1. Discover the namespace universe FROM THE REGISTRY (LIST is only an accelerator).
    result.root_shards = discoverUniverse();

    /// Parent fold seal — the cursors a prior fold sealed per shard. Absent => fresh pool (cursor 0).
    const std::optional<CasFoldSeal> parent_seal = readFoldSeal(state.snap_generation);
    const CasFoldSeal empty_parent;
    const CasFoldSeal & parent = parent_seal ? *parent_seal : empty_parent;

    const uint64_t new_generation = state.snap_generation + 1;
    result.fold_seal.generation = new_generation;
    result.fold_seal.parent_generation = state.snap_generation;

    std::vector<BlobDelta> deltas;
    bool folded_any = false;

    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = cursorKey(ns, root_shard);
        const uint64_t cursor = sealedCursorOf(parent, cursor_key);

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
        cov.classification = shard_changed ? 2 : 1;
        result.fold_seal.per_ns_shard[cursor_key] = cov;
        if (shard_changed)
            folded_any = true;

        if (Layout::isPrecommitNamespace(ns))
            reclaimAbandonedPrecommit(ns, root_shard, root, state.round + 1);
    }

    if (!folded_any)
    {
        /// Nothing new this round; still seal the (empty-delta) generation so the cursor coverage is
        /// durable and the resume rule has a fold_seal to key off. Reuse the prior generation's blob run
        /// (no delta) by sealing a fresh generation whose in-degree equals the parent.
        foldDeltasIntoGeneration(backend, layout, state.snap_generation, new_generation, /*shard*/0,
                                 {}, result.fold_seal.blob_target_runs);
    }
    else
    {
        foldDeltasIntoGeneration(backend, layout, state.snap_generation, new_generation, /*shard*/0,
                                 std::move(deltas), result.fold_seal.blob_target_runs);
    }
    writePartManifestCleanupBundle(new_generation, /*owner_shard*/0, result.mf_cleanup,
                                   result.fold_seal.part_manifest_cleanup);

    /// Write-once CasFoldSeal: its existence marks fold complete. On PreconditionFailed adopt a
    /// byte-equal occupant as our own crash-replay, else ABORTED.
    {
        const String body = encodeFoldSeal(result.fold_seal);
        const String key = layout.foldSealKey(new_generation);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing || existing->bytes != body)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc fold: fold seal at {} occupied by divergent bytes (concurrent leader); retry", key);
        }
    }

    state.snap_generation = new_generation;
    const CasResult fold_res = backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token);
    if (fold_res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (another leader advanced it); retry next round");
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
    for (const BlobCandidate & cand : zeroInDegree(backend, layout, folded.fold_seal.generation, /*shard*/0))
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
        result.blobs[/*single shard*/0].entries.push_back(std::move(entry));
    }

    /// Write each shard's retired set write-once (adopt a byte-equal occupant as our crash-replay).
    for (auto & [shard, set] : result.blobs)
    {
        const String key = layout.retiredKey(round, state.fence_seq, shard);
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

    /// 0. CAS the NAMESPACE REGISTRY's fence_round first — the ordering point for namespace creation.
    /// The committed-attempt registry is ALSO the shard-fence universe (a superset of the fold-time one).
    RootsRegistry fence_universe;
    {
        const String registry_key = layout.rootsRegistryKey();
        bool registry_fenced = false;
        for (size_t attempt = 0; attempt < 100 && !registry_fenced; ++attempt)
        {
            const auto got = backend.get(registry_key);
            RootsRegistry registry;
            if (got)
                registry = decodeRootsRegistry(got->bytes);
            registry.fence_round = std::max(registry.fence_round, round);
            ++registry.registry_version;
            const CasOutcome outcome = (got
                ? backend.casPut(registry_key, encodeRootsRegistry(registry), got->token)
                : backend.casPut(registry_key, encodeRootsRegistry(registry), std::nullopt)).outcome;
            if (outcome == CasOutcome::Committed)
            {
                state.fence_version[round]["_registry"] = registry.registry_version;
                folded.completion_seal.fence_positions["_registry"] = registry.registry_version;
                fence_universe = std::move(registry);
                registry_fenced = true;
            }
        }
        if (!registry_fenced)
            throw Exception(ErrorCodes::ABORTED,
                "CAS gc fence: registry CAS contention (runaway live-lock brake)");
    }

    /// 1. CAS fence_round := max(fence_round, round) into EVERY root shard of EVERY namespace in the
    /// fence-time registry — present or ABSENT shards alike (an absent shard's create-if-absent CAS mints
    /// a fence-only manifest; the create race against a first publish is the required total order).
    for (const String & ns_name : fence_universe.namespaces)
    {
        const RootNamespace ns{ns_name};
        for (const uint64_t shard : shardsToVisit(ns))
        {
            uint64_t committed = 0;
            store->mutateShard(ns, shard, [&](RootShard & root)
            {
                root.fence_round = std::max(root.fence_round, round);
            }, &committed);
            state.fence_version[round][cursorKey(ns, shard)] = committed;
            folded.completion_seal.fence_positions[cursorKey(ns, shard)] = committed;
        }
    }

    /// 2. ONE gc/state CAS persists the whole fence_version[round] vector.
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
            e.reason = "R3: fenced the registry + every root shard of every registered namespace";
            e.detail = {{"fence_seq", std::to_string(state.fence_seq)},
                        {"namespaces", std::to_string(fence_universe.namespaces.size())}};
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
    for (const auto & [cursor_key, fence_version] : fence_it->second)
    {
        if (cursor_key == "_registry")
            continue;
        const auto [ns, shard] = parseCursorKey(cursor_key);
        const auto [root, tok] = store->readShard(ns, shard);
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
    foldDeltasIntoGeneration(backend, layout, state.snap_generation, completion_generation, /*shard*/0,
                             std::move(window), folded.completion_seal.blob_target_runs);

    /// 2. Per retired blob: spare if in-degree > 0 in the completion generation, else exact-token delete.
    std::map<uint64_t, OutcomeLog> computed;
    for (const auto & [shard, set] : retired.blobs)
    {
        for (const RetiredEntry & entry : set.entries)
        {
            OutcomeEntry outcome{.kind = entry.kind, .hash = entry.hash, .token = entry.token,
                                 .outcome = OutcomeKind::Spared};
            const int64_t indeg_at_recheck =
                inDegreeInGeneration(backend, layout, completion_generation, /*shard*/0, entry.hash);
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
        const String key = layout.outcomesKey(round, state.fence_seq, shard);
        const String body = encodeOutcomeLog(log);
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
        }
        folded.completion_seal.delete_outcomes.push_back(
            RunRef{.key = key, .checksum = cityHash128(encodeOutcomeLog(log))});
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

    /// 3. Manifest exact-token deletes — ONLY now, after the owner-removal decrements are sealed into the
    /// fold generation (control #11). recheck never READS the body to recompute decrements (they were
    /// produced at fold from the present body); it deletes by the token captured at fold. A manifest the
    /// fold-through-fence re-pinned (its owner restored) is kept.
    for (const auto & [id, token] : folded.mf_cleanup)
    {
        if (recheck_cleanup.count(id))
            continue;   /// re-removed in the fence window too; still deleting once is fine
        const DeleteOutcome mdel = backend.deleteExact(layout.manifestKey(id), token);   /// NotFound/TokenMismatch tolerated
        /// B170: the owner-removed manifest body delete (the tree-delete analog in the manifest model) —
        /// deleted ONLY after its blob decrements were sealed into the fold generation (control #11).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::TreeDelete;
            e.namespace_ = id.root_namespace.string();
            e.object_kind = CasEventObjectKind::Tree;
            e.object_hash = u128ToHex(id.ref.manifest_instance_id);
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
    {
        const String body = encodeCompletionSeal(folded.completion_seal);
        const String key = layout.completionSealKey(completion_generation);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing || existing->bytes != body)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc recheck: completion seal at {} occupied by divergent bytes; retry", key);
        }
    }

    GcState next = state;
    next.snap_generation = completion_generation;
    std::erase_if(next.fence_version, [&](const auto & kv) { return kv.first <= round; });
    const CasResult res = backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc recheck: gc/state moved during the recheck persist (another leader advanced it); retry");
    state = std::move(next);
    state_token = res.token;

    /// Drop the round's retired-set objects on their confirmed outcomes (a GC-metadata delete).
    for (const auto & [shard, set] : retired.blobs)
    {
        const String key = layout.retiredKey(round, state.fence_seq, shard);
        if (const auto got = backend.get(key))
        {
            const DeleteOutcome dropped = backend.deleteExact(key, got->token);
            if (dropped.kind == DeleteOutcome::Kind::TokenMismatch)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS gc recheck: retired set at {} changed under us (token mismatch on drop)", key);
        }
    }
}

void Gc::trim(const FoldResult & folded, uint64_t /*round*/)
{
    for (const auto & [ns, shard] : folded.root_shards)
    {
        const String cursor_key = cursorKey(ns, shard);
        const auto cov_it = folded.fold_seal.per_ns_shard.find(cursor_key);
        if (cov_it == folded.fold_seal.per_ns_shard.end())
            continue;
        const uint64_t cursor = cov_it->second.folded_cursor;
        if (cursor == 0)
            continue;

        /// Peek: only touch the shard when there is something to trim (no pointless version bumps).
        const auto [peek, tok] = store->readShard(ns, shard);
        const bool has_trimmable = std::any_of(peek.journal.begin(), peek.journal.end(),
            [&](const RootOwnerEvent & e) { return e.transition_version <= cursor; });
        if (!has_trimmable)
            continue;

        store->mutateShard(ns, shard, [&](RootShard & fresh)
        {
            std::erase_if(fresh.journal,
                [&](const RootOwnerEvent & e) { return e.transition_version <= cursor; });
        });
        /// B170: the journal trim for this shard (events provably folded into the durable generation).
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::GcTrim;
            e.namespace_ = ns.string();
            e.object_kind = CasEventObjectKind::Root;
            e.outcome = "trimmed";
            e.reason = "INV-JOURNAL-COVERAGE: trimmed owner events at or below the durable fold cursor";
            e.detail = {{"shard", std::to_string(shard)},
                        {"trimmed_up_to_cursor", std::to_string(cursor)}};
        });
    }
}

void Gc::writePartManifestCleanupBundle(uint64_t generation, uint64_t owner_shard,
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

    const String run_key = layout.partManifestCleanupKey(generation, owner_shard, 0);
    backend.putIfAbsent(run_key, run_bytes);
    out.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes)});
}

std::optional<CasFoldSeal> Gc::readFoldSeal(uint64_t generation)
{
    if (const auto got = store->backend().get(store->layout().foldSealKey(generation)))
        return decodeFoldSeal(got->bytes);
    return std::nullopt;
}

uint64_t Gc::sealedCursorOf(const CasFoldSeal & parent, const String & cursor_key)
{
    const auto it = parent.per_ns_shard.find(cursor_key);
    return it != parent.per_ns_shard.end() ? it->second.folded_cursor : 0;
}

std::vector<std::pair<RootNamespace, uint64_t>> Gc::discoverUniverse()
{
    std::vector<std::pair<RootNamespace, uint64_t>> universe;
    if (const auto got = store->backend().get(store->layout().rootsRegistryKey()))
    {
        const RootsRegistry registry = decodeRootsRegistry(got->bytes);
        for (const String & ns_name : registry.namespaces)
        {
            const RootNamespace ns{ns_name};
            for (const uint64_t shard : shardsToVisit(ns))
                universe.emplace_back(ns, shard);
        }
    }
    return universe;
}

std::vector<uint64_t> Gc::shardsToVisit(const RootNamespace &)
{
    std::vector<uint64_t> shards;
    const uint64_t root_shards_per_ns = store->poolMeta().root_shards;
    shards.reserve(root_shards_per_ns);
    for (uint64_t shard = 0; shard < root_shards_per_ns; ++shard)
        shards.push_back(shard);
    return shards;
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

    for (const BlobCandidate & cand : zeroInDegree(backend, layout, state.snap_generation, /*shard*/0))
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
    return out;
}

bool Gc::tryResumeIncompleteRound(GcState & state, Token & state_token, RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    const uint64_t round = state.round;
    if (round == 0)
        return false;

    /// Detect an incomplete round from durable state: retired sets still present at (round, fence_seq).
    /// They drop only at the very end of a completed round (recheck step 4), so their presence is the
    /// durable incompleteness signal. The fold for this round already committed (retire's CAS advanced
    /// .round AFTER the fold's CAS), so a fold_seal exists for the current generation.
    RetireResult retired;
    for (uint64_t shard = 0; shard < state.snap_shards; ++shard)
        if (const auto got = backend.get(layout.retiredKey(round, state.fence_seq, shard)))
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
    if (const auto seal = readFoldSeal(state.snap_generation))
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
        const auto got = backend.get(layout.partManifestCleanupKey(state.snap_generation, /*owner_shard*/0, seq));
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
    trim(folded, state.round);
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

void Gc::beginWatermarkRound()
{
    watermark_cache.clear();
    server_live_this_round.clear();
}

const ServerWatermark * Gc::watermarkOf(UInt128 server_id)
{
    if (const auto it = watermark_cache.find(server_id); it != watermark_cache.end())
        return &it->second;

    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    const String key = layout.serverWatermarkKey(u128ToHex(server_id));

    const HeadResult head = backend.head(key);
    if (!head.exists)
        return nullptr;

    const auto got = backend.get(key);
    if (!got)
        return nullptr;

    const ServerWatermark w = decodeServerWatermark(got->bytes);
    const auto [it, _] = watermark_cache.emplace(server_id, w);

    bool live;
    const auto seen_it = last_seen_server_seq.find(server_id);
    if (seen_it == last_seen_server_seq.end())
    {
        live = true;
        last_seen_server_seq[server_id] = w.seq;
        server_frozen_rounds[server_id] = 0;
    }
    else if (w.seq != seen_it->second)
    {
        live = true;
        server_frozen_rounds[server_id] = 0;
        seen_it->second = w.seq;
    }
    else
    {
        const uint64_t frozen = ++server_frozen_rounds[server_id];
        live = frozen < 2;
    }
    server_live_this_round[server_id] = live;

    return &it->second;
}

void Gc::reclaimAbandonedPrecommit(const RootNamespace & ns, uint64_t shard, const RootShard & root,
                                   uint64_t /*round*/)
{
    /// B171 §C.3: reclaim ABANDONED precommits — those whose owning build is no longer live. The
    /// precommit namespace is sharded like any namespace (ref name == build_seq, shard == shardOf), so
    /// one shard holds many precommit refs. We derive the server from `<server_hex>/_precommits` and the
    /// build_seq from each REF NAME, reading the shard already in hand (the fold's `root`).
    if (root.refs.empty())
        return;

    const String & ns_str = ns.string();
    static constexpr std::string_view kPrecommitsSuffix = "/_precommits";
    chassert(ns_str.ends_with(kPrecommitsSuffix));
    const String server_hex = ns_str.substr(0, ns_str.size() - kPrecommitsSuffix.size());
    if (server_hex.size() != 32)
        return;
    UInt128 server;
    try
    {
        server = hexToU128(server_hex);
    }
    catch (...)
    {
        return;
    }

    const ServerWatermark * w = watermarkOf(server);
    const bool server_live = server_live_this_round[server];

    struct DeadRef { String name; uint64_t build_seq; };
    std::vector<DeadRef> dead_refs;
    for (const auto & [name, payload] : root.refs)
    {
        uint64_t build_seq = 0;
        try
        {
            size_t consumed = 0;
            build_seq = std::stoull(name, &consumed);
            if (consumed != name.size())
                continue;
        }
        catch (...)
        {
            continue;
        }

        const bool dead = (w == nullptr) || !server_live || build_seq < w->min_active;
        if (dead)
            dead_refs.push_back(DeadRef{name, build_seq});
    }

    if (dead_refs.empty())
        return;

    /// Reclaim: drop each dead precommit ref + append a removal RootOwnerEvent on the SAME shard (the
    /// next fold reads the removal and releases the edges, so the closure's blobs become normal
    /// zero-in-degree candidates). ONE mutateShard CAS removes all dead refs at once.
    store->mutateShard(ns, shard, [&](RootShard & fresh)
    {
        for (const DeadRef & dr : dead_refs)
        {
            auto it = fresh.refs.find(dr.name);
            if (it == fresh.refs.end())
                continue;
            OwnerBinding old_binding;
            old_binding.owner_kind = OwnerKind::Precommit;
            old_binding.ref_name = it->second.ref_name;
            old_binding.manifest_ref = it->second.manifest_ref;
            fresh.refs.erase(it);
            fresh.journal.push_back(RootOwnerEvent{
                .transition_version = fresh.shard_version + 1,
                .old_binding = std::move(old_binding),
                .new_binding = std::nullopt});
        }
    });

    /// B170: each abandoned precommit reclaimed (its owner edge removed; the next fold releases its
    /// blob edges). Records WHY the build was judged dead — the soak's leak/dangle attribution.
    for (const DeadRef & dr : dead_refs)
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::PrecommitReclaim;
            e.namespace_ = ns_str;
            e.ref_name = dr.name;
            e.object_kind = CasEventObjectKind::Root;
            e.outcome = "reclaimed";
            e.reason = w == nullptr
                ? "precommit reclaim: owning server has no watermark (abandoned/vanished build)"
                : (!server_live
                    ? "precommit reclaim: owning server frozen K=2 rounds (crashed build)"
                    : "precommit reclaim: build_seq below the server's min_active floor (retired build)");
            e.detail = {{"build_seq", std::to_string(dr.build_seq)},
                        {"min_active", w ? std::to_string(w->min_active) : "absent"}};
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

}
