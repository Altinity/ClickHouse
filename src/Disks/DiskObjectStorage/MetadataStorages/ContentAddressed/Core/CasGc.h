#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/ThreadPool.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// The logical (GC-bookkeeping) size of a retired object: a blob subtracts the pool's fixed
/// blob_header_len (a blob OBJECT smaller than the fixed header is corrupt — CORRUPTED_DATA,
/// fail closed, never a wrapped-around size). Sizes feed cost/health accounting only — no protocol
/// decision ever reads them.
uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len);

/// Phase-4 skip-unchanged decision (spec 2026-07-06). Pure. Returns true iff the current round may be
/// DEFERRED (re-adopt the sealed generation, no fold/delete). A round MUST fold when: enough shards
/// changed (>= fold_threshold), OR a destructive decision is due (graduation_due), OR the defer bound
/// is reached (rounds_since_last_fold >= fold_max_defer_rounds). The graduation_due term is the
/// load-bearing safety guard: no destructive decision ever runs on a not-fully-folded snapshot.
bool shouldDeferRound(size_t changed_shards, bool graduation_due, uint64_t rounds_since_last_fold,
                      uint64_t fold_threshold, uint64_t fold_max_defer_rounds);

/// One anomaly the fold surfaced (a clamped cursor — a missing committed/removal body, or the fold
/// barrier on a live missing-body precommit). Surfaced to fsck/logs; NEVER a throw (record-and-continue,
/// per feedback_ca_gc_never_throw_on_404).
struct RoundAnomaly
{
    RootNamespace ns;
    uint64_t shard = 0;
    ManifestId id;
    String reason;
};

/// What one runRegularRound did (counters are health metrics, not protocol state).
/// Outcome of `Gc::rebuildBaseline` (spec 2026-07-03-cas-gc-rebuild-design.md): either performed
/// (with the minted numbering + coverage counters) or refused (fail-closed; `refusal` says why and
/// nothing was committed — gc/state is untouched by a refused rebuild).
struct RebuildReport
{
    bool performed = false;
    String refusal;
    uint64_t round = 0;
    uint64_t generation = 0;
    uint64_t namespaces = 0;
    uint64_t shards = 0;
    uint64_t committed_refs = 0;
    uint64_t live_precommits = 0;
    uint64_t unowned_alive_manifests = 0;   /// over-protected trimmed-but-live class (design delta 2)
    uint64_t edges = 0;
    uint64_t clamped_shards = 0;
};

struct RoundReport
{
    bool acquired_lease = false;  /// false => another leader is alive; nothing else was done
    /// Phase-4 skip-unchanged (spec 2026-07-06): true iff this round DEFERRED (re-adopted the sealed
    /// in-degree generation instead of folding). A deferred round performs no fold, no pre-CAS
    /// deletes, and no gc/state CAS -- every other RoundReport field below is meaningless/zero on it.
    bool deferred = false;
    uint64_t round = 0;
    uint64_t candidates = 0;      /// retired entries WRITTEN this round (absent candidates are skipped)
    uint64_t deleted = 0;
    uint64_t absent = 0;
    uint64_t replaced = 0;        /// 412-saves — a health metric
    uint64_t spared = 0;
    uint64_t manifests_deleted = 0;  /// owner-removed manifest bodies deleted (B11 — distinct from blob deletes)
    /// Retired-cursor pipeline observability (Task 11): the pipeline's per-round transitions.
    size_t condemned = 0;         /// entries newly condemned into the retired list this round
    size_t graduated = 0;         /// entries newly floor-passed (published delete_pending) this round
    size_t redeleted = 0;         /// pending deletes executed this round (exact-token blob deletes)
    size_t fence_outs = 0;        /// expired mounts fenced-out by the round's heartbeat floor
    /// Task 5: per-hash freshness META write ops (condemn/spare/delete) that threw on the bounded pool
    /// this round. Advisory-only failures (never wedge the round) but a persistently non-zero count means
    /// the writer's meta point-read gate is drifting from the ledger -- an operator signal, not a protocol
    /// input.
    uint64_t meta_write_anomalies = 0;
    std::vector<RoundAnomaly> anomalies;   /// fold clamps surfaced this round (never wedge the round)

    /// Record a fold/recheck anomaly (a clamped cursor). Surfacing, never throwing.
    void recordAnomaly(const RootNamespace & ns_, uint64_t shard_, const ManifestId & id_, const char * reason_)
    {
        anomalies.push_back(RoundAnomaly{.ns = ns_, .shard = shard_, .id = id_, .reason = reason_});
    }
    bool hasAnomaly(const RootNamespace & ns_, uint64_t shard_) const
    {
        for (const RoundAnomaly & a : anomalies)
            if (a.ns.string() == ns_.string() && a.shard == shard_)
                return true;
        return false;
    }
};

/// Leader-paced regular GC (ack-floor redesign, spec 2026-07-02): ONE pass per round — heartbeat
/// ack floor -> fold (three-cursor merge) -> pre-CAS deletes of previously-published pending
/// entries -> single gc/state CAS -> post-CAS cleanup/trim, over the root-local part-manifest model. The lease is WORK DEDUP ONLY —
/// every step is idempotent and split-brain-safe (monotone gc/state, append-by-unique-path retire/
/// outcome logs, exact-token deletes); the Phase-0 model (CaGcRootLocalPartManifestCore.tla) proves the
/// round safe with NO leadership assumption at all. A stale leader can only duplicate work, never roll
/// back state or mis-delete.
///
/// LEASE / STEAL WINDOW (deterministic — this class NEVER reads a clock). The lease lives inside
/// gc/state as {owner, seq} and moves only by CAS on the whole gc/state object. See
/// acquireOrRenewLease for the full observation/renew/steal protocol (unchanged by the redesign).
///
/// NOT thread-safe: one pacing thread drives a Gc instance. gc_id uniqueness across instances
/// (a random u128) is a CALLER obligation — duplicate ids make two leaders indistinguishable.
class Gc
{
public:
    /// `now_ms_fn` is the ONLY clock in Gc (injected for tests): the ack-floor heartbeat gate uses it
    /// for the inherited lease-expiry classification (spec: the single timing assumption). Everything
    /// else in the round stays deterministic/clock-free.
    Gc(StorePtr store_, UInt128 gc_id_, std::function<uint64_t()> now_ms_fn_ = {});

    /// One full round. Returns acquired_lease=false (nothing else done) if another leader is alive.
    /// `on_lease_acquired`, if set, is invoked ONCE, synchronously, immediately after the lease is
    /// acquired/renewed and BEFORE the (potentially long) fold begins - the scheduler uses this to
    /// mark itself leader and fire the first advisory heartbeat pulse right away (B160/P3-B1: a new
    /// leader's first round must not run unprotected for the whole fold before the pacing thread's
    /// post-round bookkeeping would otherwise have set it). Never called when the lease is not held;
    /// exceptions from the callback propagate like any other round failure (the caller is expected to
    /// keep it advisory/non-throwing, matching pulseHeartbeat's own contract).
    RoundReport runRegularRound(std::function<void()> on_lease_acquired = {});

    /// B160 advisory heartbeat: bump <prefix>/gc/hb to {gc_id, hb_seq+1}. Best-effort (a lost CAS is
    /// harmless — the next pulse retries). Touches NO Gc instance state. Static by design.
    static void pulseHeartbeat(Store & store, UInt128 gc_id);


    /// One previewed deletion the next regular round would make, with the reason it is eligible.
    struct PreviewEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        BlobRef ref{};
        String key;
        uint64_t size = 0;
        String reason;          /// "unreachable" | "delete_pending" | "awaiting_graduation"
        Token token;            /// stored condemn-time token (empty for "unreachable")
        uint64_t condemn_round = 0;
    };

    /// WRITE-FREE preview of the next round's deletes, derived from the DURABLE sealed in-degree
    /// generation + gc/state. Diagnostic / cross-check ONLY — its output must never feed a real delete.
    /// It reads the durable generation WITHOUT folding new owner events, so at NON-QUIESCENCE it can
    /// OVER-REPORT a blob a since-landed publish re-referenced (the real round folds first and spares
    /// it). The {preview} ⊆ {genuinely-unreachable} guarantee holds ONLY at quiescence. No CAS/delete.
    std::vector<PreviewEntry> previewDeletes();

    /// Raw baseline rebuild — the gc/state disaster-recovery command (spec 2026-07-03). Recomputes
    /// the in-degree snapshot from raw owner state (committed refs + live precommits + the unowned
    /// not-provably-dead over-protection), mints round/generation above every surviving ack/number,
    /// publishes EMPTY retired lists, and CASes gc/state. Live-conservative; fail-closed refusals.
    RebuildReport rebuildBaseline(bool force);

    void setRebuildEdgeBudgetForTest(uint64_t n) { rebuild_edge_budget_override = n; }

    /// TEST SEAM (M1 regression): disable the round's journal trim so a folded event stays in the journal
    /// across rounds — exactly the Phase-3 lazy-trim / partial-trim-after-crash condition under which the
    /// next round's fold MUST recover the exact sealed cursor (else it re-folds the event and double-counts
    /// blob in-degree). Production never calls this; trim is always enabled.
    void setTrimEnabledForTest(bool enabled) { trim_enabled = enabled; }

    /// B12 lazy-trim TEST SEAM: enable maintenance (full-compaction) mode for the next round.
    /// When true, trim ignores the event-count and body-size thresholds and compacts every shard that
    /// has any trimmable events (the pre-B12 eager behaviour). Cleared to false after each round so
    /// each test call arms exactly one round. Production never calls this; maintenance mode is driven
    /// externally (e.g. by a scheduled maintenance task, not yet wired).
    void setMaintenanceTrimForTest(bool enabled) { maintenance_trim = enabled; }

    /// (GC-side abandoned-precommit reclaim was removed with the snapshot+log ref model: dead-precommit
    /// cleanup is now the writer's job -- it appends exact `owner_transition` removals in ref logs, spec
    /// §Failed Precommit Cleanup / §Clean Up Old Precommits. `GC` never invents a ref transition.)

    /// DELETE-SITE AUDIT (closeout invariant; grep `deleteExact` under Core/): the round's pre-CAS
    /// redelete phase (R3 in runRegularRound) holds the ONLY content (blob reachability) delete in
    /// the whole core, restricted to previously-published delete_pending entries.
    /// The recheck's manifest-body exact-token delete (after its decrements are sealed), the retired-set
    /// drop, the resume path (GC metadata), dropNamespace (verbatim files), and the capability probe
    /// (throwaway keys) remove non-content objects they own. Adding a second content-delete site is a
    /// protocol defect.

private:
    /// Lease acquire/renew/steal per the documented observation protocol. On success `state` holds the
    /// committed gc/state (with our lease) and `state_token` its backend token.
    bool acquireOrRenewLease(GcState & state, Token & state_token);

    /// What one R1 fold produced (spec rev. 15 §Fold Owner Transitions). The blob deltas are sealed
    /// into a write-once generation; `fold_seal` is the durable index of WHAT WAS FOLDED (a CasFoldSeal),
    /// `root_shards` the discovered universe, `mf_cleanup` the part-manifest cleanup work keyed by
    /// ManifestId (owner-removed bodies whose exact-token delete is deferred until their decrements are
    /// sealed — spec §Retire), and `retired_merge` the per-gc-shard ack-floor retired-cursor outcome.
    struct FoldResult
    {
        CasFoldSeal fold_seal;
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;
        std::map<ManifestId, Token> mf_cleanup;
        /// Ack-floor one-pass round: the retired-cursor outcome per gc-shard (settled entries, new
        /// condemnations, floor-passed pendings, and the prior pendings to delete pre-CAS).
        std::vector<RetiredMergeResult> retired_merge;
        /// The round's one global ref LIST, grouped per table (spec §Step 1). Reused post-CAS for
        /// ref-object cleanup (covered logs / superseded snapshots) so a second LIST is never issued.
        std::map<String, RefTableListing> ref_tables;
    };

    /// R1 (spec rev. 15 §Fold Owner Transitions): per changed root shard, stream the ONE ordered
    /// RootOwnerEvent journal in transition_version order and dispatch each event by comparing
    /// old_binding.manifest_ref to new_binding.manifest_ref:
    ///   - EQUAL (an owner move, e.g. a promote Precommit->Committed at the SAME ref) => NO blob delta,
    ///     NO part-manifest cleanup (the activating PrecommitAdd was folded earlier — see the barrier);
    ///   - TRUE REMOVAL (old present, the ref not owned afterwards) => read the OLD body, emit -1 per
    ///     blob entry + queue the body for cleanup (an old precommit never activated emitted no edges);
    ///   - ACTIVATION (new present) => read the NEW body, emit +1 per blob entry, SUBJECT TO THE FOLD
    ///     BARRIER (control #23): do NOT advance the durable fold cursor past a RootOwnerEvent that
    ///     leaves a LIVE precommit binding whose manifest body is not present+valid; re-read each round.
    /// 404 RULE: a body that is PRESENT-but-invalid (ref/namespace mismatch) is genuine corruption =>
    /// CORRUPTED_DATA (hard). A MISSING body (404) is handled by where it appears: a precommit
    /// activation new missing body => no edges + barrier holds the cursor; a committed/promote new
    /// missing body or a true-removal old body missing at removal-fold => fail-closed FOR THAT DECISION
    /// (clamp the shard's folded_cursor below it, record the anomaly, stop folding THIS shard) — never
    /// guess a delta, never throw/wedge (feedback_ca_gc_never_throw_on_404).
    /// On success `state` carries the committed snap_generation and `state_token` the committed gc/state
    /// token. The committed pair is THREADED into retire, never re-read (zombie-steal protection).
    /// Round-paced graduation: `current_round` (= state.round + 1, the SAME basis condemn_round is
    /// stamped at) is the threshold the fold's three-cursor merge graduates/condemns against — an entry
    /// graduates once `condemn_round < current_round`, i.e. it survived at least one full round after
    /// being condemned. The fold no longer CASes gc/state — it sets (snap_generation, snap_attempt)
    /// in-memory; the SINGLE round CAS commits them.
    FoldResult fold(GcState & state, Token & state_token, RoundReport & report, uint64_t current_round);

    /// Read ONE part manifest named by `id`, validate it, and append sign*(+1) blob deltas for each
    /// blob entry to `deltas`. On sign<0 queue (id -> token) into mf_cleanup. Returns whether a body was
    /// read+validated: false => ABSENT body (404; the caller decides per the 404 rule). A body that is
    /// PRESENT but fails refMatchesBody / manifestNamespaceMatches throws CORRUPTED_DATA.
    bool foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup);




    /// Ref-object cleanup (spec §Step 6 / §Concurrent Startup And Cleanup): delete each table's ref logs
    /// covered by BOTH the durable fold cursor AND an observed snapshot, and snapshots older than the
    /// newest observed one, in batches of <=1000 exact keys. A remove_namespace log is retained until its
    /// namespace-cleanup item is durably Completed. Runs post-CAS (durable cursor) and only on a clamp-free
    /// round (`suppress_destructive == false`). Acts only on keys THIS round's scan returned (reused via
    /// `folded.ref_tables`), so a covering snapshot is always durable before any deletion it authorizes.
    void cleanupRefObjects(const FoldResult & folded, bool suppress_destructive);

    /// Namespace-cleanup item passes (spec §Step 6): for each item in the committed seal, a Pending item
    /// runs a bounded exact-key enumerate-and-delete pass over the removed namespace's physical `@cas@`
    /// prefixes (manifest bodies + verbatim files); a Completed item publishes the `_cleanup` marker and
    /// republishes the constant-size `Removed` snapshot (both idempotent `putIfAbsent`). Runs post-CAS, so
    /// winning the round's gc/state CAS is the leadership fence that gates publication.
    void runNamespaceCleanupPasses(const CasFoldSeal & seal, bool suppress_destructive);

    /// Whether a namespace's physical `@cas@` metadata prefixes (manifest bodies + verbatim files) hold no
    /// object -- the Pending->Completed condition for its namespace-cleanup item. LIST-only, no delete.
    bool namespacePhysicallyEmpty(const RootNamespace & ns);

    /// Best-effort cursor-paced orphan part-manifest sweep. This is cleanup-only state: a lost CAS only
    /// discards cursor progress and must not fail the already-completed GC round.
    void runManifestSweepCursorPass(GcState & state, Token & state_token);

    /// Discover the present (namespace, shard) pairs from LIST(cas/refs/); shared by the fold and the
    /// resume re-fence. Returns only shards that have a backend object (absent shards not included).
    /// Fresh pool (no shards yet) => empty result.
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverse();

    /// Task 3 (Phase 4 Lever A): the two cheap pre-fold GC round-defer signals — both computed from
    /// state already reachable before the fold's snapshot merge (O(retired)/O(shards), no snapshot
    /// read), so `runRegularRound` can decide DEFER-vs-FOLD before paying the fold's cost.
    ///
    /// True iff a graduation is due this round, read ZERO-I/O from the adopted fold seal's per-shard
    /// `condemned_summary` (retired-in-snapshot T4): `∃ shard: pending_total > 0 ||
    /// oldest_nonpending_condemn_round < current_round`. `snap_generation == 0` (fresh pool) => false.
    /// This is the load-bearing SAFETY signal (Task-1 TLA+ gate, GREEN): it forces a fold before any
    /// destructive decision. FAIL-CLOSED: a missing / undecodable seal, or a summary not TOTAL over
    /// gc_shards, is corrupt GC bookkeeping — returns TRUE (forces a fold so the round's own fail-closed
    /// path surfaces it); a round must never silently defer on corrupt bookkeeping.
    bool graduationDue(const GcState & state, uint64_t current_round);

    /// The number of tables (namespaces) with at least one ref log above their sealed durable cursor --
    /// the pre-fold DEFER signal (spec §Step 1). Computed from one `LIST cas/refs/` compared against the
    /// per-table cursors in the adopted fold seal at `state.snap_generation`/`snap_attempt`.
    size_t changedShardCount(const GcState & state);

    /// (reclaimDroppedShards was removed with the snapshot+log ref model: there is no mutable per-namespace
    /// shard object to tombstone+reclaim; physical namespace reclamation is the namespace-cleanup item.)


    /// Write the part-manifest cleanup bundle(s) for one fold generation: a write-once record listing
    /// every owner-removed (ManifestId, token) whose body delete is deferred to recheck. Keyed by
    /// owner_shard (one bundle for gc_shards==1). Appends the produced RunRefs to `out`.
    void writePartManifestCleanupBundle(uint64_t generation, uint64_t attempt, uint64_t owner_shard,
                                        const std::map<ManifestId, Token> & cleanup, std::vector<RunRef> & out);

    /// B9 retention (attempt-scoped). The SOLE reclaimer — bounded per round and FAIL-OPEN on a benign
    /// 404 (never throw during a prune — it would only wedge GC):
    ///   WHOLESALE generation-retention: every generation at or below the retention floor
    ///   (`adopted_generation - gc_snap_generations_to_keep`) is reclaimed by LISTing its whole
    ///   `gc/gen/<g>/` prefix and deleting every object — ALL attempts, including the attempt-scoped
    ///   retired/ and outcomes/ sets AND any deposed-leader debris under a non-adopted attempt. Walks
    ///   forward from `next.snap_pruned_through`, advancing it over generations fully reclaimed (persisted
    ///   by the round-commit CAS).
    /// There is deliberately NO per-round current-generation attempt-sweep (it cost a per-round LIST for a
    /// rare concurrent-leader collision, the GC-DISCOVERY-LIST-QUADRATIC concern). Deposed-leader
    /// current-generation debris is bounded space that waits at most `keep` completion-advances for the
    /// wholesale prune to reclaim it. keep==0 prunes nothing (keep-all forensics mode). `attempt` is the
    /// adopted attempt (`next.snap_attempt`); it is currently unused (retention keys on generation alone).
    void pruneSupersededGenerations(uint64_t adopted_generation, uint64_t attempt, GcState & next,
                                    const std::set<uint64_t> & referenced_generations);

    /// Read the fold seal for (generation, attempt) (nullopt when absent). Used by resume + parent-cursor reads.
    std::optional<CasFoldSeal> readFoldSeal(uint64_t generation, uint64_t attempt);

    /// The per-(ns,shard) fold cursor coverage as of `generation`, read from the fold seal at
    /// (generation, attempt) (the one-pass round's coverage record; empty when absent). This is what the
    /// next fold keys its parent cursor off, so a folded event is never re-folded from 0 (no in-degree
    /// double-count).
    std::map<String, ShardCoverage> readSealedCursors(uint64_t generation, uint64_t attempt);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    /// Task 5: submit one per-hash freshness-meta op (condemn/spare/delete) to the bounded `meta_pool`.
    /// NEVER throws: `job` is wrapped in its own try/catch (an exception increments `meta_anomaly_count`
    /// + a log line, per feedback_ca_gc_never_throw_on_404); if scheduling itself fails (e.g. resource
    /// exhaustion) the op runs inline rather than being silently lost. Callers must capture every value
    /// `job` touches BY VALUE (never by reference to a loop-local like the fold's `cur_blob`, which
    /// mutates across iterations while this job may still be queued).
    void scheduleMetaJob(std::function<void()> job);

    StorePtr store;
    UInt128 gc_id{};
    uint64_t rebuild_edge_budget_override = 0;   /// tests force tiny batches
    std::function<uint64_t()> now_ms_fn;   /// wall-clock ms; injected (tests), defaults to system_clock              /// this leader's identity (random u128, never 0)
    bool trim_enabled = true;     /// TEST SEAM ONLY (M1): production always trims; see setTrimEnabledForTest
    bool maintenance_trim = false; /// B12 TEST SEAM: bypass lazy-trim thresholds for one round (full compaction); see setMaintenanceTrimForTest
    /// Phase-4 skip-unchanged (spec 2026-07-06): leader-local, in-memory count of consecutive DEFERRED
    /// rounds since the last FOLD. NOT persisted (a fresh/stolen leader starts at 0 -- conservative:
    /// it may fold one round sooner than a long-lived leader would, never later). Reset to 0 whenever
    /// a round folds; incremented on every DEFER. Bounds batching via `gc_fold_max_defer_rounds`.
    uint64_t rounds_since_last_fold_ = 0;

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
    /// B160: the heartbeat observed alongside the lease (gates the steal).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;

    /// Task 5: bounded pool for the round's per-hash freshness-meta writes (condemn/spare/delete);
    /// sized from `PoolConfig::gc_meta_pool_size` (constructed in the ctor, after the null/id checks --
    /// never touches a possibly-null `store` at member-init time). A `unique_ptr` (not a plain member)
    /// so construction can happen in the ctor body, after validating `store`.
    std::unique_ptr<ThreadPool> meta_pool;
    /// Count of meta-op jobs that threw this round (reset at the top of every `runRegularRound`,
    /// folded into `RoundReport::meta_write_anomalies` after the round's `meta_pool->wait()`). Written
    /// from pool worker threads, so atomic.
    std::atomic<uint64_t> meta_anomaly_count{0};

public:
    /// TEST SEAM: thin public wrapper so unit tests can call foldManifestEdges without driving a full round.
    bool foldManifestEdgesForTest(const ManifestId & id, bool activation, std::vector<BlobDelta> & deltas,
                                   std::map<ManifestId, Token> & cleanup)
    {
        return foldManifestEdges(id, activation ? +1 : -1, deltas, cleanup);
    }

    /// TEST SEAM (Task 4): expose LIST-based namespace/shard discovery so unit tests can assert the
    /// discovered universe equals the set of present ref shards without driving a full round.
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverseForTest()
    {
        return discoverUniverse();
    }

    /// TEST SEAM (Task 3): expose the two cheap pre-fold GC round-defer signals so unit tests can
    /// assert them directly without driving a full round.
    bool graduationDueForTest(const GcState & state, uint64_t current_round)
    {
        return graduationDue(state, current_round);
    }

    size_t changedShardCountForTest(const GcState & state)
    {
        return changedShardCount(state);
    }

};

}
