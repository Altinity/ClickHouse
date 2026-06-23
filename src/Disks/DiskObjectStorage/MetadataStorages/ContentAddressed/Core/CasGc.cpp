#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <base/defines.h>
#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

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

namespace
{

/// The committed fold cursor for (ns, shard), read from the snap (the single source of truth).
/// snap_shards==1 is enforced, so the whole folded_cursor map lives in snap shard 0.
uint64_t cursorOf(const std::map<uint64_t, GcSnap> & snap, const String & cursor_key)
{
    const auto shard_it = snap.find(0);
    if (shard_it == snap.end())
        return 0;
    const auto it = shard_it->second.folded_cursor.find(cursor_key);
    return it != shard_it->second.folded_cursor.end() ? it->second : 0;
}

}

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

    /// B170: fold begins — the round's R1. round here is state.round (pre-retire); generation is the
    /// authoritative one being folded.
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::GcFoldBegin;
        _ev0.object_kind = CasEventObjectKind::Snap;
        _ev0.round = state.round;
        _ev0.gen = state.snap_generation;
        _ev0.reason = "R1: fold journals into a new durable snap generation";
        store->emitEvent(_ev0);
    }

    /// R1: fold the journals into a new durable snap generation (cursors advance only after).
    FoldResult folded = fold(state, state_token);   /// non-const: the recheck folds through the fence into the same snap

    /// B170: fold ended — the snap generation after the fold's persist (advanced iff records folded).
    {
        CasEvent _ev1;
        _ev1.type = CasEventType::GcFoldEnd;
        _ev1.object_kind = CasEventObjectKind::Snap;
        _ev1.round = state.round;
        _ev1.gen = state.snap_generation;
        _ev1.outcome = "ok";
        _ev1.reason = "R1 complete";
        _ev1.detail = {{"transitioned", std::to_string(folded.transitioned.size())}};
        store->emitEvent(_ev1);
    }

    /// B140-dangle FAIL-CLOSED guard: before ANY retire/delete, verify the committed snap's edges
    /// are coherent with its folded_cursor — every folded, still-live ref `Add` at or below the
    /// cursor has its tree expanded into the snap. An incoherent pair (cursor ahead of the edges)
    /// is the cursor-skip under-count that would drive a wrong delete of a still-pinned blob; refuse
    /// the round rather than proceed. Placed before `retire` so neither retire nor the recheck's
    /// content-delete site can run on an incoherent state.
    assertSnapJournalCoherent(folded.snap, folded.root_shards);

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
    trim(folded.snap, folded.root_shards, state.round);

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

void Gc::trim(const std::map<uint64_t, GcSnap> & snap,
              const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards,
              uint64_t round)
{
    for (const auto & [ns, shard] : root_shards)
    {
        const String cursor_key = ns.string() + "/" + std::to_string(shard);
        const uint64_t cursor = cursorOf(snap, cursor_key);
        if (cursor == 0)
            continue;

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

        uint64_t removed = 0;
        store->mutateShard(ns, shard, [&](RootShard & fresh)
        {
            /// INV-JOURNAL-COVERAGE: only records at or below the DURABLE cursor may go - they are
            /// provably incorporated into the durable snap generation. Records above the cursor
            /// (a publish racing this very trim included - mutateShard re-reads per attempt) stay.
            const size_t before = fresh.journal.size();
            std::erase_if(fresh.journal, [&](const JournalRecord & record)
            {
                return record.at_version <= cursor;
            });
            removed = before - fresh.journal.size();
        });
        /// B170: journal trim for this shard (records provably incorporated into the durable snap).
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::GcTrim;
            _ev2.namespace_ = ns.string();
            _ev2.round = round;
            _ev2.object_kind = CasEventObjectKind::Root;
            _ev2.outcome = "trimmed";
            _ev2.reason = "INV-JOURNAL-COVERAGE: trimmed records at or below the durable cursor";
            _ev2.detail = {{"shard", std::to_string(shard)},
                       {"trimmed_up_to_cursor", std::to_string(cursor)},
                       {"records_removed", std::to_string(removed)}};
            store->emitEvent(_ev2);
        }
    }
}

void Gc::assertSnapJournalCoherent(const std::map<uint64_t, GcSnap> & snap,
                                   const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards)
{
    const auto snap_it = snap.find(0);
    if (snap_it == snap.end())
        return;                                   /// cold start: nothing committed yet
    const GcSnap & s = snap_it->second;
    for (const auto & [ns, shard] : root_shards)
    {
        const String cursor_key = ns.string() + "/" + std::to_string(shard);
        const uint64_t cursor = cursorOf(snap, cursor_key);
        if (cursor == 0)
            continue;
        const auto [root, manifest_token] = store->readShard(ns, shard);
        /// latest record per ref name:
        std::map<String, const JournalRecord *> latest;
        for (const JournalRecord & rec : root.journal)
        {
            auto it = latest.find(rec.ref_name);
            if (it == latest.end() || it->second->at_version < rec.at_version)
                latest[rec.ref_name] = &rec;
        }
        for (const auto & [ref_name, rec] : latest)
        {
            if (rec->op != JournalRecord::Op::Add || rec->at_version > cursor)
                continue;                          /// only a folded, still-live Add must be reflected
            if (!s.isKnown(ObjectKind::Tree, rec->tree_id))
            {
                /// B170: the coherence guard tripped — record the anomaly with the entity it touches
                /// before failing closed, so the incident is a single SQL query, not a log-grep.
                {
                    CasEvent _ev3;
                    _ev3.type = CasEventType::SnapJournalIncoherent;
                    _ev3.namespace_ = ns.string();
                    _ev3.ref_name = ref_name;
                    _ev3.object_kind = CasEventObjectKind::Tree;
                    _ev3.object_hash = u128ToHex(rec->tree_id);
                    _ev3.at_version = rec->at_version;
                    _ev3.outcome = "fail_closed";
                    _ev3.reason = "snap/journal incoherent: live Add at or below cursor but tree absent from snap (B140-dangle cursor-skip)";
                    _ev3.detail = {{"cursor", std::to_string(cursor)},
                               {"shard", std::to_string(shard)},
                               {"code", "CORRUPTED_DATA"},
                               {"site", "assertSnapJournalCoherent"}};
                    store->emitEvent(_ev3);
                }
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS gc: snap/journal incoherent - ref '{}' (ns {} shard {}) Add@{} is at or below the "
                    "folded cursor {} but its tree {} is absent from the snap (B140-dangle cursor-skip); "
                    "refusing to retire/delete this round",
                    ref_name, ns.string(), shard, rec->at_version, cursor, u128ToHex(rec->tree_id));
            }
        }
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
        const uint64_t cursor = cursorOf(snap, cursor_key);
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
            foldShardRecords(snap, state, ns, cursor_key, root, cursor, fence_version);
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

            const uint64_t indeg_at_recheck =
                snap.at(hashPrefixShard(entry.hash, state.snap_shards)).inDegree(entry.kind, entry.hash);
            const uint64_t fence_version_for_round = state.fence_seq;   /// the leadership-epoch fence seq
            const CasEventObjectKind ev_kind =
                entry.kind == ObjectKind::Tree ? CasEventObjectKind::Tree
                : (entry.kind == ObjectKind::Pack ? CasEventObjectKind::Pack : CasEventObjectKind::Blob);
            if (indeg_at_recheck > 0)
            {
                /// A publish at or below the fence re-pinned it (folded above) - never delete.
                outcome.outcome = OutcomeKind::Spared;
                /// B170: the recheck spared this candidate — record the verdict + the in-degree it
                /// found through the fold-through-fence, so a "why wasn't it deleted" is answerable.
                {
                    CasEvent _ev4;
                    _ev4.type = CasEventType::GcRecheckVerdict;
                    _ev4.object_kind = ev_kind;
                    _ev4.object_hash = u128ToHex(entry.hash);
                    _ev4.token = entry.token.value;
                    _ev4.round = round;
                    _ev4.gen = state.snap_generation;
                    _ev4.outcome = "spared";
                    _ev4.reason = "inDeg>0 after fold-through-fence; a publish re-pinned it";
                    _ev4.detail = {{"indeg_at_recheck", std::to_string(indeg_at_recheck)},
                               {"fence_seq", std::to_string(fence_version_for_round)}};
                    store->emitEvent(_ev4);
                }
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
                const String del_key = objectKey(layout, entry.kind, entry.hash);
                const DeleteOutcome deleted = backend.deleteExact(del_key, entry.token);
                /// ========================================================================
                /// B170: the single content-delete site (was the CAGCDEL audit line). This is the
                /// ONLY place Gc deletes a content object, so a `blob_delete`/`tree_delete` row makes
                /// any "reachable object MISSING" finding attributable: a missing blob whose hash
                /// appears here was deleted by GC; one that does not was lost elsewhere. Carries the
                /// full decision context (round/fence_seq/generation/shard) + the cause chain in
                /// `reason` so the delete is localizable from the row alone.
                const String del_outcome =
                    deleted.kind == DeleteOutcome::Kind::Deleted ? "deleted"
                    : (deleted.kind == DeleteOutcome::Kind::NotFound ? "absent" : "replaced");
                {
                    CasEvent _ev5;
                    _ev5.type = entry.kind == ObjectKind::Tree ? CasEventType::TreeDelete : CasEventType::BlobDelete;
                    _ev5.object_kind = ev_kind;
                    _ev5.object_hash = u128ToHex(entry.hash);
                    _ev5.token = entry.token.value;
                    _ev5.round = round;
                    _ev5.gen = state.snap_generation;
                    _ev5.outcome = del_outcome;
                    _ev5.reason = "in-degree 0 through fence; recheck confirmed 0; exact-token delete";
                    /// `token_outcome` = the exact-token deleteExact verdict (deleted/absent/replaced),
                    /// matching the recheck verdict — the load-bearing fact for B140-dangle attribution
                    /// ("did our token actually remove the bytes, or did a fresh incarnation displace it").
                    /// `parent_tree`: the owning tree is NOT in scope here. By construction a candidate
                    /// reaches the delete site only AFTER its last incoming edge was erased (in-degree 0;
                    /// `RetiredEntry` carries kind/hash/token/size, no owner). The parent that dropped the
                    /// last edge is recorded on the earlier `IndegZero` row for this same `object_hash`
                    /// (its `dropped_by` = strip(<tree>) / root_remove(<ref>) / root_repoint(<ref>)),
                    /// so the attribution join is object_hash -> IndegZero.dropped_by, not a field here.
                    _ev5.detail = {{"fence_seq", std::to_string(state.fence_seq)},
                               {"shard", std::to_string(hashPrefixShard(entry.hash, state.snap_shards))},
                               {"indeg_at_recheck", "0"},
                               {"token_outcome", del_outcome},
                               {"parent_tree", "see IndegZero.dropped_by for object_hash=" + u128ToHex(entry.hash)},
                               {"key", del_key}};
                    store->emitEvent(_ev5);
                }
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
                /// B170: the recheck verdict for the delete arm (deleted/absent/replaced). Pairs with
                /// the blob_delete/tree_delete above so a recheck verdict exists for every candidate.
                {
                    CasEvent _ev6;
                    _ev6.type = CasEventType::GcRecheckVerdict;
                    _ev6.object_kind = ev_kind;
                    _ev6.object_hash = u128ToHex(entry.hash);
                    _ev6.token = entry.token.value;
                    _ev6.round = round;
                    _ev6.gen = state.snap_generation;
                    _ev6.outcome = del_outcome;
                    _ev6.reason = del_outcome == "replaced"
                        ? "token displaced (412); a fresh incarnation pins it"
                        : (del_outcome == "absent" ? "already gone (our own crashed delete landed)"
                                                   : "inDeg 0 confirmed through fence; deleted");
                    _ev6.detail = {{"indeg_at_recheck", "0"},
                               {"fence_seq", std::to_string(state.fence_seq)}};
                    store->emitEvent(_ev6);
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
    /// B170: the generation we fold ON TOP of — captured BEFORE the `state = std::move(next)` below
    /// reassigns state.snap_generation to the adopted one, so GcSnapPersist can report the real prior.
    const uint64_t prev_generation = state.snap_generation;
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
        /// B170: every cascade strip of a tree, with the children it freed (the next round's
        /// zero-in-degree delete candidates). Was the CASTRIP audit line.
        {
            CasEvent _ev7;
            _ev7.type = CasEventType::TreeStrip;
            _ev7.object_kind = CasEventObjectKind::Tree;
            _ev7.object_hash = u128ToHex(tree);
            _ev7.round = round;
            _ev7.gen = state.snap_generation;
            _ev7.outcome = "stripped";
            _ev7.reason = "cascade strip of confirmed-deleted tree; child edges freed";
            _ev7.detail = {{"freed", std::to_string(freed.size())}};
            store->emitEvent(_ev7);
        }
        /// B170: each freed child whose in-degree just hit 0 is a new retire candidate — emit the
        /// in-degree-zero transition with what dropped it (the parent strip), so a blob's
        /// "why did it become collectable" is reconstructable from the rows.
        for (const Candidate & child : freed)
        {
            CasEvent _ev8;
            _ev8.type = CasEventType::IndegZero;
            _ev8.object_kind = child.kind == ObjectKind::Tree ? CasEventObjectKind::Tree : CasEventObjectKind::Blob;
            _ev8.object_hash = u128ToHex(child.hash);
            _ev8.round = round;
            _ev8.gen = state.snap_generation;
            _ev8.reason = "last edge dropped";
            /// prev_indeg is exactly 1: a Candidate is returned ONLY on the 1->0 transition
            /// (dropEdgeTarget short-circuits while in-degree stays > 0).
            _ev8.detail = {{"prev_indeg", "1"},
                       {"dropped_by", "strip(" + u128ToHex(tree) + ")"}};
            store->emitEvent(_ev8);
        }
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
        /// B170: P9 prune — the node is forgotten from `known` so the next round's stateless scan
        /// stops re-deriving (and re-HEAD-404ing) it. Closes the node's lifecycle in the rows.
        {
            CasEvent _ev9;
            _ev9.type = CasEventType::BlobForget;
            _ev9.object_kind = node.kind == ObjectKind::Tree ? CasEventObjectKind::Tree : CasEventObjectKind::Blob;
            _ev9.object_hash = u128ToHex(node.hash);
            _ev9.round = round;
            _ev9.gen = state.snap_generation;
            _ev9.outcome = "forgotten";
            _ev9.reason = "deleted/absent in this round; pruned from known (P9)";
            store->emitEvent(_ev9);
        }
    }

    /// 2. PERSIST the post-strip snap - WITH the recheck's fence-window fold included - and the
    /// folded_cursor advanced to the recorded fence versions IN THE SNAP BYTES (B140-dangle fix:
    /// cursor lives in snap shard 0, so (edges, cursor) are one write-once unit that can never
    /// diverge across a crash). This is the pipeline ORDERING rule that closes the
    /// cascade-vs-recreate race: a re-create-and-publish racing the deletion necessarily lands at a
    /// shard_version ABOVE this round's fence version, so the next round folds its Add on a snap
    /// whose marker was already CLEARED by the strip - the re-expansion re-pins the children.
    /// Persisting the strip lazily (or not advancing the cursors with it) would let that Add fold
    /// against a still-set marker, skip the re-expansion, and leave the recreated live tree's
    /// children edge-less - an under-count, a wrong delete.
    /// Same write-once probe-upward discipline as the fold's persist. SKIPPED when the snap is
    /// semantically unchanged since the fold's persist (no strips, no fence-window records, no
    /// forgotten nodes - the common idle case): persisting a byte-equivalent-except-generation snap
    /// every round would mint an orphan object per round forever for nothing. When skipped, the
    /// cursor does NOT advance (conservative: the next fold re-reads those journal records and the
    /// trim leaves them in place - no loss). The closing gc/state CAS below still runs
    /// unconditionally (fence_version[<= round] is erased).
    const bool snap_changed = !rechecked.deleted_trees.empty() || rechecked.fence_window_records_folded
        || report.forgotten_on_delete > 0   /// P9: a blob-only prune round must still persist the snap
        || report.forgotten_absent > 0;      /// P9: a retire-time 404 forgot a node in-memory; the
                                             /// forget must go durable or the next round re-derives
                                             /// and re-HEAD-404s it (the RetireForgets regression).
    constexpr uint64_t max_generation_probes = 1000;
    constexpr uint64_t MAX_PRUNE_GENERATIONS_PER_ROUND = 64;   /// B174: bound the per-round prune burst
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
            /// Advance the fold cursor in snap shard 0 (the single source of truth, B140-dangle fix):
            /// write the fence versions into the snap so (edges, cursor) are persisted as one
            /// write-once unit — they can never diverge across a crash.
            if (snap_shard == 0)
            {
                for (const auto & [cursor_key, fence_version] : fence_it->second)
                {
                    if (cursor_key == "_registry")
                        continue;
                    shard_snap.folded_cursor[cursor_key]
                        = std::max(shard_snap.folded_cursor[cursor_key], fence_version);
                }
            }
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
    /// fence_version[<= round] served its recheck - erase it (gc/state must not grow forever).
    std::erase_if(next.fence_version, [&](const auto & kv) { return kv.first <= round; });

    /// B174: prune superseded snap generations. loadSnap reads ONLY gcSnapKey(snap_generation), so
    /// any generation strictly below the committed one is dead; keep `keep` as the safety margin for
    /// in-flight/resuming leaders (a leader more than `keep` generations behind has lost its lease —
    /// its round-commit CAS fails). Walk forward from the durable cursor, bounded per round, so a
    /// large legacy backlog drains over many rounds without ever LISTing the generation directories.
    /// Done BEFORE the gc/state CAS so the advanced cursor rides the same write: if the CAS then
    /// loses the lease, the deletes were still below the winner's even-higher floor (safe) and the
    /// cursor is not durably advanced (idempotent retry — old generations are write-once, so a
    /// re-HEAD finds them absent).
    const uint64_t keep = store->poolConfig().gc_snap_generations_to_keep;
    if (keep > 0 && adopted_generation > keep)
    {
        const uint64_t prune_floor = adopted_generation - keep;   /// prune generations <= prune_floor
        uint64_t generation = next.snap_pruned_through + 1;
        uint64_t pruned = 0;
        for (; generation <= prune_floor && pruned < MAX_PRUNE_GENERATIONS_PER_ROUND; ++generation, ++pruned)
        {
            for (uint64_t snap_shard = 0; snap_shard < state.snap_shards; ++snap_shard)
            {
                const String snap_key = layout.gcSnapKey(generation, snap_shard);
                const HeadResult hr = backend.head(snap_key);
                if (hr.exists)
                    backend.deleteExact(snap_key, hr.token);   /// NotFound/TokenMismatch tolerated
            }
        }
        next.snap_pruned_through = generation - 1;   /// highest generation fully processed this round
    }

    Token committed_token;
    if (backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token, &committed_token)
        != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc cascade: gc/state moved during the cascade persist (another leader advanced it); "
            "retry next round");
    state = std::move(next);
    state_token = committed_token;

    /// B170: the cascade persisted a new snap generation (or skipped it on the idle path) and the
    /// gc/state CAS advanced the fold cursor through the fence window. Emit both transitions so a
    /// snap-vs-truth divergence query can pin which generation a fact landed in.
    if (snap_changed)
    {
        {
            CasEvent _ev10;
            _ev10.type = CasEventType::GcSnapPersist;
            _ev10.object_kind = CasEventObjectKind::Snap;
            _ev10.round = round;
            _ev10.gen = adopted_generation;
            _ev10.outcome = "persisted";
            _ev10.reason = "cascade persisted the post-strip snap (strips/fence-window/forgets)";
            /// `cursor` = the folded cursors persisted into snap shard 0 ("ns/shard:version" pairs,
            /// the single source of truth the new generation carries). `size` = the snap's edge-shard
            /// count (the serialized snap is sharded over these). `generation_probed` = how many
            /// generations above the prior the putIfAbsent contention loop had to probe before it
            /// found an adoptable (non-diverged) slot.
            String persisted_cursor;
            if (const auto snap0 = snap.find(0); snap0 != snap.end())
                for (const auto & [cursor_key, folded_version] : snap0->second.folded_cursor)
                {
                    if (!persisted_cursor.empty())
                        persisted_cursor += ",";
                    persisted_cursor += cursor_key + ":" + std::to_string(folded_version);
                }
            _ev10.detail = {{"prev_generation", std::to_string(prev_generation)},
                       {"cursor", persisted_cursor},
                       {"size", std::to_string(snap.size())},
                       {"generation_probed", std::to_string(adopted_generation - prev_generation)},
                       {"deleted_trees", std::to_string(rechecked.deleted_trees.size())},
                       {"forgotten_on_delete", std::to_string(report.forgotten_on_delete)}};
            store->emitEvent(_ev10);
        }
        {
            CasEvent _ev11;
            _ev11.type = CasEventType::GcCursorAdvance;
            _ev11.object_kind = CasEventObjectKind::Snap;
            _ev11.round = round;
            _ev11.gen = adopted_generation;
            _ev11.outcome = "advanced";
            _ev11.reason = "fold cursor advanced to the recorded fence versions (one write-once unit)";
            store->emitEvent(_ev11);
        }
    }

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
    /// registry_version is recorded under the reserved `"_registry"` key in `fence_version` (a
    /// logical discriminator that never collides with any `"ns/shard"` entry).
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
    uint64_t fenced_shards = 0;
    for (const String & ns_name : fence_universe.namespaces)
    {
        const RootNamespace ns{ns_name};
        /// Every namespace — table and precommit (B171 fix) — is fenced over the static [0, root_shards)
        /// fan-out via shardsToVisit. Precommit namespaces are now bounded by root_shards, not per-build.
        for (const uint64_t shard : shardsToVisit(ns))
        {
            ++fenced_shards;
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
        /// B170: the monotone fence committed — the durable point the recheck folds through.
        {
            CasEvent _ev12;
            _ev12.type = CasEventType::GcFence;
            _ev12.object_kind = CasEventObjectKind::Snap;
            _ev12.round = round;
            _ev12.gen = state.snap_generation;
            _ev12.outcome = "fenced";
            _ev12.reason = "R3: fenced every root shard of every registered namespace";
            _ev12.detail = {{"fence_seq", std::to_string(state.fence_seq)},
                       {"namespaces", std::to_string(fence_universe.namespaces.size())},
                       {"shards", std::to_string(fenced_shards)}};
            store->emitEvent(_ev12);
        }
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

    /// B171: retire no longer consults the watermark (the per-candidate `protectedByLiveBuild` guard
    /// was removed — protection is now the precommit edge, via reachability). The per-round watermark
    /// caches are begun in `fold` (it visits the precommit shards for precommit reclaim), so there is
    /// no `beginWatermarkRound` here anymore.

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
            const CasEventObjectKind ev_kind =
                candidate.kind == ObjectKind::Tree ? CasEventObjectKind::Tree
                : (candidate.kind == ObjectKind::Pack ? CasEventObjectKind::Pack : CasEventObjectKind::Blob);
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            /// B170: HEAD-observe — the current incarnation token, the only token the eventual
            /// exact-token delete may carry.
            {
                CasEvent _ev13;
                _ev13.type = CasEventType::GcRetireObserve;
                _ev13.object_kind = ev_kind;
                _ev13.object_hash = u128ToHex(candidate.hash);
                _ev13.token = observed.exists ? observed.token.value : "";
                _ev13.round = round;
                _ev13.gen = state.snap_generation;
                _ev13.outcome = observed.exists ? "present" : "absent";
                _ev13.reason = "zero-in-degree candidate; HEAD-observe current token";
                store->emitEvent(_ev13);
            }
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
                ///
                /// B199-S1: if the absent candidate is a TREE that WAS expanded, its child edges are
                /// in this (home) shard's snap. A bare `forget` drops the tree node from `known` but
                /// does NOT release those edges, so each child keeps its in-degree contribution from
                /// this tree and never reaches zero — its (now genuinely-unreferenced) blobs leak as
                /// `unreachable` debris forever (the soak's space-only leak; dangling stays 0 — not
                /// data loss). Release the children the SAME way the delete-time cascade does
                /// (cascadeAndPersist: `stripTree`), THEN forget the tree node. `stripTree` drops the
                /// tree's child edges, decrements each child's in-degree, clears the `expanded` marker,
                /// and returns the newly-zeroed children — which become zero-in-degree candidates a
                /// SUBSEQUENT round picks up via the normal cascade cadence (we do NOT delete them
                /// inline). This is in-memory only (like `forget`); the actual child deletes still go
                /// through the next round's fence→recheck→exact-token path, so a concurrent
                /// re-reference is still caught (no dangle introduced). A never-expanded tree (S2)
                /// strips as a no-op — its edges were never recorded — and its children still leak;
                /// that class needs an eager-edge/full-sweep backstop, out of S1 scope.
                if (candidate.kind == ObjectKind::Tree)
                {
                    const std::vector<Candidate> freed = shard_snap.stripTree(candidate.hash);
                    report.cascaded += freed.size();
                    for (const Candidate & child : freed)
                    {
                        CasEvent _ev_indeg;
                        _ev_indeg.type = CasEventType::IndegZero;
                        _ev_indeg.object_kind =
                            child.kind == ObjectKind::Tree ? CasEventObjectKind::Tree : CasEventObjectKind::Blob;
                        _ev_indeg.object_hash = u128ToHex(child.hash);
                        _ev_indeg.round = round;
                        _ev_indeg.gen = state.snap_generation;
                        _ev_indeg.reason = "last edge dropped";
                        _ev_indeg.detail = {{"prev_indeg", "1"},
                                            {"dropped_by", "retire-absent-strip(" + u128ToHex(candidate.hash) + ")"}};
                        store->emitEvent(_ev_indeg);
                    }
                }
                shard_snap.forget(candidate.kind, candidate.hash);
                ++report.forgotten_absent;
                /// B170: defensive prune — the candidate is gone (a 404) but still sat in `known`.
                /// Forget it so the next round stops re-deriving and re-HEAD-404ing it.
                {
                    CasEvent _ev14;
                    _ev14.type = CasEventType::BlobForget;
                    _ev14.object_kind = ev_kind;
                    _ev14.object_hash = u128ToHex(candidate.hash);
                    _ev14.round = round;
                    _ev14.gen = state.snap_generation;
                    _ev14.outcome = "forgotten";
                    _ev14.reason = "retire-time HEAD 404; pruned from known (P9 defensive)";
                    store->emitEvent(_ev14);
                }
                continue;
            }

            /// B171: the retire decision is now PURE REACHABILITY — present ∧ known ∧ inDeg=0 ⇒ condemn.
            /// The old `protectedByLiveBuild` per-candidate `cas_owner` guard is gone: an object an
            /// in-flight build needs is protected by the build's PRECOMMIT EDGE (a precommit root ref
            /// the fold expands into in-degree ≥ 1), so it is never a zero-in-degree candidate here. The
            /// watermark is repurposed for precommit RECLAIM liveness (see reclaimAbandonedPrecommit),
            /// not per-object protection.
            {
                CasEvent _ev16;
                _ev16.type = CasEventType::GcRetireDecision;
                _ev16.object_kind = ev_kind;
                _ev16.object_hash = u128ToHex(candidate.hash);
                _ev16.token = observed.token.value;
                _ev16.round = round;
                _ev16.gen = state.snap_generation;
                _ev16.outcome = "condemn";
                _ev16.reason = "condemn: present and known and inDeg=0 (reachability)";
                store->emitEvent(_ev16);
            }

            RetiredEntry entry;
            entry.kind = candidate.kind;
            entry.hash = candidate.hash;
            entry.token = observed.token;
            entry.size = retiredLogicalSize(candidate.kind, observed.size, store->poolMeta().blob_header_len);
            /// B170: the retire entry written for this incarnation (per RetiredEntry).
            {
                CasEvent _ev17;
                _ev17.type = candidate.kind == ObjectKind::Tree ? CasEventType::TreeRetire : CasEventType::BlobRetire;
                _ev17.object_kind = ev_kind;
                _ev17.object_hash = u128ToHex(candidate.hash);
                _ev17.token = observed.token.value;
                _ev17.round = round;
                _ev17.gen = state.snap_generation;
                _ev17.outcome = "retired";
                _ev17.reason = "condemned candidate written to the round's retired set";
                _ev17.detail = {{"size", std::to_string(entry.size)}};
                store->emitEvent(_ev17);
            }
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
                                            const RootNamespace & ns, const String & cursor_key,
                                            const RootShard & root,
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
            /// B170: the root edge for this ref (a publish Add folded into the snap). A repoint over
            /// an existing edge displaces the old target — emit RootRepoint, else RootAdd.
            {
                CasEvent _ev18;
                _ev18.type = displaced.empty() ? CasEventType::RootAdd : CasEventType::RootRepoint;
                _ev18.namespace_ = ns.string();
                _ev18.ref_name = record.ref_name;
                _ev18.object_kind = CasEventObjectKind::Tree;
                _ev18.object_hash = u128ToHex(record.tree_id);
                _ev18.round = state.round;
                _ev18.gen = state.snap_generation;
                _ev18.at_version = record.at_version;
                _ev18.outcome = "ok";
                _ev18.reason = displaced.empty()
                    ? "folded Add(" + record.ref_name + ")"
                    : "folded republish Add(" + record.ref_name + "); old target displaced";
                _ev18.detail = {{"displaced", std::to_string(displaced.size())}};
                store->emitEvent(_ev18);
            }
            /// B170: each old target whose in-degree zeroed on the repoint — emit per freed candidate
            /// so the in-degree-zero transition (and what dropped it) is in the rows.
            for (const Candidate & old : displaced)
            {
                CasEvent _ev19;
                _ev19.type = CasEventType::IndegZero;
                _ev19.namespace_ = ns.string();
                _ev19.object_kind = old.kind == ObjectKind::Tree ? CasEventObjectKind::Tree : CasEventObjectKind::Blob;
                _ev19.object_hash = u128ToHex(old.hash);
                _ev19.round = state.round;
                _ev19.gen = state.snap_generation;
                _ev19.at_version = record.at_version;
                _ev19.reason = "last edge dropped";
                /// prev_indeg is exactly 1 (Candidate returned only on the 1->0 transition).
                _ev19.detail = {{"prev_indeg", "1"},
                           {"dropped_by", "root_repoint(" + record.ref_name + ")"}};
                store->emitEvent(_ev19);
            }

            /// Once-per-tree expansion: the FIRST '+' to a tree with no marker reads the tree
            /// once and adds its child-edge set (each edge into the CHILD's shard).
            GcSnap & tree_home = shard_for(record.tree_id);
            if (!tree_home.isExpanded(record.tree_id))
            {
                uint64_t out_edges = 0;

                /// ONE unified fold source for EVERY Add (B199-S2 Task 7). A node's entries come from:
                ///   • the precommit Add's INLINE closure when the node is STAGED (`record.closure`,
                ///     keyed by tree hash) — no pool read; this is the S2 protection that records a
                ///     staged tree's edges even when its object vanished before expansion; OR
                ///   • `readTree(node)` otherwise — table-ns Adds carry an empty closure so they ALWAYS
                ///     read (identical to the pre-S2 behavior), and an ADOPTED/committed root precommit
                ///     (replication relink `adoptTree`, rename/move `adoptEvidence`) also carries an
                ///     empty closure and reads the committed object the live source kept present.
                /// The two former branches collapse here. An empty closure is NOT corruption — it is the
                /// normal adopted-root case (the Task-5 LOGICAL_ERROR guard was the regression and is gone).
                std::map<UInt128, std::vector<TreeEntry>> by_node;
                for (const ClosureNode & node : record.closure)
                    by_node.emplace(node.tree_hash, node.entries);

                const bool precommit_ns = Layout::isPrecommitNamespace(ns);

                /// A folded Add implies the tree was published, but the object may legitimately be gone
                /// NOW (displaced later in the journal + a completed competing round deleted it; or a
                /// precommit naming a not-yet-uploaded / raced tree). Tolerance is PER-NODE, never
                /// whole-closure (B199-S2 Task 7 follow-up): a deeper adopted node that 404s must NOT
                /// drop the edges/marks already in hand for STAGED nodes — those are authoritative from
                /// the inline closure regardless of any adopted object's presence (dropping them would
                /// reopen the S2 leak: a staged parent would become a never-expanded tree and leak its
                /// OWN unique blobs on abandon).
                ///   • ROOT 404 under a table ns: tolerate ONLY if a later record for the SAME ref
                ///     displaces this Add (Remove or re-pointing Add); otherwise a live ref to a missing
                ///     tree is INV-NO-DANGLE — fail closed (the unchanged B170 contract).
                ///   • ROOT 404 under a precommit ns: a precommit naming an absent target is a legal
                ///     pending/aborting reservation (B171) — tolerate (the root joins `pending_nodes`).
                ///   • Any node 404 reached DEEPER under a precommit ns: tolerate as pending — that node
                ///     joins `pending_nodes` (it is not marked expanded), but its parent's edge INTO it
                ///     and all other staged edges/marks are kept. A later present-object Add re-expands
                ///     the absent node once its object arrives.
                ///   • A node 404 reached deeper under a TABLE ns: a present parent referencing a missing
                ///     child is a real dangle — fail closed (propagate; unchanged). A table-ns node never
                ///     enters `pending_nodes`.
                /// Marking/edges are BUFFERED, then applied: every buffered edge is applied (a pending
                /// node contributes no edges AS A PARENT — it returned {} — so only the in-edge from its
                /// non-pending parent survives, giving it in-degree = the "known but unexpanded" state);
                /// markExpanded is applied for every buffered node EXCEPT those in `pending_nodes`. So a
                /// node is marked expanded ONLY when its entries were fully in hand (staged-inline or a
                /// successful readTree) ⇒ a marked node always has COMPLETE edges.
                std::set<UInt128> pending_nodes;

                auto rootDisplacedLater = [&]() -> bool
                {
                    for (size_t ahead = record_idx + 1; ahead < root.journal.size(); ++ahead)
                    {
                        const JournalRecord & next = root.journal[ahead];
                        if ((next.op == JournalRecord::Op::Remove || next.op == JournalRecord::Op::Add)
                            && next.ref_name == record.ref_name && next.at_version > record.at_version)
                            return true;
                    }
                    return false;
                };

                auto failClosedMissingTree = [&](const Exception & e)
                {
                    /// B170: the manifest claims a LIVE ref to a missing tree and no later record
                    /// displaces it — INV-NO-DANGLE surfaced. Record before propagating (fail closed).
                    CasEvent _ev20;
                    _ev20.type = CasEventType::FailClosed;
                    _ev20.namespace_ = ns.string();
                    _ev20.ref_name = record.ref_name;
                    _ev20.object_kind = CasEventObjectKind::Tree;
                    _ev20.object_hash = u128ToHex(record.tree_id);
                    _ev20.round = state.round;
                    _ev20.gen = state.snap_generation;
                    _ev20.at_version = record.at_version;
                    _ev20.outcome = "fail_closed";
                    _ev20.reason = e.message();
                    _ev20.detail = {{"code", "FILE_DOESNT_EXIST"},
                               {"site", "foldShardRecords: live ref to missing tree"}};
                    store->emitEvent(_ev20);
                };

                /// The unified source: inline for staged nodes, readTree otherwise. On a tolerated 404 it
                /// records `node` in `pending_nodes` and returns {} (that node is left unexpanded); on an
                /// intolerable 404 it fails closed (propagate). A staged node is in `by_node` ⇒ never
                /// readTree ⇒ never pending — only adopted/committed nodes can become pending.
                auto unifiedSource = [&](const UInt128 & node) -> std::vector<TreeEntry>
                {
                    if (auto it = by_node.find(node); it != by_node.end())
                        return it->second;   /// staged inline — no read, no 404
                    try
                    {
                        return store->readTree(TreeId(u128ToHex(node)));
                    }
                    catch (const Exception & e)
                    {
                        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
                            throw;
                        const bool is_root = node == record.tree_id;
                        if (precommit_ns || (is_root && rootDisplacedLater()))
                        {
                            /// Pending/displaced tolerance: this node is left unexpanded (see above).
                            pending_nodes.insert(node);
                            return {};
                        }
                        /// Table ns: root with no displacing record, or a missing internal child — a real
                        /// dangle. Fail closed.
                        failClosedMissingTree(e);
                        throw;
                    }
                };

                /// BUFFER marks + edges so per-node tolerance can be applied after the walk.
                std::vector<UInt128> to_mark;
                std::vector<std::pair<UInt128, TreeEntry>> to_edge;
                auto on_tree = [&](const UInt128 & tree)
                {
                    to_mark.push_back(tree);
                };
                auto on_edge = [&](const UInt128 & parent_tree, const TreeEntry & entry)
                {
                    to_edge.emplace_back(parent_tree, entry);
                };

                closureWalk(record.tree_id, unifiedSource, on_tree, on_edge);

                /// Apply ALL buffered edges (a pending node contributed none as a parent), but mark only
                /// nodes whose entries were fully in hand (NOT pending).
                for (const UInt128 & tree : to_mark)
                    if (!pending_nodes.contains(tree))
                        shard_for(tree).markExpanded(tree);
                for (const auto & [parent_tree, entry] : to_edge)
                {
                    switch (entry.placement)
                    {
                        case Placement::Blob:
                            shard_for(entry.file_hash).addTreeEdge(parent_tree, ObjectKind::Blob, entry.file_hash);
                            ++out_edges;
                            break;
                        case Placement::Subtree:
                            shard_for(entry.file_hash).addTreeEdge(parent_tree, ObjectKind::Tree, entry.file_hash);
                            ++out_edges;
                            break;
                        case Placement::PackSlice:
                            shard_for(entry.pack_hash).addPackEdge(parent_tree, entry.pack_hash);
                            ++out_edges;
                            break;
                        case Placement::Inline:
                            break;   /// embedded bytes - no separate object, no edge
                    }
                }

                /// The root counts as expanded (emit TreeExpand) iff it was NOT pending — an adopted root
                /// that 404'd (relink/rename racing a drop) stays unexpanded and is re-expanded by a later
                /// present-object Add.
                const bool root_expanded = !pending_nodes.contains(record.tree_id);
                if (root_expanded)
                {
                    /// B170: the tree was expanded — its child edges are now recorded in the snap.
                    /// This is the "set the expansion marker / record child edges" transition.
                    CasEvent _ev21;
                    _ev21.type = CasEventType::TreeExpand;
                    _ev21.namespace_ = ns.string();
                    _ev21.ref_name = record.ref_name;
                    _ev21.object_kind = CasEventObjectKind::Tree;
                    _ev21.object_hash = u128ToHex(record.tree_id);
                    _ev21.round = state.round;
                    _ev21.gen = state.snap_generation;
                    _ev21.at_version = record.at_version;
                    _ev21.outcome = "expanded";
                    _ev21.reason = "first folded Add to this tree; child edges recorded in snap";
                    _ev21.detail = {{"out_edges", std::to_string(out_edges)},
                               {"driving_add_at_version", std::to_string(record.at_version)}};
                    store->emitEvent(_ev21);
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
            /// B170: a folded Remove dropped this ref's root edge (was the CAROOTREM audit line).
            /// `zeroed` = the tree's in-degree hit 0 (it becomes a delete candidate -> cascade strip).
            {
                CasEvent _ev22;
                _ev22.type = CasEventType::RootRemove;
                _ev22.namespace_ = ns.string();
                _ev22.ref_name = record.ref_name;
                _ev22.object_kind = CasEventObjectKind::Tree;
                _ev22.object_hash = u128ToHex(record.tree_id);
                _ev22.round = state.round;
                _ev22.gen = state.snap_generation;
                _ev22.at_version = record.at_version;
                _ev22.outcome = cands.empty() ? "ok" : "zeroed";
                _ev22.reason = "folded Remove(" + record.ref_name + ")";
                _ev22.detail = {{"zeroed", cands.empty() ? "false" : "true"}};
                store->emitEvent(_ev22);
            }
            /// B170: each target whose in-degree zeroed on the remove — emit per freed candidate.
            for (const Candidate & freed : cands)
            {
                CasEvent _ev23;
                _ev23.type = CasEventType::IndegZero;
                _ev23.namespace_ = ns.string();
                _ev23.object_kind = freed.kind == ObjectKind::Tree ? CasEventObjectKind::Tree : CasEventObjectKind::Blob;
                _ev23.object_hash = u128ToHex(freed.hash);
                _ev23.round = state.round;
                _ev23.gen = state.snap_generation;
                _ev23.at_version = record.at_version;
                _ev23.reason = "last edge dropped";
                /// prev_indeg is exactly 1 (Candidate returned only on the 1->0 transition).
                _ev23.detail = {{"prev_indeg", "1"},
                           {"dropped_by", "root_remove(" + record.ref_name + ")"}};
                store->emitEvent(_ev23);
            }
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
        for (const String & ns_name : registry.namespaces)
        {
            const RootNamespace ns{ns_name};
            /// Every namespace — table and precommit (B171 fix) — uses the static [0, root_shards)
            /// fan-out via shardsToVisit. Precommit namespaces are now bounded by root_shards, not per-build.
            for (const uint64_t shard : shardsToVisit(ns))
                universe.emplace_back(ns, shard);
        }
    }
    return universe;
}

std::vector<uint64_t> Gc::shardsToVisit(const RootNamespace &)
{
    /// EVERY namespace — table AND precommit (B171 fix) — uses the static shard fan-out [0, root_shards).
    /// The precommit namespace is now sharded exactly like a table namespace (ref name == build_seq, shard ==
    /// shardOf(build_seq)), so it has at most root_shards shards (bounded), each holding many builds'
    /// precommit refs. This removes the old per-build LIST special-case whose O(total-builds-ever) fold
    /// cost wedged GC. The fence mints fence-only manifests for absent shards, so the absent ones are
    /// needed too (the absent-shard hole). `ns` is unused now but kept in the signature: both callers
    /// (discoverUniverse, the fence walk) iterate this uniformly per namespace.
    std::vector<uint64_t> shards;
    const uint64_t root_shards_per_ns = store->poolMeta().root_shards;
    shards.reserve(root_shards_per_ns);
    for (uint64_t shard = 0; shard < root_shards_per_ns; ++shard)
        shards.push_back(shard);
    return shards;
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

    /// B140-dangle FAIL-CLOSED guard (task #131, symmetry with runRegularRound): a resumed round
    /// reaches the SAME content-delete (via recheck) on the loaded durable snap; verify the snap's
    /// folded_cursor is coherent with its edges before any delete, exactly as the normal path does.
    const auto resume_universe = discoverUniverse();
    assertSnapJournalCoherent(snap, resume_universe);

    const RecheckResult rechecked = recheck(state, snap, retired, report);
    cascadeAndPersist(state, state_token, snap, rechecked, retired, report);
    trim(snap, resume_universe, state.round);
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

    /// B171 precommit reclaim (§C.3): the fold visits the precommit shards and decides each owning
    /// build's liveness from the per-server watermark. That uses the SAME per-round caches the K=2
    /// crash detector accumulates, so begin the watermark round HERE (before the precommit visit
    /// below) rather than in retire — retire no longer consults the watermark since
    /// `protectedByLiveBuild` was removed (B171); the watermark is now read only for reclaim liveness.
    beginWatermarkRound();

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
    /// Record the position each shard folded TO (its shard_version at read time): this is the
    /// coherent cursor for the persisted generation — cursor == folded extent, never ahead of the
    /// edges. The persist loop below writes these into snap shard 0's folded_cursor so fold's
    /// generation is self-coherent (B140-dangle fix). Without it the persisted generation would
    /// inherit the PRIOR generation's cursor (0 on a fresh pool), and the recheck's
    /// fold-through-fence would re-fold (0, fence_version] and re-add nodes that retire's defensive
    /// 404-prune had just forgotten — the P9-forget regression.
    bool folded_any = false;
    std::map<String, uint64_t> folded_to;   /// cursor_key -> position fold folded to (shard_version)
    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = ns.string() + "/" + std::to_string(root_shard);
        const uint64_t cursor = cursorOf(result.snap, cursor_key);
        folded_to[cursor_key] = root.shard_version;

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

        auto transitioned = foldShardRecords(result.snap, state, ns, cursor_key, root, cursor, root.shard_version);
        result.transitioned.insert(result.transitioned.end(), transitioned.begin(), transitioned.end());

        /// B171 precommit reclaim (§C.3): a precommit shard may hold precommit refs of builds no longer
        /// live — ABANDONED precommits whose edges must be released so the closures become normal GC
        /// candidates. We hold the shard manifest in hand under the GC lease; reclaim drops each dead
        /// build's ref (keyed by build_seq, parsed from the ref name — the SAME erase + journal Remove
        /// shape `publish`/the premature-reclaim fixture use), and the NEXT fold sees the Removes and lifts
        /// the edges. Reclaiming AFTER folding is conservative: this round's
        /// snap still over-counts the released edges (retire never under-deletes a live ref), the next
        /// round folds the Remove. The mutate is on the shard manifest — independent of the fold's own
        /// gc/state + gc/snap CAS below — so it does not fight the fold.
        if (Layout::isPrecommitNamespace(ns))
            reclaimAbandonedPrecommit(ns, root_shard, root, state.round + 1);
    }

    /// No new records since the last fold: the snap is unchanged and already durable at
    /// state.snap_generation. Skip the whole-snap re-write AND fold's own gc/state CAS.
    /// state_token is returned UNMODIFIED, so retire's round CAS rides the incoming lease token
    /// (preserving the zombie-steal protection runRegularRound documents).
    if (!folded_any)
        return result;

    /// 4. Persist, durable-before-cursor: the new-generation snap objects go durable FIRST; only
    /// then does the gc/state CAS advance snap_generation. A crash between the two loses nothing
    /// - the old generation stays authoritative. The persisted snap shard 0 carries folded_cursor
    /// advanced to the position fold folded to (each shard's shard_version) — (edges, cursor) are
    /// one write-once unit (B140-dangle fix); the cascade later advances the SAME cursor through the
    /// fence window. fold writes only its OWN folded extent, never the fence version (it has not
    /// folded the fence window), so the cursor is never ahead of the edges.
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
            /// Advance the fold cursor in snap shard 0 (the single source of truth, B140-dangle fix)
            /// to the position fold ACTUALLY folded to (each shard's shard_version). This makes the
            /// persisted generation self-coherent: (edges, cursor) are one write-once unit and the
            /// cursor never runs ahead of the edges it represents. Idempotent under the probe-upward
            /// retry (std::max), so the encoded bytes stay stable for byte-equal adoption.
            if (snap_shard == 0)
            {
                for (const auto & [cursor_key, folded_version] : folded_to)
                    snap.folded_cursor[cursor_key]
                        = std::max(snap.folded_cursor[cursor_key], folded_version);
            }
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
    const CasOutcome outcome = store.backend().casPut(key, encodeGcHeartbeat(hb), expected, /*out_token=*/nullptr);
    /// B170: the advisory liveness pulse (B160) — a leader bumping gc/hb so a follower's steal backs
    /// off. Distinct from the lease-renew GcLeaseHeartbeat in acquireOrRenewLease; only the committed
    /// pulse is an event (a Conflict means another writer raced us — best-effort, the next pulse retries).
    if (outcome == CasOutcome::Committed)
    {
        CasEvent ev;
        ev.type = CasEventType::GcLeaseHeartbeat;
        ev.object_kind = CasEventObjectKind::Snap;
        ev.outcome = "pulsed";
        ev.reason = "advisory heartbeat pulse (B160): bumped gc/hb so a follower steal backs off";
        ev.detail = {{"owner", u128ToHex(gc_id)}, {"hb_seq", std::to_string(hb.hb_seq)}};
        store.emitEvent(ev);
    }
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
    /// report nullptr — reclaim then treats the owning build as dead and reclaims its precommit.
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

void Gc::reclaimAbandonedPrecommit(const RootNamespace & ns, uint64_t shard, const RootShard & root,
                                   uint64_t round)
{
    /// B171 §C.3 (design §4.3, fixed 2026-06-19): reclaim ABANDONED precommits — those whose owning
    /// build is no longer live. The precommit namespace is sharded like any namespace now (ref name == build_seq,
    /// shard == shardOf(build_seq)), so ONE precommit shard holds MANY precommit refs, each keyed by its
    /// owning build's `build_seq`. We derive the server from the namespace (`<server_hex>/_precommits`) and
    /// the build_seq from each REF NAME. Protection here is the precommit EDGE (the root ref the fold
    /// expands); dropping a ref releases that protection, so we reclaim ONLY refs whose build is provably
    /// dead. We read the shard manifest already in hand (the fold's `root`) — no extra read per ref.
    ///
    /// Nothing to reclaim if the shard carries no refs (already-reclaimed / never-published / committed
    /// builds that removed their own ref — all leave a ref-less or Remove'd shard).
    if (root.refs.empty())
        return;

    /// Derive the owning server_id from `<server_hex>/_precommits` — the segment BEFORE `/_precommits`.
    const String & ns_str = ns.string();
    static constexpr std::string_view kPrecommitsSuffix = "/_precommits";
    chassert(ns_str.ends_with(kPrecommitsSuffix));
    const String server_hex = ns_str.substr(0, ns_str.size() - kPrecommitsSuffix.size());
    if (server_hex.size() != 32)
        return;   /// not a well-formed precommit namespace — leave it untouched (never reclaim blindly)
    UInt128 server;
    try
    {
        server = hexToU128(server_hex);
    }
    catch (...)
    {
        return;
    }

    /// LIVENESS verdict reuses the existing watermark machinery (watermarkOf + the K=2 seq-freshness
    /// verdict computed once per round in beginWatermarkRound's caches). A precommit's owning build is
    /// DEAD iff:
    ///   - the server has no watermark (unrecognized / never-anchored — a stale or vanished owner), OR
    ///   - the server is judged not-live this round (frozen seq for K=2 rounds => crash), OR
    ///   - this build's seq is below the server's `min_active` floor (the build retired — clean abort,
    ///     publish, or crash whose restart raised the floor; min_active == UINT64_MAX (farewell) also
    ///     satisfies this, retiring every seq).
    /// The build epoch is NOT stored per-precommit, so we judge by server-liveness + the min_active
    /// floor (design §4.3): a hard-killed server returns either no watermark (vanished) or a frozen seq
    /// (K=2 dead), and a restarted server's farewell/new-epoch renewal raises min_active past the
    /// orphaned seq — both reclaim. A LIVE build (seq >= min_active AND server live) is LEFT intact.
    const ServerWatermark * w = watermarkOf(server);
    const bool server_live = server_live_this_round[server];

    /// Decide which refs to reclaim from the shard manifest in hand, parsing build_seq from each ref
    /// NAME. A ref whose name is not a parseable build_seq is left untouched (never reclaim blindly).
    struct DeadRef { String name; UInt128 tree; uint64_t build_seq; };
    std::vector<DeadRef> dead_refs;
    for (const auto & [name, payload] : root.refs)
    {
        uint64_t build_seq = 0;
        try
        {
            size_t consumed = 0;
            build_seq = std::stoull(name, &consumed);
            if (consumed != name.size())
                continue;   /// trailing garbage — not a clean build_seq ref
        }
        catch (...)
        {
            continue;   /// non-numeric ref name — leave it (never reclaim blindly)
        }

        const bool dead = (w == nullptr) || !server_live || build_seq < w->min_active;
        if (dead)
            dead_refs.push_back(DeadRef{name, payload.tree_id, build_seq});
    }

    if (dead_refs.empty())
        return;

    /// Reclaim: drop each dead precommit ref + journal Remove on the SAME shard (mirrors the commit-time
    /// removal in `Build::publish`). The next fold reads the Removes and releases the edges, so the
    /// closures' objects become normal zero-in-degree candidates. ONE mutateShard CAS removes all dead
    /// refs at once; it re-reads + re-runs the lambda per attempt, so an already-removed/raced ref is a
    /// no-op.
    store->mutateShard(ns, shard, [&](RootShard & fresh)
    {
        for (const DeadRef & dr : dead_refs)
        {
            auto it = fresh.refs.find(dr.name);
            if (it == fresh.refs.end())
                continue;   /// raced — a concurrent commit/reclaim already removed it
            const UInt128 tree = it->second.tree_id;
            fresh.refs.erase(it);
            fresh.journal.push_back(JournalRecord{
                .op = JournalRecord::Op::Remove, .ref_name = dr.name, .tree_id = tree,
                .at_version = fresh.shard_version + 1, .closure = {}});
        }
    });

    if (store->hasEventSink())
    {
        for (const DeadRef & dr : dead_refs)
        {
            CasEvent ev;
            ev.type = CasEventType::PrecommitReclaim;
            ev.namespace_ = ns_str;
            ev.ref_name = dr.name;
            ev.object_kind = CasEventObjectKind::Tree;
            ev.object_hash = u128ToHex(dr.tree);
            ev.round = round;
            ev.outcome = "reclaimed";
            ev.reason = w == nullptr
                ? "precommit reclaim: owning server has no watermark (abandoned/vanished build)"
                : (!server_live
                    ? "precommit reclaim: owning server frozen K=2 rounds (crashed build)"
                    : "precommit reclaim: build_seq below the server's min_active floor (retired build)");
            ev.detail = {{"build_seq", std::to_string(dr.build_seq)},
                         {"min_active", w ? std::to_string(w->min_active) : "absent"}};
            store->emitEvent(ev);
        }
    }
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
                /// B170: lease acquired on a fresh pool (gc/state created holding our lease).
                {
                    CasEvent _ev24;
                    _ev24.type = CasEventType::GcLeaseAcquire;
                    _ev24.round = state.round;
                    _ev24.gen = state.snap_generation;
                    _ev24.outcome = "acquired";
                    _ev24.reason = "fresh pool: created gc/state holding our lease";
                    _ev24.detail = {{"owner", u128ToHex(gc_id)}, {"seq", std::to_string(state.lease.seq)},
                               {"fence_seq", std::to_string(state.fence_seq)}};
                    store->emitEvent(_ev24);
                }
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
                /// B170: lease renewed (seq advanced; fence_seq unchanged — not a new epoch).
                {
                    CasEvent _ev25;
                    _ev25.type = CasEventType::GcLeaseHeartbeat;
                    _ev25.round = state.round;
                    _ev25.gen = state.snap_generation;
                    _ev25.outcome = "renewed";
                    _ev25.reason = "lease renew: seq advanced (same epoch)";
                    _ev25.detail = {{"owner", u128ToHex(gc_id)}, {"seq", std::to_string(state.lease.seq)},
                               {"fence_seq", std::to_string(state.fence_seq)}};
                    store->emitEvent(_ev25);
                }
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
            /// B170: lease STOLEN from a dead incumbent — fence_seq++ opens a new leadership epoch.
            {
                CasEvent _ev26;
                _ev26.type = CasEventType::GcLeaseSteal;
                _ev26.round = state.round;
                _ev26.gen = state.snap_generation;
                _ev26.outcome = "stolen";
                _ev26.reason = "incumbent did not renew across our observation window; stole the lease";
                _ev26.detail = {{"owner", u128ToHex(gc_id)},
                           {"prev_owner", u128ToHex(current.lease.owner)},
                           {"seq", std::to_string(state.lease.seq)},
                           {"fence_seq", std::to_string(state.fence_seq)}};
                store->emitEvent(_ev26);
            }
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
