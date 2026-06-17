#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <algorithm>
#include <charconv>
#include <limits>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len)
{
    if (kind != ObjectKind::Blob)
        return object_size;   /// trees/packs account whole-object (sizes are GC bookkeeping)
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
    /// owner 0 in GcLease means "never held" — a leader with id 0 would be indistinguishable from it.
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

    /// CRASH-RESUME first: an incomplete prior round (its retired sets still present) is finished
    /// from durable state before any new round starts - the model's idempotent replay ("re-runs
    /// both steps from the durable outcome log over set semantics - no-ops").
    if (tryResumeIncompleteRound(state, state_token, report))
        return report;

    /// R1: fold the journals into a new durable snap generation (cursors advance only after).
    FoldResult folded = fold(state, state_token);   /// non-const: the recheck folds through the fence into the same snap

    /// R2 consumes the EXACT (state, token) the fold's CAS committed - THREADED through, never
    /// re-read. A post-fold re-read would open a zombie window: a lease steal landing between the
    /// fold CAS and the re-read hands this (now stale) leader the thief's state with its bumped
    /// fence_seq, letting stale-snap retire sets land inside the thief's epoch paths - and the
    /// round CAS would ride the post-steal token. With the committed token threaded, any
    /// intervening steal makes the retire round CAS Conflict => ABORTED (fail closed).
    ///
    /// R2: HEAD-observe each candidate's current token, write the round's retire sets, advance
    /// .round. Candidates are derived STATELESSLY from the durable snap (the model GRetire guard
    /// `present ∧ everEdged ∧ InDeg = 0` over zeroInDegreeKnown) - never from the in-memory fold
    /// transitions, so a crash-replayed round re-derives the same set. `folded.transitioned` is
    /// only a health cross-check; the counts can differ legitimately (e.g. a node zeroed by an
    /// EARLIER round's fold is in zeroInDegreeKnown but did not transition in THIS fold).
    const std::map<uint64_t, RetiredSet> retired = retire(state, state_token, folded.snap, report);
    report.round = state.round;
    for (const auto & [snap_shard, set] : retired)
        report.candidates += set.entries.size();

    /// R3: CAS the monotone fence into the registry and EVERY root shard of every namespace in
    /// the FENCE-TIME registry (minting fence-only manifests for absent shards) and persist the
    /// recorded fence versions - the durable point the recheck folds through. `state.round` was
    /// advanced by retire's CAS; `state`/`state_token` are current after it.
    fence(state, state_token);

    /// R4: fold-through-fence recheck + the single content-delete site + outcomes.
    const RecheckResult rechecked = recheck(state, folded.snap, retired, report);

    /// CASCADE (the pipeline step): strip deleted trees, persist the post-strip snap with cursors
    /// advanced to the fence versions (the cascade-vs-recreate ordering), then drop the round's
    /// retired sets on their confirmed outcomes.
    cascadeAndPersist(state, state_token, folded.snap, rechecked, retired, report);

    /// Journal trim - the round's maintenance tail (cursors are durable; INV-JOURNAL-COVERAGE).
    trim(state, folded.root_shards);

    /// Pillar A1: keep the final post-round snap resident for the next round's fold (write-once
    /// generation makes this safe to reuse while we remain the leader at the same generation).
    /// Only refreshed on the normal full-round completion path — NOT on the two early returns
    /// (!acquired_lease and tryResumeIncompleteRound) so the stale-or-absent resident is always
    /// re-validated against state.snap_generation on the next call.
    /// Stored AFTER cascadeAndPersist, so the resident copy already includes the cascade strip and
    /// any fold-through-fence delta cascade persisted — it matches the durable snap at
    /// state.snap_generation. (When cascade did NOT advance the generation, folded.snap equals the
    /// unchanged durable snap, so the pair is still consistent.)
    /// folded is dead after this point (recheck/cascade consumed folded.snap by reference earlier,
    /// trim used only folded.root_shards, and the next line returns), so move instead of copy.
    resident_snap = std::move(folded.snap);
    resident_generation = state.snap_generation;

    return report;
}

void Gc::trim(const GcState & state,
              const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards)
{
    for (const auto & [ns, shard] : root_shards)
    {
        const String cursor_key = ns.string() + "/" + std::to_string(shard);
        const auto cursor_it = state.folded_cursor.find(cursor_key);
        if (cursor_it == state.folded_cursor.end())
            continue;
        const uint64_t cursor = cursor_it->second;

        /// Touch the manifest only when there is something to trim (a peek first - the CAS loop
        /// re-reads anyway, but a per-round no-op version bump on every shard would be noise).
        const auto [root, manifest_token] = store->readShard(ns, shard);
        bool has_trimmable = false;
        for (const JournalRecord & record : root.journal)
            if (record.at_version <= cursor)
            {
                has_trimmable = true;
                break;
            }
        if (!has_trimmable)
            continue;

        store->mutateShard(ns, shard, [&](RootShard & fresh)
        {
            /// INV-JOURNAL-COVERAGE: only records at or below the DURABLE cursor may go - they are
            /// provably incorporated into the durable snap generation. Records above the cursor
            /// (a publish racing this very trim included - mutateShard re-reads per attempt) stay.
            std::erase_if(fresh.journal, [&](const JournalRecord & record)
            {
                return record.at_version <= cursor;
            });
        });
    }
}

Gc::RecheckResult Gc::recheck(const GcState & state, std::map<uint64_t, GcSnap> & snap,
                              const std::map<uint64_t, RetiredSet> & retired, RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    chassert(state.lease.owner == gc_id);

    const uint64_t round = state.round;

    /// A delete for round R requires R's fence vector: without the recorded fence positions the
    /// fold-through-fence coverage below is unprovable and NO delete may fire (fail closed). In
    /// the sequential round this is structurally guaranteed (fence ran just before); the guard
    /// protects future resume paths (Task 12) and out-of-model state.
    const auto fence_it = state.fence_version.find(round);
    if (fence_it == state.fence_version.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS gc recheck: no fence_version recorded for round {} - refusing to delete", round);

    bool fence_window_records_folded = false;

    /// 1. FOLD-THROUGH-FENCE (the model's FoldedThroughFence guard): re-read every fenced shard
    /// and fold the records in (folded_cursor, fence_version] - exactly the publishes that raced
    /// the fence (horn 1 of the no-return argument: a pre-fence publish's record sits at or below
    /// the fence's committed version, so it is folded HERE and re-pins its targets => Spared).
    /// This in-memory delta is NOT persisted: the next round's durable fold re-derives it
    /// idempotently (set semantics); persisting would advance cursors past records the durable
    /// snap generation does not contain.
    for (const auto & [cursor_key, fence_version] : fence_it->second)
    {
        if (cursor_key == "_registry")
            continue;   /// the registry fence orders namespace creation; it carries no journal
        const size_t slash = cursor_key.rfind('/');
        chassert(slash != String::npos);
        const RootNamespace ns{cursor_key.substr(0, slash)};
        uint64_t shard = 0;
        [[maybe_unused]] const auto parse =
            std::from_chars(cursor_key.data() + slash + 1, cursor_key.data() + cursor_key.size(), shard);
        chassert(parse.ec == std::errc());

        const auto [root, manifest_token] = store->readShard(ns, shard);
        const auto cursor_it = state.folded_cursor.find(cursor_key);
        const uint64_t cursor = cursor_it != state.folded_cursor.end() ? cursor_it->second : 0;
        if (fence_version > cursor)
        {
            /// Did any RECORD actually sit in the window? (The fence's own bump is recordless, so
            /// the common idle case folds nothing - the cascade can then skip its snap persist.)
            for (const JournalRecord & record : root.journal)
                if (record.at_version > cursor && record.at_version <= fence_version)
                {
                    fence_window_records_folded = true;
                    break;
                }
            foldShardRecords(snap, state, cursor_key, root, cursor, fence_version);
        }
    }

    /// 2. Per retired entry: spare or delete. The retired sets passed in are the DURABLE sets the
    /// retire step wrote or adopted (a crashed prior attempt's set is the authoritative round
    /// input - the Task-7 contract).
    RecheckResult result;
    result.fence_window_records_folded = fence_window_records_folded;
    std::map<uint64_t, OutcomeLog> computed;
    for (const auto & [snap_shard, set] : retired)
    {
        for (const RetiredEntry & entry : set.entries)
        {
            OutcomeEntry outcome;
            outcome.kind = entry.kind;
            outcome.hash = entry.hash;
            outcome.token = entry.token;

            if (snap.at(hashPrefixShard(entry.hash, state.snap_shards)).inDegree(entry.kind, entry.hash) > 0)
            {
                /// A publish at or below the fence re-pinned it (folded above) - never delete.
                outcome.outcome = OutcomeKind::Spared;
            }
            else
            {
                /// ==================== THE SINGLE CONTENT-DELETE SITE ====================
                /// The ONLY place Cas::Gc ever deletes a content object (blob/tree/pack), and the
                /// only reachability delete in the whole core. All four gates of INV-NO-LOSS hold
                /// here, in order:
                ///   1. the retire entry is DURABLE (written/adopted by R2 before .round advanced);
                ///   2. EVERY root shard of every registered namespace is fenced at versions
                ///      recorded in gc/state.fence_version[round] (R3; the registry fence orders
                ///      namespace creation itself);
                ///   3. the fold-through-fence above re-derived in-degree THROUGH those recorded
                ///      versions and found 0 (no journal record at or below any fence names it);
                ///   4. the delete carries the EXACT token R2 observed - a post-retire publish
                ///      either resurrected (new token => TokenMismatch => Replaced, no loss) or was
                ///      folded above (=> Spared). INV-NO-RETURN: the deleted incarnation's token
                ///      can never be current again (W-FRESH-TAG), so a duplicated/zombie replay of
                ///      this very call is forever harmless.
                const DeleteOutcome deleted =
                    backend.deleteExact(objectKey(layout, entry.kind, entry.hash), entry.token);
                /// ========================================================================
                if (deleted.created_delete_marker)
                    /// Versioning-enabled bucket: the probe exists to reject this pool shape; a
                    /// marker here means the pool is mis-provisioned - fail loud, never continue
                    /// (the "deleted" bytes silently linger as a noncurrent version).
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "CAS gc recheck: delete of {} created a delete marker - versioning is "
                        "enabled on the pool (mis-provisioned; the capability probe must reject this)",
                        u128ToHex(entry.hash));
                switch (deleted.kind)
                {
                    case DeleteOutcome::Kind::Deleted:
                        outcome.outcome = OutcomeKind::Deleted;
                        break;
                    case DeleteOutcome::Kind::NotFound:
                        outcome.outcome = OutcomeKind::Absent;
                        break;
                    case DeleteOutcome::Kind::TokenMismatch:
                        outcome.outcome = OutcomeKind::Replaced;
                        break;
                }
            }
            computed[snap_shard].entries.push_back(std::move(outcome));
        }
    }

    /// 3. Outcome logs - append-by-unique-path with crashed-attempt adoption (the retire-set
    /// pattern): an occupant log is the durable truth of what a PRIOR attempt's deletes actually
    /// did (e.g. it recorded Deleted where our idempotent replay just saw Absent) - adopt it and
    /// discard this attempt's tallies for that shard. Undecodable occupant => ABORTED (corrupt
    /// state never becomes a cascade input).
    for (auto & [snap_shard, log] : computed)
    {
        const String key = layout.outcomesKey(round, state.fence_seq, snap_shard);
        const String body = encodeOutcomeLog(log);
        if (backend.putIfAbsent(key, body) == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc recheck: outcome log at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try
                {
                    log = decodeOutcomeLog(existing->bytes);
                }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc recheck: undecodable outcome log at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
        result.outcomes.emplace(snap_shard, log);
    }

    /// 4. Tallies + the cascade input, derived from the FINAL (written-or-adopted) logs so a
    /// crash-replayed recheck reports and cascades identically. An ABSENT tree cascades too: its
    /// entry is HELD (we are processing it), so the 404 proves OUR OWN crashed delete landed
    /// (spec §7 cascade rule); a Replaced tree NEVER cascades - the new incarnation pins its
    /// children, its own lifecycle handles them.
    for (const auto & [snap_shard, log] : result.outcomes)
    {
        for (const OutcomeEntry & outcome : log.entries)
        {
            switch (outcome.outcome)
            {
                case OutcomeKind::Deleted:
                    ++report.deleted;
                    result.deleted_nodes.push_back(Candidate{outcome.kind, outcome.hash});
                    if (outcome.kind == ObjectKind::Tree)
                        result.deleted_trees.push_back(outcome.hash);
                    break;
                case OutcomeKind::Absent:
                    ++report.absent;
                    result.deleted_nodes.push_back(Candidate{outcome.kind, outcome.hash});
                    if (outcome.kind == ObjectKind::Tree)
                        result.deleted_trees.push_back(outcome.hash);
                    break;
                case OutcomeKind::Replaced:
                    ++report.replaced;
                    break;
                case OutcomeKind::Spared:
                    ++report.spared;
                    break;
            }
        }
    }

    /// Steps 5-7 of the round tail (the strip, the persist, the entry drop) belong to the CASCADE
    /// pipeline step - see cascadeAndPersist, which the orchestration runs right after this.
    return result;
}

void Gc::cascadeAndPersist(GcState & state, Token & state_token, std::map<uint64_t, GcSnap> & snap,
                           const RecheckResult & rechecked,
                           const std::map<uint64_t, RetiredSet> & retired, RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    chassert(state.lease.owner == gc_id);

    const uint64_t round = state.round;
    const auto fence_it = state.fence_version.find(round);
    chassert(fence_it != state.fence_version.end());   /// recheck guarded this; same round state

    /// 1. THE CASCADE STRIP (spec §7 "cascade is a pipeline step, not a foldable record"; the
    /// model's Land does the strip atomically with the landing). For every round-R tree whose
    /// outcome confirmed Deleted - or Absent while its entry was HELD, proving our own crashed
    /// delete landed - strip its child edges and clear its marker. The freed children become
    /// zero-in-degree KNOWN nodes in the durable snap: the NEXT round's stateless candidate scan
    /// retires and deletes them (the model's GRetire runs in the next retiring phase; the spec's
    /// LIVE-RECLAIM bound is "~2 regular rounds"). Replaced trees are NOT in deleted_trees and
    /// NEVER cascade - the new incarnation pins its children. The strip needs no tree read: the
    /// snap recorded the child edges at expansion (all incarnations are payload-identical), and a
    /// never-expanded tree strips as a no-op (it contributed no edges).
    for (const UInt128 & tree : rechecked.deleted_trees)
    {
        const auto freed = snap.at(hashPrefixShard(tree, state.snap_shards)).stripTree(tree);
        report.cascaded += freed.size();
    }

    /// P9: forget every confirmed-gone node (trees AND blobs/packs) from `known`, so the next
    /// round's stateless candidate scan no longer re-derives — and re-HEAD-404s — them. This is the
    /// PRIMARY prune site: a node deleted in round R is out of `known` before R's retired sets drop,
    /// keeping `known` tight by construction. Orthogonal to stripTree above (which clears a deleted
    /// tree's OUTGOING edges/marker; this clears the node's INCOMING `known` membership).
    for (const Candidate & node : rechecked.deleted_nodes)
    {
        snap.at(hashPrefixShard(node.hash, state.snap_shards)).forget(node.kind, node.hash);
        ++report.forgotten_on_delete;
    }

    /// 2. PERSIST the post-strip snap - WITH the recheck's fence-window fold included - and
    /// advance folded_cursor to the recorded fence versions in the SAME gc/state CAS. This is the
    /// pipeline ORDERING rule that closes the cascade-vs-recreate race: a re-create-and-publish
    /// racing the deletion necessarily lands at a shard_version ABOVE this round's fence version,
    /// so the next round folds its Add on a snap whose marker was already CLEARED by the strip -
    /// the re-expansion re-pins the children. Persisting the strip lazily (or not advancing the
    /// cursors with it) would let that Add fold against a still-set marker, skip the re-expansion,
    /// and leave the recreated live tree's children edge-less - an under-count, a wrong delete.
    /// Same write-once probe-upward discipline as the fold's persist. SKIPPED when the snap is
    /// semantically unchanged since the fold's persist (no strips, no fence-window records - the
    /// common idle case): persisting a byte-equivalent-except-generation snap every round would
    /// mint an orphan object per round forever for nothing. The closing gc/state CAS below still
    /// runs unconditionally (cursors advance to the fence versions - vacuous coverage when the
    /// window held no records - and fence_version[<= round] is erased).
    const bool snap_changed = !rechecked.deleted_trees.empty() || rechecked.fence_window_records_folded
        || report.forgotten_on_delete > 0;   /// P9: a blob-only prune round must still persist the snap
    constexpr uint64_t max_generation_probes = 1000;
    uint64_t adopted_generation = state.snap_generation;
    if (snap_changed)
    for (uint64_t generation = state.snap_generation + 1; ; ++generation)
    {
        if (generation > state.snap_generation + max_generation_probes)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS gc cascade: no adoptable snap generation within {} probes above {} (runaway divergence)",
                max_generation_probes, state.snap_generation);
        bool diverged = false;
        for (auto & [snap_shard, shard_snap] : snap)
        {
            shard_snap.generation = generation;
            const String snap_key = layout.gcSnapKey(generation, snap_shard);
            const String body = encodeGcSnap(shard_snap);
            if (backend.putIfAbsent(snap_key, body) == PutOutcome::PreconditionFailed)
            {
                const auto existing = backend.get(snap_key);
                if (!existing || existing->bytes != body)
                {
                    diverged = true;
                    break;
                }
            }
        }
        if (!diverged)
        {
            adopted_generation = generation;
            break;
        }
    }

    GcState next = state;
    next.snap_generation = adopted_generation;   /// unchanged when the persist was skipped
    for (const auto & [cursor_key, fence_version] : fence_it->second)
    {
        if (cursor_key == "_registry")
            continue;
        auto it = next.folded_cursor.find(cursor_key);
        if (it != next.folded_cursor.end())
            it->second = std::max(it->second, fence_version);
        else
            next.folded_cursor[cursor_key] = fence_version;
    }
    /// fence_version[<= round] served its recheck - erase it (gc/state must not grow forever).
    std::erase_if(next.fence_version, [&](const auto & kv) { return kv.first <= round; });
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token, &committed_token)
        != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc cascade: gc/state moved during the cascade persist (another leader advanced it); "
            "retry next round");
    state = std::move(next);
    state_token = committed_token;

    /// 3. The entries "drop on confirmed outcomes": with the outcomes durable AND the cascade
    /// persisted, the round's retired-set objects are deleted (a GC-METADATA delete, not the
    /// content-delete site - these are gc/retired/ bookkeeping objects GC itself wrote).
    /// RetireView tolerates a list-then-get disappearance, and from this moment the writer-facing
    /// barrier for these incarnations is gone - correct: every entry has a confirmed outcome
    /// (deleted/absent incarnations cannot return, INV-NO-RETURN; replaced/spared ones are alive
    /// at a NEWER token that was never condemned). A crash BEFORE this loop leaves the sets
    /// lingering with their outcomes durable - the resume rule (Task 12) detects exactly that
    /// (retired sets at rounds <= state.round whose outcome logs exist), re-applies the idempotent
    /// strip from the logs, and drops the sets.
    for (const auto & [snap_shard, set] : retired)
    {
        const String key = layout.retiredKey(round, state.fence_seq, snap_shard);
        if (const auto got = backend.get(key))
        {
            const DeleteOutcome dropped = backend.deleteExact(key, got->token);
            if (dropped.kind == DeleteOutcome::Kind::TokenMismatch)
                /// Nobody else legally writes this path (fence_seq epoch isolation) - fail loud.
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "CAS gc cascade: retired set at {} changed under us (token mismatch on drop)", key);
        }
    }
}

void Gc::fence(GcState & state, Token & state_token)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    /// Same no-re-read contract as retire: the state we fence under is the one OUR lease committed.
    chassert(state.lease.owner == gc_id);

    /// The round being executed: retire's CAS already advanced state.round to it.
    const uint64_t round = state.round;

    /// 0. CAS the NAMESPACE REGISTRY's fence_round first (spec §7 R3, decision 2026-06-12). This
    /// is the ordering point for namespace CREATION: a writer registering a namespace BELOW this
    /// fence is in this round's discovery (its shards are fenced below); one registering AFTER it
    /// observes fence_round >= round and must refresh its retire view before its first publish
    /// (W-REGISTER gate floor) - the two-horn argument at namespace granularity. The committed
    /// registry_version is recorded under the reserved "_registry" key (checkNamespace forbids it
    /// as a namespace segment, so it can never collide with an "ns/shard" entry).
    ///
    /// The registry decoded in the COMMITTED attempt is ALSO the shard-fence universe below. It
    /// must be: the fold's registry read is STALE by fence time, and a namespace registered in
    /// the window between them would fall into the hole BETWEEN the horns - registered below this
    /// registry fence (so its writer never observes a fence_round floor, horn 2 misses) yet
    /// absent from the fold-time universe (so its shards are never minted/fenced and the recheck
    /// never folds their journals, horn 1 misses). A stale-view publish into such a namespace
    /// could then re-reference a condemned hash past the recheck => the exact-token delete
    /// dangles a live ref. The registry is CAS-append-only, so the committed-attempt universe is
    /// a superset of the fold-time one - fencing MORE shards only folds more and spares more.
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
            const CasOutcome outcome = got
                ? backend.casPut(registry_key, encodeRootsRegistry(registry), got->token)
                : backend.casPut(registry_key, encodeRootsRegistry(registry), std::nullopt);
            if (outcome == CasOutcome::Committed)
            {
                state.fence_version[round]["_registry"] = registry.registry_version;
                fence_universe = std::move(registry);
                registry_fenced = true;
            }
            /// Conflict: a racing namespace registration - re-read and retry (their append is then
            /// either below our fence commit, i.e. in the universe the NEXT round discovers, or
            /// they observed our fence_round and took the gate floor).
        }
        if (!registry_fenced)
            throw Exception(ErrorCodes::ABORTED,
                "CAS gc fence: registry CAS contention (runaway live-lock brake)");
    }

    /// 1. CAS fence_round := max(fence_round, round) into EVERY root shard of EVERY namespace in
    /// the FENCE-TIME registry (the committed attempt above - NOT the fold-time universe; see the
    /// hole described there) - present or ABSENT shards alike.
    /// For an absent shard, mutateShard's create-if-absent CAS MINTS a fence-only manifest
    /// (fence_round = round, empty refs/journal): the create race against a first publish into
    /// that shard is exactly the required total order (whoever loses the CAS re-reads - the
    /// publisher then sees fence_round = round and its gate refreshes). Fencing only PRESENT
    /// shards would leave that first publish unordered against the fence - the absent-shard hole.
    /// The mutate lambda runs on the FRESHLY READ manifest on every CAS attempt (mutateShard
    /// re-reads inside its loop), and the max makes a stale leader's lower round get ABSORBED,
    /// never lower the fence - the model's monotone fence (GFenceShard, INV-MONOTONE-GC).
    ///
    /// THE TWO HORNS of the no-return argument (spec section 7, steps 2-4) both rest on the
    /// manifest CAS totally ordering this fence against publishes on the shard:
    ///   horn 1 - a publish racing the fence conflicts one of the two CASes; if the publish wins,
    ///     mutateShard re-reads and retries, so the publish's journal record lands at a version
    ///     strictly BELOW the fence's committed version => the recheck (which folds through the
    ///     recorded version) SEES it and spares the resurrected object;
    ///   horn 2 - a publish AFTER the fence reads a manifest carrying fence_round = round => the
    ///     writer gate (Build::publish, W-PUBLISH-GATE) refreshes its RetireView and revalidates
    ///     against the new round's retired entries before committing.
    /// Either way no publish can slip a reference past both the fence and the recheck.
    ///
    /// The fence bumps shard_version but appends NO journal record: a version bump with no record
    /// is a fold no-op (the fold reads records, not versions), and INV-JOURNAL-COVERAGE is
    /// untouched - trim (Task 11) is gated on at_version <= folded_cursor, and a fence bump above
    /// the cursor leaves no record to trim.
    const uint64_t root_shards_per_ns = store->poolMeta().root_shards;
    for (const String & ns_name : fence_universe.namespaces)
    {
        const RootNamespace ns{ns_name};
        for (uint64_t shard = 0; shard < root_shards_per_ns; ++shard)
        {
            uint64_t committed = 0;
            store->mutateShard(ns, shard, [&](RootShard & root)
            {
                root.fence_round = std::max(root.fence_round, round);
            }, &committed);

            /// RE-FENCING ON REPLAY (a crashed round resumed, Task 12): the manifest CAS re-applies
            /// the max (a no-op on fence_round if already fenced at this round) but still BUMPS
            /// shard_version, so the recorded version DIFFERS from the first attempt's - it is HIGHER.
            /// Safe in the provable-coverage direction: the recheck must fold THROUGH at least the
            /// recorded version, and recording the higher replayed version is MORE conservative
            /// (folds more, never less).
            state.fence_version[round][ns.string() + "/" + std::to_string(shard)] = committed;
        }
    }

    /// 2. ONE gc/state CAS persists the whole fence_version[round] vector (everything else in
    /// `state` preserved - it is the retire-committed state mutated in place).
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token, &committed_token)
        == CasOutcome::Committed)
    {
        state_token = committed_token;
        return;
    }

    /// Conflict: gc/state moved under us between retire's CAS and this one. Re-read ONCE to
    /// sharpen the message, then fail closed (ABORTED) - bounded, never a blind retry.
    const auto current = backend.get(layout.gcStateKey());
    if (!current)
        /// gc/state is never legally deleted once created (see acquireOrRenewLease).
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc fence: gc/state vanished during the fence");
    const GcState observed = decodeGcState(current->bytes);
    if (observed.lease.owner != gc_id)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fence: lease lost during fence (stolen by {}); retry next round",
            u128ToHex(observed.lease.owner));
    /// Lease still ours yet the token moved: a racing fold of our own is impossible (one pacing
    /// thread per Gc instance) - this is a split-brain zombie of OURSELVES (another process with
    /// our gc_id, a caller-obligation violation) or out-of-model tooling. Fail closed; the
    /// manifest fences already written are monotone and idempotent - the next round re-fences
    /// and re-records (the higher replayed versions are the MORE conservative coverage).
    throw Exception(ErrorCodes::ABORTED,
        "CAS gc fence: gc/state moved during the fence while the lease is still ours; retry next round");
}

std::map<uint64_t, RetiredSet> Gc::retire(GcState & state, Token & state_token,
                                          std::map<uint64_t, GcSnap> & snap, RoundReport & report)
{
    const Layout & layout = store->layout();
    Backend & backend = store->backend();

    /// Belt-and-braces for the no-re-read contract: the state retire runs under must be the one
    /// OUR lease committed (the caller threads the fold-committed state; a thief's state would
    /// carry a foreign owner).
    chassert(state.lease.owner == gc_id);

    /// state.round = "highest round whose retire sets are durable" => THIS round is the next one.
    const uint64_t round = state.round + 1;

    /// Reset the per-round watermark caches before observing candidates. The guard below
    /// (protectedByLiveBuild) judges each owning server's liveness ONCE per round; the across-round
    /// frozen-seq memory persists for the K=2 crash detector.
    beginWatermarkRound();

    /// 1. Observe. One HEAD per candidate captures the CURRENT incarnation token - the only token
    /// the eventual delete may carry (exact-token delete, spec §7).
    ///
    /// Cross-round duplicates are possible and benign: a prior round may have retired the same
    /// (hash, token) and crashed before its recheck/outcomes - this round re-derives the candidate
    /// (still known + zero + present) and writes ANOTHER entry under its own unique path. The
    /// RetireView unions all present sets (the same condemned token twice), the delete is
    /// exact-token idempotent, and stale sets are cleaned by Task 9: a round's retired-set objects
    /// are deleted after its outcome log is durable -
    /// so no cross-round dedup is attempted here. Within one round a candidate appears once by
    /// construction (zeroInDegreeKnown yields unique nodes; a node lives in exactly one shard).
    std::map<uint64_t, RetiredSet> retired;
    for (auto & [snap_shard, shard_snap] : snap)
    {
        for (const Candidate & candidate : shard_snap.zeroInDegreeKnown())
        {
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            if (!observed.exists)
            {
                /// P9 defensive prune. The object is gone but its node still sits in `known`
                /// (in-degree 0); forget it so the next round's stateless scan stops re-deriving and
                /// re-HEAD-404ing it. In correct single-leader operation the delete-time prune (the
                /// cascade) already keeps `known` tight, so this path is rare; it self-heals the
                /// cases that prune cannot reach from THIS leader's snapshot: a stale leader
                /// observing a node a live leader already deleted (split-brain — the lease is
                /// work-dedup only, by design), the crash/resume window before a delete-time prune is
                /// durable, and any out-of-band deletion. `exists == false` is a GENUINE 404
                /// (getObjectInfoIfExists returns absent only for NO_SUCH_KEY/NO_SUCH_BUCKET/
                /// RESOURCE_NOT_FOUND; every other backend error throws and aborts the round), so a
                /// transient error never masquerades as absence — we never forget a live node. NOT a
                /// LOGICAL_ERROR: throwing here would crash on benign split-brain races. The prune is
                /// in-memory here; the cascade persists the same threaded snap (a crash before then
                /// re-HEAD-404s on replay and re-forgets — idempotent, set semantics). Mutating
                /// `shard_snap` here does not invalidate iteration: zeroInDegreeKnown() returned a
                /// value (a snapshot copy of the candidate set).
                shard_snap.forget(candidate.kind, candidate.hash);
                ++report.forgotten_absent;
                continue;
            }

            if (protectedByLiveBuild(observed.attributes))
                continue;   // owned by a live build -> skip condemn this round (non-destructive deferral)

            RetiredEntry entry;
            entry.kind = candidate.kind;
            entry.hash = candidate.hash;
            entry.token = observed.token;
            entry.size = retiredLogicalSize(candidate.kind, observed.size, store->poolMeta().blob_header_len);
            retired[hashPrefixShard(candidate.hash, state.snap_shards)].entries.push_back(std::move(entry));
        }
    }

    /// 2. Write each shard's set - append-by-unique-path: the path is unique per
    /// (round, fence_seq, shard) and written ONCE (write-once, never rewritten). An EMPTY shard
    /// writes nothing (no empty objects; RetireView handles absent keys), and an all-empty round
    /// still advances .round below - the round RAN; the common idle case, with a trivially empty
    /// recheck.
    ///
    /// AN OCCUPIED PATH IS OUR OWN CRASHED PRIOR ATTEMPT. With the fold-committed state threaded
    /// (no re-read window) and fence_seq isolation (a lease steal bumps fence_seq, changing every
    /// path), nobody but THIS leader in THIS epoch ever writes this key. Byte-identical content is
    /// the exact replay - proceed. DIVERGENT content is the same round derived from an OLDER snap:
    /// the crash window admitted new journal activity or token displacement before this replay -
    /// ADOPT the occupant as this round's set (never rewrite). Safety: the occupant's entries were
    /// derived from an older durable snap of the same pool and are conservative - retired != dead,
    /// the recheck re-validates in-degree through the fence before any delete, and a stale
    /// observed token just makes the delete 412 => outcome replaced. Candidates the occupant lacks
    /// are NOT lost: zeroInDegreeKnown is stateless, the next round re-derives and retires them.
    /// Aborting here instead would WEDGE this leader forever - renewal never bumps fence_seq, so
    /// every later round would re-hit the same occupied path (the same problem class the fold's
    /// generation probe-upward solves). Fail closed (ABORTED) only when the occupant cannot be
    /// decoded - corrupt state must never be adopted as a delete input.
    for (auto & [snap_shard, set] : retired)
    {
        const String key = layout.retiredKey(round, state.fence_seq, snap_shard);
        const String body = encodeRetiredSet(set);
        if (backend.putIfAbsent(key, body) == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc retire: retired set at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try
                {
                    set = decodeRetiredSet(existing->bytes);
                }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc retire: undecodable occupant at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
    }

    /// 3. The durable "retire phase complete" marker: ONE gc/state CAS advances .round, strictly
    /// AFTER the sets are durable (INV-MONOTONE-GC ordering: a writer whose RetireView refreshes
    /// at the new round is guaranteed to see the entries). A crash between step 2 and here leaves
    /// only re-derivable durable state - the replay adopts the byte-equal sets and re-runs this CAS.
    GcState next = state;
    next.round = round;
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token, &committed_token)
        != CasOutcome::Committed)
    {
        /// Another leader moved gc/state under us. Bounded and fail-closed: this leader's round
        /// attempt ends here (the lease makes contention rare); the re-read only sharpens the
        /// message - a round already at/past ours means a competitor COMPLETED this retire
        /// (duplicate work detected), anything else is a plain interleaving.
        const auto current = backend.get(layout.gcStateKey());
        if (current && decodeGcState(current->bytes).round >= round)
            throw Exception(ErrorCodes::ABORTED,
                "CAS gc retire: another leader already completed retire round {}; retry next round", round);
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc retire: gc/state moved during retire (another leader advanced it); retry next round");
    }
    state = std::move(next);
    state_token = committed_token;
    return retired;
}

std::vector<Candidate> Gc::foldShardRecords(std::map<uint64_t, GcSnap> & snap, const GcState & state,
                                            const String & cursor_key, const RootShard & root,
                                            uint64_t lo_exclusive, uint64_t hi_inclusive)
{
    std::vector<Candidate> transitioned;

    /// Edges live in the TARGET's snap shard (in-degree is intra-shard); the expansion marker
    /// lives in the TREE's own home shard. std::map node references are stable across inserts.
    const auto shard_for = [&](const UInt128 & hash) -> GcSnap &
    {
        return snap.at(hashPrefixShard(hash, state.snap_shards));
    };

    for (size_t record_idx = 0; record_idx < root.journal.size(); ++record_idx)
    {
        const JournalRecord & record = root.journal[record_idx];
        if (record.at_version <= lo_exclusive || record.at_version > hi_inclusive)
            continue;

        if (record.op == JournalRecord::Op::Add)
        {
            /// Last-op-wins root edge (spec §7): a republish of an existing ref produces
            /// consecutive Adds for the same (root_shard, part_name) with DIFFERENT trees and
            /// no Remove between - addRootEdge re-points the edge and returns the displaced
            /// old target if it zeroed.
            auto displaced = shard_for(record.tree_id).addRootEdge(cursor_key, record.ref_name, record.tree_id);
            transitioned.insert(transitioned.end(), displaced.begin(), displaced.end());

            /// Once-per-tree expansion: the FIRST '+' to a tree with no marker reads the tree
            /// once and adds its child-edge set (each edge into the CHILD's shard).
            GcSnap & tree_home = shard_for(record.tree_id);
            if (!tree_home.isExpanded(record.tree_id))
            {
                std::vector<TreeEntry> entries;
                bool tree_present = true;
                try
                {
                    entries = store->readTree(TreeId(u128ToHex(record.tree_id)));
                }
                catch (const Exception & e)
                {
                    if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
                        throw;
                    /// DECISION (Task 6, refined by review): a journal Add implies the tree was
                    /// published, but the object may legitimately be gone NOW - this record's
                    /// edge was DISPLACED later in the journal and an already COMPLETED
                    /// competing round deleted the displaced tree (in-order folding still
                    /// visits the Add). Displacement proof is a later record for the SAME
                    /// ref_name: a Remove drops the edge, and a later Add re-points it
                    /// (last-op-wins republish - the COMMON mutation path). Either => skip the
                    /// expansion (the edges would be stripped/displaced anyway; the marker
                    /// stays unset; no edges were added for a gone tree, so over-count-only is
                    /// preserved). No later record for the ref => the manifest claims a LIVE
                    /// ref to a missing tree => INV-NO-DANGLE surfaced; fail closed
                    /// (propagate). The op check stays EXPLICIT: a future journal op (e.g. the
                    /// model's fence records) must NOT count as displacement.
                    bool displaced_later = false;
                    for (size_t ahead = record_idx + 1; ahead < root.journal.size(); ++ahead)
                    {
                        const JournalRecord & next = root.journal[ahead];
                        if ((next.op == JournalRecord::Op::Remove || next.op == JournalRecord::Op::Add)
                            && next.ref_name == record.ref_name && next.at_version > record.at_version)
                        {
                            displaced_later = true;
                            break;
                        }
                    }
                    if (!displaced_later)
                        throw;
                    tree_present = false;
                }

                if (tree_present)
                {
                    for (const TreeEntry & entry : entries)
                    {
                        switch (entry.placement)
                        {
                            case Placement::Blob:
                                shard_for(entry.file_hash).addTreeEdge(record.tree_id, ObjectKind::Blob, entry.file_hash);
                                break;
                            case Placement::Subtree:
                                shard_for(entry.file_hash).addTreeEdge(record.tree_id, ObjectKind::Tree, entry.file_hash);
                                break;
                            case Placement::PackSlice:
                                shard_for(entry.pack_hash).addPackEdge(record.tree_id, entry.pack_hash);
                                break;
                            case Placement::Inline:
                                break;   /// embedded bytes - no separate object, no edge
                        }
                    }
                    tree_home.markExpanded(record.tree_id);
                }
            }
        }
        else
        {
            /// The model's GFold rem only removes the root edge - the cascade strip of the
            /// tree's child edges belongs to the recheck/delete pipeline (Task 10).
            /// LOAD-BEARING routing: the Remove routes via the journal record's DROP-TIME
            /// tree_id, which by in-order folding is exactly the shard where the live
            /// (root_shard, part_name) edge resides - any earlier displacing Add already
            /// re-pointed the edge when ITS record folded (intra-shard while snap_shards == 1).
            auto cands = shard_for(record.tree_id).removeRootEdge(cursor_key, record.ref_name);
            transitioned.insert(transitioned.end(), cands.begin(), cands.end());
        }
    }

    return transitioned;
}

std::vector<std::pair<RootNamespace, uint64_t>> Gc::discoverUniverse()
{
    /// FROM THE REGISTRY - never LIST. The registry is the authoritative universe: writers
    /// CAS-append a namespace before its first manifest exists (W-REGISTER), so every manifest GC
    /// must order against is reachable from here. The universe is namespaces x ALL root_shards
    /// shards (present or absent) - the fence needs the absent ones too (it mints fence-only
    /// manifests there; fencing only present shards leaves a first publish into an absent shard
    /// totally unordered). A manifest of an UNREGISTERED namespace is out-of-model (production
    /// writers always register; fixtures call registerNamespaceRaw) and invisible by contract.
    /// An absent registry is the fresh-pool case: empty universe.
    std::vector<std::pair<RootNamespace, uint64_t>> universe;
    if (const auto got = store->backend().get(store->layout().rootsRegistryKey()))
    {
        const RootsRegistry registry = decodeRootsRegistry(got->bytes);
        const uint64_t root_shards_per_ns = store->poolMeta().root_shards;
        for (const String & ns : registry.namespaces)
            for (uint64_t shard = 0; shard < root_shards_per_ns; ++shard)
                universe.emplace_back(RootNamespace{ns}, shard);
    }
    return universe;
}

std::map<uint64_t, GcSnap> Gc::loadSnap(const GcState & state)
{
    /// An absent shard object is an EMPTY snap, not an error: generation 0 of a fresh pool has no
    /// objects at all (and the very first fold legitimately starts from nothing).
    std::map<uint64_t, GcSnap> snap;
    for (uint64_t snap_shard = 0; snap_shard < state.snap_shards; ++snap_shard)
    {
        if (const auto got = store->backend().get(store->layout().gcSnapKey(state.snap_generation, snap_shard)))
            snap.emplace(snap_shard, decodeGcSnap(got->bytes));
        else
        {
            GcSnap empty;
            empty.snap_shard = snap_shard;
            empty.generation = state.snap_generation;
            snap.emplace(snap_shard, std::move(empty));
        }
    }
    return snap;
}

std::vector<Gc::PreviewEntry> Gc::previewDeletes()
{
    std::vector<PreviewEntry> out;

    const auto state_bytes = store->backend().get(store->layout().gcStateKey());
    if (!state_bytes)
        return out;   /// no GC state yet => nothing retired
    const GcState state = decodeGcState(state_bytes->bytes);

    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    std::map<uint64_t, GcSnap> snap = loadSnap(state);

    for (const auto & [snap_shard, shard_snap] : snap)
    {
        for (const Candidate & candidate : shard_snap.zeroInDegreeKnown())
        {
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            if (!observed.exists)
                continue;   /// absent => already reclaimed; nothing to preview
            PreviewEntry e;
            e.kind = candidate.kind;
            e.hash = candidate.hash;
            e.key = objectKey(layout, candidate.kind, candidate.hash);
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

    /// Detect: retired sets still present at (round, fence_seq) - they drop only at the end of a
    /// COMPLETED round, so their presence is the durable incompleteness signal.
    std::map<uint64_t, RetiredSet> retired;
    for (uint64_t snap_shard = 0; snap_shard < state.snap_shards; ++snap_shard)
        if (const auto got = backend.get(layout.retiredKey(round, state.fence_seq, snap_shard)))
            retired.emplace(snap_shard, decodeRetiredSet(got->bytes));
    if (retired.empty())
        return false;

    report.round = round;
    for (const auto & [snap_shard, set] : retired)
        report.candidates += set.entries.size();

    /// The round's fold already committed (retire's CAS advanced .round AFTER the fold's CAS);
    /// the candidates come from the durable sets - no re-fold, just load the durable snap.
    std::map<uint64_t, GcSnap> snap = loadSnap(state);

    /// Re-fence when the durable state lacks the round's vector (a crash between retire and the
    /// fence's gc/state CAS, or after the cascade erased it but before the sets dropped). The
    /// monotone max makes re-fencing idempotent; the re-recorded HIGHER versions are MORE
    /// conservative coverage (the recheck folds a larger window and can only spare more).
    if (!state.fence_version.contains(round))
        fence(state, state_token);

    const RecheckResult rechecked = recheck(state, snap, retired, report);
    cascadeAndPersist(state, state_token, snap, rechecked, retired, report);
    trim(state, discoverUniverse());
    return true;
}

Gc::FoldResult Gc::fold(GcState & state, Token & state_token)
{
    /// M-C3 limitation, fail closed: last-op-wins displacement is INTRA-shard only (addRootEdge
    /// re-points the edge inside ONE GcSnap). With the displaced and displacing trees in DIFFERENT
    /// snap shards, a republish would leave the old root edge behind forever - a permanent leak
    /// (over-count-safe, but silently broken sharding). Raising the constant needs a design first:
    /// per-ref displacement records, or root edges sharded by REF identity rather than target hash.
    if (state.snap_shards != 1)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS gc fold: snap_shards = {} - cross-shard last-op-wins displacement is undesigned; "
            "snap_shards must be 1 in M-C3", state.snap_shards);

    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    FoldResult result;

    /// 1. Discover the namespace universe FROM THE REGISTRY (spec section 4/7 R1).
    result.root_shards = discoverUniverse();

    /// 2. Load the authoritative snap generation — or reuse the resident cache when the durable
    /// generation is unchanged. A snap generation is write-once (putIfAbsent + byte-equal adoption),
    /// so a matching resident_generation == state.snap_generation guarantees identical bytes without
    /// any HEAD/GET. This avoids the per-round whole-snap GET on idle (churn-free) rounds.
    if (resident_snap && resident_generation == state.snap_generation)
        result.snap = *resident_snap;
    else
        result.snap = loadSnap(state);

    /// 3. Fold each discovered root shard's journal records in (folded_cursor, shard_version],
    /// in journal (= insertion) order (the shared R1/R4 record semantics — foldShardRecords).
    bool folded_any = false;
    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = ns.string() + "/" + std::to_string(root_shard);
        const auto cursor_it = state.folded_cursor.find(cursor_key);
        const uint64_t cursor = cursor_it != state.folded_cursor.end() ? cursor_it->second : 0;

        /// A version bump alone (e.g. from the trim step) does not mean new records exist —
        /// the trim CAS advances shard_version without adding journal entries, so
        /// (shard_version > cursor) can be true while the journal window is empty.
        /// Check the journal directly: any record in (cursor, shard_version] is genuine new work.
        const bool shard_has_new_records = std::any_of(
            root.journal.begin(), root.journal.end(),
            [&](const JournalRecord & r)
            {
                return r.at_version > cursor && r.at_version <= root.shard_version;
            });
        if (shard_has_new_records)
            folded_any = true;   /// genuine new journal records exist in (cursor, shard_version]

        auto transitioned = foldShardRecords(result.snap, state, cursor_key, root, cursor, root.shard_version);
        result.transitioned.insert(result.transitioned.end(), transitioned.begin(), transitioned.end());

        /// Don't mint zero cursor entries for absent shards (the registry universe includes every
        /// shard of every namespace; an absent shard has nothing folded and nothing to remember).
        if (root.shard_version > 0 || cursor > 0)
            state.folded_cursor[cursor_key] = root.shard_version;
    }

    /// No new records since the last fold: the snap is unchanged and already durable at
    /// state.snap_generation. Skip the whole-snap re-write AND fold's own gc/state CAS.
    /// state_token is returned UNMODIFIED, so retire's round CAS rides the incoming lease token
    /// (preserving the zombie-steal protection runRegularRound documents). state.folded_cursor may
    /// have been advanced to shard_version for empty-window shards; that advance is over an empty
    /// (cursor, shard_version] range (no records to skip — same as the original always-persist code
    /// did every round) and is persisted atomically with .round by retire's subsequent CAS.
    if (!folded_any)
        return result;

    /// 4. Persist, durable-before-cursor: the new-generation snap objects go durable FIRST; only
    /// then does the gc/state CAS advance snap_generation + folded_cursor. A crash between the two
    /// loses nothing - the old generation stays authoritative.
    ///
    /// THE WRITE GENERATION PROBES UPWARD. Generation objects are write-once, and a generation key
    /// occupied by DIFFERENT bytes can never be reused: a fold that wrote its snaps but lost the
    /// gc/state CAS leaves an orphan at snap_generation+1, and once ANY new journal record arrives
    /// every later fold covers a larger range - its bytes can never match the orphan again, so a
    /// FIXED write generation would abort forever (a permanent GC wedge). Per probed generation:
    /// every shard Done or byte-equal => adopt it (byte-equality keeps crash-replay adoption: the
    /// orphan of OUR identical earlier attempt is reused, not abandoned); ANY shard divergent =>
    /// abandon the generation (partially written objects there are orphans - harmless, write-once,
    /// never referenced by any cursor; full-GC reclaims them in M-F) and retry one higher. The
    /// adopted generation is > the snap_generation we read and the gc/state CAS below guards the
    /// advance, so INV-MONOTONE-GC holds; generations need NOT be dense - the gc/state pointer is
    /// authoritative, a reader never enumerates generations.
    constexpr uint64_t max_generation_probes = 1000;   /// runaway brake, never a legitimate bound
    uint64_t adopted_generation = 0;
    for (uint64_t generation = state.snap_generation + 1; ; ++generation)
    {
        if (generation > state.snap_generation + max_generation_probes)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS gc fold: no adoptable snap generation within {} probes above {} (runaway divergence)",
                max_generation_probes, state.snap_generation);

        bool diverged = false;
        for (auto & [snap_shard, snap] : result.snap)
        {
            snap.generation = generation;
            const String snap_key = layout.gcSnapKey(generation, snap_shard);
            const String body = encodeGcSnap(snap);
            if (backend.putIfAbsent(snap_key, body) == PutOutcome::PreconditionFailed)
            {
                const auto existing = backend.get(snap_key);
                if (!existing || existing->bytes != body)
                {
                    diverged = true;
                    break;
                }
                /// byte-equal: OUR replay (a prior attempt crashed or lost the cursor CAS) - adopt.
            }
        }
        if (!diverged)
        {
            adopted_generation = generation;
            break;
        }
    }

    state.snap_generation = adopted_generation;
    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token, &committed_token)
        != CasOutcome::Committed)
        /// Another leader advanced gc/state under us. The new-generation snap we just wrote is
        /// orphaned garbage - harmless (write-once, never referenced by any cursor); full-GC
        /// reclaims it in M-F.
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (another leader advanced it); retry next round");

    /// The committed token is threaded to retire - the round CAS rides exactly this incarnation,
    /// so any intervening lease steal Conflicts there (no re-read window, see runRegularRound).
    state_token = committed_token;
    return result;
}

void Gc::rememberObservation(const GcLease & lease)
{
    has_observation = true;
    last_seen_owner = lease.owner;
    last_seen_seq = lease.seq;
}

void Gc::pulseHeartbeat(Store & store, UInt128 gc_id)
{
    /// B160 advisory liveness pulse: bump gc/hb to {gc_id, hb_seq+1}. Touches NO Gc instance state,
    /// so the scheduler's separate heartbeat thread may call it concurrently with the round thread.
    /// Best-effort: a Conflict means another writer raced us; skip — the next pulse retries.
    const String key = store.layout().gcHbKey();
    const auto got = store.backend().get(key);
    GcHeartbeat hb;
    std::optional<Token> expected;
    if (got)
    {
        hb = decodeGcHeartbeat(got->bytes);
        expected = got->token;
    }
    hb.owner = gc_id;   /// we believe we are the leader; take/keep gc/hb ownership
    ++hb.hb_seq;
    store.backend().casPut(key, encodeGcHeartbeat(hb), expected, /*out_token=*/nullptr);
}

void Gc::beginWatermarkRound()
{
    /// Per-round caches reset; the across-round frozen-seq memory (last_seen_server_seq,
    /// server_frozen_rounds) persists — it is the K=2 crash detector's accumulator.
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

    /// Absent watermark => unrecognized owner. Cache nothing (cheap re-HEAD next candidate) and
    /// report nullptr — protectedByLiveBuild then defaults this object to unprotected.
    const HeadResult head = backend.head(key);
    if (!head.exists)
        return nullptr;

    const auto got = backend.get(key);
    if (!got)
        /// Disappeared between HEAD and GET (a farewell/restart raced us) — treat as unrecognized.
        return nullptr;

    const ServerWatermark w = decodeServerWatermark(got->bytes);
    const auto [it, _] = watermark_cache.emplace(server_id, w);

    /// Liveness verdict — computed ONCE here, on the first fetch of this server this round.
    bool live;
    const auto seen_it = last_seen_server_seq.find(server_id);
    if (seen_it == last_seen_server_seq.end())
    {
        /// Never seen before: need a second observation before we can judge it dead, so treat as
        /// LIVE this round and start tracking its seq.
        live = true;
        last_seen_server_seq[server_id] = w.seq;
        server_frozen_rounds[server_id] = 0;
    }
    else if (w.seq != seen_it->second)
    {
        /// seq advanced since last round => the server renewed => LIVE; reset the frozen counter.
        live = true;
        server_frozen_rounds[server_id] = 0;
        seen_it->second = w.seq;
    }
    else
    {
        /// seq unchanged => frozen this round. Dead iff frozen for K=2 consecutive rounds.
        const uint64_t frozen = ++server_frozen_rounds[server_id];
        live = frozen < 2;
    }
    server_live_this_round[server_id] = live;

    return &it->second;
}

bool Gc::protectedByLiveBuild(const ObjectMeta & meta)
{
    /// The owner triple lives under the fixed "cas_owner" metadata key. Absent => unprotected
    /// (a pre-watermark object, or one written without an owner) — the safe pre-watermark default.
    const auto owner_it = meta.find("cas_owner");
    if (owner_it == meta.end())
        return false;

    /// Parse "<server_hex>:<epoch>:<build_seq>". Anything not EXACTLY this shape => unprotected
    /// (fail closed to the pre-watermark default — never fabricate a protection).
    const String & owner = owner_it->second;
    const auto first_colon = owner.find(':');
    if (first_colon == String::npos)
        return false;
    const auto second_colon = owner.find(':', first_colon + 1);
    if (second_colon == String::npos)
        return false;
    /// A third colon means a malformed (over-long) triple.
    if (owner.find(':', second_colon + 1) != String::npos)
        return false;

    const String server_hex = owner.substr(0, first_colon);
    const std::string_view epoch_str(owner.data() + first_colon + 1, second_colon - first_colon - 1);
    const std::string_view seq_str(owner.data() + second_colon + 1, owner.size() - second_colon - 1);
    if (server_hex.size() != 32 || epoch_str.empty() || seq_str.empty())
        return false;

    UInt128 server;
    try
    {
        server = hexToU128(server_hex);
    }
    catch (...)
    {
        return false;
    }

    uint64_t owner_epoch = 0;
    uint64_t owner_seq = 0;
    {
        const auto e = std::from_chars(epoch_str.data(), epoch_str.data() + epoch_str.size(), owner_epoch);
        if (e.ec != std::errc{} || e.ptr != epoch_str.data() + epoch_str.size())
            return false;
        const auto s = std::from_chars(seq_str.data(), seq_str.data() + seq_str.size(), owner_seq);
        if (s.ec != std::errc{} || s.ptr != seq_str.data() + seq_str.size())
            return false;
    }

    const ServerWatermark * w = watermarkOf(server);
    if (!w)
        return false;

    return server_live_this_round[server]
        && w->epoch == owner_epoch
        && owner_seq >= w->min_active
        && w->min_active != std::numeric_limits<uint64_t>::max();
}

bool Gc::acquireOrRenewLease(GcState & state, Token & state_token)
{
    const String key = store->layout().gcStateKey();

    /// Bounded: at most 2 CAS attempts per call. Iteration 2 is reached only after a create or
    /// renew Conflict (the lost-steal path returns directly — a contender that loses the steal
    /// race must re-enter the observation protocol on its NEXT round, never retry blindly).
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto got = store->backend().get(key);

        if (!got)
        {
            /// gc/state is NEVER legally deleted once created — absent AFTER we observed a lease on
            /// it proves an out-of-model deletion (operator/tooling wipe). Fail closed: recreating a
            /// default state would silently reset round/fence_seq/snap_generation/cursors and could
            /// let already-condemned incarnations be reused as live.
            if (has_observation)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc/state vanished after being observed (owner {}, seq {})",
                    u128ToHex(last_seen_owner), last_seen_seq);

            /// Step 1: fresh pool — create gc/state holding our lease (create-if-absent CAS).
            GcState fresh;
            fresh.lease = GcLease{gc_id, 1};
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(fresh), /*expected*/ std::nullopt, &committed_token)
                == CasOutcome::Committed)
            {
                rememberObservation(fresh.lease);
                state = std::move(fresh);
                state_token = committed_token;
                return true;
            }
            /// A racer created it first — re-read and fall through to renew/observe/steal.
            continue;
        }

        GcState current = decodeGcState(got->bytes);

        if (current.lease.owner == gc_id)
        {
            /// Step 2: renew — seq advances, fence_seq does NOT (renewal is not a new epoch).
            GcState next = current;
            ++next.lease.seq;
            Token committed_token;
            if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
                == CasOutcome::Committed)
            {
                /// Remember the COMMITTED (owner, seq) so the next renew never self-triggers
                /// a steal-window anomaly.
                rememberObservation(next.lease);
                state = std::move(next);
                state_token = committed_token;
                return true;
            }
            /// Someone moved the lease under us (a steal happened) — re-read once; if the owner
            /// is still us, retry the renew once; else the foreign-owner branch records the
            /// observation and backs off (our remembered observation carries owner == gc_id, so
            /// it can never match a foreign owner and turn this into a steal).
            continue;
        }

        /// Foreign owner. B160: read the advisory heartbeat. A leader bumps gc/hb on a fast cadence
        /// INDEPENDENT of round progress, so a slow-but-alive leader (whose lease.seq is frozen for
        /// the duration of its round) is still seen as alive here and is NOT stolen from. gc/hb
        /// absent, or owned by a displaced ex-leader (owner != the current gc/state owner) => no
        /// signal => fall back to the seq-only observation below (never worse than before B160).
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
            /// Step 3: the incumbent is alive (it renewed, OR its heartbeat advanced — B160), or this
            /// is our first sight of this lease — record the observation (lease + heartbeat) and back
            /// off. Eligibility to steal requires seeing the SAME (owner, seq) AND a frozen heartbeat
            /// across one whole prior round attempt of OURS.
            rememberObservation(current.lease);
            last_seen_hb_owner = hb.owner;
            last_seen_hb_seq = hb.hb_seq;
            return false;
        }

        /// Step 4: the incumbent did not renew across our full observation window — steal.
        /// fence_seq++ opens a new leadership epoch: the new leader's retire/outcome paths
        /// (<round>.<fence_seq>) never collide with the old leader's (append-by-unique-path).
        GcState next = current;
        next.lease.owner = gc_id;
        ++next.lease.seq;
        ++next.fence_seq;
        Token committed_token;
        if (store->backend().casPut(key, encodeGcState(next), got->token, &committed_token)
            == CasOutcome::Committed)
        {
            rememberObservation(next.lease);
            state = std::move(next);
            state_token = committed_token;
            return true;
        }

        /// A racer stole (or the incumbent revived and renewed) first — our CAS carried the token
        /// we read BEFORE its write, so it lost. Re-read, record what is there now, back off.
        if (const auto reread = store->backend().get(key))
            rememberObservation(decodeGcState(reread->bytes).lease);
        /// else: gc/state vanished (never legal — it is never deleted). KEEP the stale observation,
        /// so the next round's absent-after-observed check above fails closed (CORRUPTED_DATA)
        /// instead of recreating a default state.
        return false;
    }

    /// Two CAS attempts exhausted (create/renew conflicted twice) — heavy contention on gc/state
    /// means another leader is moving it; back off, this round.
    return false;
}

}
