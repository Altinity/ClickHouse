#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/Logger.h>
#include <Common/ThreadPool.h>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// WHOSE UNIVERSE THE DESTRUCTIVE GATE IS ALLOWED TO TRUST.
///
/// A round may destroy only while holding a FRONTIER PROOF for EVERY namespace that can hold a live
/// edge — because reachability is a property of the whole pool, not of one namespace. The proof per
/// namespace is cheap and exact (one `GET` at the cursor's arithmetic successor, `CasGc.cpp`'s intake
/// walk). What Stage A cannot supply is the SET the proofs must cover: there is no catalog, so the
/// round's universe is `(namespaces with a cursor in the adopted fold seal) ∪ (this round's LIST hint)`
/// — two sources that can BOTH omit a namespace at once.
///
/// The scenario that makes this a correctness question rather than a tidiness one (codex finding 1,
/// quoted): a hidden acked `+1` lands in a namespace absent from BOTH the hint and `gc/state`, while a
/// visible `-1` elsewhere drives the shared blob's OBSERVABLE in-degree to zero. Every namespace the
/// round knows about is walked to a proven frontier, every probe comes back clean, and the round would
/// delete a blob a durable committed edge still owns. No amount of per-namespace proof detects it: the
/// namespace is not in the set being proved.
///
/// So Stage A does not pretend. `StageA_Suppressed` makes `frontier_incomplete` unconditionally true,
/// which suppresses every destructive site in the round — the pool leaks until Stage B, and that is the
/// deliberate trade. Stage B's catalog makes the universe knowable; its Task 7b flips `kDefault` to a
/// value that consults the per-namespace proofs, and that ONE LINE is the entire flip. Nothing else in
/// the gate changes shape, which is why the per-namespace logic is written and tested now.
///
/// PRODUCTION-UNREACHABLE, BY CONSTRUCTION: the policy is a PARAMETER with a default, not an override.
/// `AuthoritativeForTest` is passed EXPLICITLY by gtests at the round entry with a CLOSED universe they
/// construct themselves; the single production call site (`CasGcScheduler`) passes nothing. There is no
/// config key, no environment variable, no runtime flag, no settable member and no test-hook struct
/// that can reach it — a test-only header cannot override a constant compiled into production code, so
/// none is offered.
enum class UniversePolicy : uint8_t
{
    /// The universe is not knowable this stage; every round suppresses all destruction.
    StageA_Suppressed = 0,
    /// The caller asserts that the universe it constructed is CLOSED, so the per-namespace frontier
    /// proofs decide the gate on their own. Only gtests may assert this.
    AuthoritativeForTest = 1,

    /// STAGE B'S TASK 7b EDITS EXACTLY THIS LINE.
    kDefault = StageA_Suppressed,
};

/// The logical (GC-bookkeeping) size of a retired object: a blob subtracts the pool's fixed
/// blob_header_len (a blob OBJECT smaller than the fixed header is corrupt — CORRUPTED_DATA,
/// fail closed, never a wrapped-around size). Sizes feed cost/health accounting only — no protocol
/// decision ever reads them.
uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len);

/// Pure skip-unchanged decision. Returns true iff the current round may be
/// DEFERRED (re-adopt the sealed generation, no fold/delete). A round MUST fold when: enough shards
/// changed (>= fold_threshold), OR a destructive decision is due (graduation_due), OR the defer bound
/// is reached (rounds_since_last_fold >= fold_max_defer_rounds). The graduation_due term is the
/// load-bearing safety guard: no destructive decision ever runs on a not-fully-folded snapshot.
bool shouldDeferRound(size_t changed_shards, bool graduation_due, uint64_t rounds_since_last_fold,
                      uint64_t fold_threshold, uint64_t fold_max_defer_rounds);

/// One anomaly the fold surfaced (a clamped cursor — a missing committed/removal body, or the fold
/// barrier on a live missing-body precommit). Surfaced to fsck/logs; recording an anomaly never throws:
/// the round records the problem and continues, while the affected shard remains conservatively clamped.
struct RoundAnomaly
{
    RootNamespace ns;
    uint64_t shard = 0;
    ManifestId id;
    String reason;
};

/// Outcome of `Gc::rebuildBaseline`: either a live-conservative baseline was published, with its minted
/// numbering and coverage counters, or the rebuild was refused. Refusal is fail-closed: `refusal` says
/// why, no partial baseline is committed, and `gc/state` remains untouched.
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
    /// Trimmed-but-live manifests kept over-protected because ownership could not be proved.
    uint64_t unowned_alive_manifests = 0;
    uint64_t edges = 0;
    uint64_t clamped_shards = 0;
    /// This rebuild concluded FROM ENUMERATION ALONE that the pool had never sealed a baseline, and so
    /// carried NO durable hold forward. It is the single route by which a hold can still be dropped
    /// silently, so the hand-run disaster command REPORTS it rather than leaving it to a log line: on a
    /// pool that has ever completed a GC round, a 1 here means the enumeration lied.
    bool virgin_by_enumeration = false;
    /// WHICH generation's fold seal this rebuild carried holds from; 0 when it carried none. The two
    /// verdicts this command reaches about the pool's history are then both on its own row — whether a
    /// baseline was found at all, and which one it was. Grepping that out of the logs is the thing an
    /// operator should not have to do in the middle of a disaster, and it is also how a step-down past
    /// a crashed newest generation becomes visible rather than invisible.
    uint64_t adopted_seal_generation = 0;
};

/// Counters and diagnostics returned by one `runRegularRound`. These values describe work attempted or
/// observed by the round; durable `gc/state` and the fold artifacts remain the protocol state.
struct RoundReport
{
    bool acquired_lease = false;  /// false => another leader is alive; nothing else was done
    /// True iff this round deferred (re-adopted the sealed
    /// in-degree generation instead of folding). A deferred round performs no fold, no pre-CAS
    /// deletes, and no gc/state CAS -- every other RoundReport field below is meaningless/zero on it,
    /// EXCEPT `round` (below), which a deferred round still sets to the honest, already-adopted round
    /// number (see the defer branch of runRegularRound, CasGc.cpp) -- it just never advances it.
    bool deferred = false;
    uint64_t round = 0;
    uint64_t candidates = 0;      /// retired entries WRITTEN this round (absent candidates are skipped)
    uint64_t deleted = 0;
    uint64_t absent = 0;
    uint64_t replaced = 0;        /// 412-saves — a health metric
    uint64_t spared = 0;
    uint64_t manifests_deleted = 0;  /// owner-removed manifest bodies deleted, distinct from blob deletes
    /// Retired-cursor pipeline observability: the pipeline's per-round transitions.
    size_t condemned = 0;         /// entries newly condemned into the retired list this round
    size_t graduated = 0;         /// entries newly floor-passed (published delete_pending) this round
    size_t redeleted = 0;         /// pending deletes executed this round (exact-token blob deletes)
    size_t fence_outs = 0;        /// expired mounts fenced-out by the round's heartbeat floor
    std::vector<RoundAnomaly> anomalies;   /// fold clamps surfaced this round (never wedge the round)

    /// Retire-pipeline REMAINING sizes as of the gc/state this round's single CAS just published --
    /// unlike `condemned`/`graduated`/`redeleted` above (this round's DELTAs), these are the pipeline's
    /// current outstanding totals, summed over every gc-shard's sealed `CondemnedSummary`
    /// (`Formats/CasFoldSealFormat.h`). `pending_retired` = still delete_pending (graduated, awaiting the
    /// exact-token delete next pass); `pending_candidates` = condemned but not yet floor-passed;
    /// `pending_condemned` = their total (candidates + retired), the overall pipeline gauge. Left at 0 on
    /// a `!acquired_lease`/`deferred` round -- those flags already mark the row non-authoritative.
    size_t pending_candidates = 0;
    size_t pending_condemned = 0;
    size_t pending_retired = 0;

    /// Record a fold/recheck anomaly (a clamped cursor). Surfacing, never throwing.
    void recordAnomaly(const RootNamespace & ns_, uint64_t shard_, const ManifestId & id_, const char * reason_)
    {
        anomalies.push_back(RoundAnomaly{.ns = ns_, .shard = shard_, .id = id_, .reason = reason_});
    }

    /// Return whether this report already contains an anomaly for the specified namespace and shard.
    bool hasAnomaly(const RootNamespace & ns_, uint64_t shard_) const
    {
        for (const RoundAnomaly & a : anomalies)
            if (a.ns.string() == ns_.string() && a.shard == shard_)
                return true;
        return false;
    }
};

/// One phase of one GC round. Pure data with no Interpreters dependency -- the same discipline
/// `GcRoundLogRecord` follows, so the round engine can be instrumented without linking the system-log
/// machinery. `CasGcScheduler` converts it into one `Phase` row of
/// `system.content_addressed_garbage_collection_log`.
///
/// DELIBERATELY NO VERB COLUMNS. `profile_events` is this phase's delta of the ordinary process
/// counters, so `ProfileEvents['S3ListObjects']` on a `Phase` row answers "which phase burns the LIST
/// budget" with no schema invention; `metrics` carries only the semantic counts a phase computes for
/// itself and no counter can supply.
///
/// This lives in `CasGc.h` rather than next to `GcRoundLogRecord` in `CasGcScheduler.h` because
/// `Cas::Gc` holds the sink as a member, and `CasGcScheduler.h` already includes this header -- putting
/// it there would make the include cycle.
struct GcPhaseRecord
{
    String phase;
    UInt64 duration_us = 0;
    std::map<String, UInt64> metrics;
    std::map<String, UInt64> profile_events;   /// this phase's ProfileEvents delta
};

/// Where a phase record goes. Installed for the duration of one round by `CasGcScheduler`; empty
/// elsewhere (a unit test driving `Gc` directly emits nothing).
using GcPhaseSink = std::function<void(const GcPhaseRecord &)>;

/// THE ROUND'S ONE HINT ENUMERATION of `cas/refs/`, taken before the defer decision and consumed by
/// everything downstream: the DEFER signal (`changed_shards`), and — on a folding round — the strict
/// grouping the fold works from (`keys`, regrouped by `groupRefKeys`).
///
/// It is a HINT, never a census. The fold's intake walks the ref stream ARITHMETICALLY by exact key
/// (`cursor + 1`, epoch crossings proved through the seal chain), so an id this enumeration omits is
/// still folded, and the round listing the prefix a second time would buy the intake nothing. The
/// second enumeration this round used to perform — kept deliberately separate so the two could be
/// compared — now happens only on the rounds the sampled store-quality detector is due
/// (`PoolConfig::gc_probe_a_period`, `Gc::sampleRefListQuality`), and its result reaches nothing but
/// the detector's own report.
struct RefScanSummary
{
    size_t changed_shards = 0;                          /// tables with a log above their sealed cursor
    std::vector<String> keys;                           /// every listed key, verbatim, for the fold's strict grouping
    std::map<String, std::set<RefTxnId>> logs_by_ns;    /// Log-kind ids only, per namespace
    std::map<String, RefTxnId> max_log_by_ns;           /// greatest Log-kind id per namespace
};

/// PROBE B2 — end-to-end transaction accounting for ONE round. Round-local, never persisted.
///
/// The naive "intended vs applied" counter pair is VACUOUS: in the intake both counts increment in the
/// same basic block, so they cannot differ. This ledger separates them across the whole pipeline
/// instead — a transaction is `committed` when the intake merges its staged buffers, `produced` when
/// it emitted at least one `BlobDelta`, and `applied` only when a shard reducer actually CONSUMED one
/// of its deltas. A delta lost between the intake and a reducer (routing across gc shards, a skipped
/// bucket, a future filter) leaves a committed+produced transaction unapplied.
///
/// Marking happens at reducer CONSUMPTION, not at run flush: the in-degree model is a SET, so an
/// unmatched `-1` and a duplicate `+1` legitimately vanish inside the reducer and a flush-side mark
/// would fire on healthy rounds. Loss INSIDE the reducer's own set collapse is a different class,
/// covered by `CasGcUnmatchedRemoveDeltas` and by the mirror safety test in
/// `gtest_cas_holey_list_detector.cpp`; probe B2 does not claim it.
///
/// SINGLE-THREADED: the shard reducers run sequentially on the fold thread (`Gc::fold`), so `applied`
/// needs no synchronisation. A future parallel reducer must revisit this.
struct TxnApplyLedger
{
    std::vector<RefTxnId> txns;        /// ordinal -> the log id
    std::vector<String> namespaces;    /// ordinal -> namespace, for the failure message
    std::vector<uint8_t> produced;     /// this transaction emitted >= 1 BlobDelta
    std::vector<uint8_t> committed;    /// this transaction folded fully and merged into the round buffers
    /// >= 1 of this transaction's deltas was consumed by a reducer. Public and written through a raw
    /// pointer by `foldDeltasIntoGeneration`: that write sits in a loop over potentially millions of
    /// rows, where a `std::function` hop is not free.
    std::vector<uint8_t> applied;

    /// Open a transaction and return its round-local ordinal.
    uint32_t open(const RootNamespace & ns, const RefTxnId & id)
    {
        const uint32_t ordinal = static_cast<uint32_t>(txns.size());
        txns.push_back(id);
        namespaces.push_back(ns.string());
        produced.push_back(0);
        committed.push_back(0);
        applied.push_back(0);
        return ordinal;
    }
    void markProduced(uint32_t ordinal) { produced[ordinal] = 1; }
    void markCommitted(uint32_t ordinal) { committed[ordinal] = 1; }
    void markApplied(uint32_t ordinal) { applied[ordinal] = 1; }

    /// Ordinals that were committed AND produced deltas but whose deltas never reached a reducer.
    /// Empty on a healthy round.
    std::vector<uint32_t> unapplied() const
    {
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < txns.size(); ++i)
            if (committed[i] && produced[i] && !applied[i])
                out.push_back(i);
        return out;
    }
};

/// Leader-paced regular GC: one pass per round — heartbeat
/// ack floor -> fold (two-cursor merge) -> pre-CAS deletes of previously-published pending
/// entries -> single `gc/state` CAS -> post-CAS cleanup/trim, over the root-local part-manifest model. The lease is work deduplication only —
/// every step is idempotent and split-brain-safe (monotone `gc/state`, append-by-unique-path retire and
/// outcome logs, exact-token deletes). These properties mean that a stale leader can duplicate work but
/// cannot roll back state or delete a newer object incarnation; the safety argument does not depend on
/// the lease being perfectly exclusive.
///
/// LEASE / STEAL WINDOW (deterministic — this class NEVER reads a clock). The lease lives inside
/// `gc/state` as {owner, seq} and moves only by CAS on the whole `gc/state` object. The
/// `acquireOrRenewLease` method implements the observation, renewal, and steal protocol.
///
/// NOT thread-safe: one pacing thread drives a Gc instance. gc_id uniqueness across instances
/// (a random u128) is a CALLER obligation — duplicate ids make two leaders indistinguishable.
class Gc
{
public:
    /// `now_ms_fn` is the WALL clock (injected for tests): audit/diagnostic stamps only (e.g. the
    /// heartbeat floor's `now_ms` argument) — it never gates a fence decision. `mono_ms_fn` is the
    /// OBSERVATION clock: monotonic on this
    /// process, injected for tests, defaults to `Pool::bootMs()`, and the ONLY clock the heartbeat
    /// gate's own fence-out threshold is measured against (mirrors `claimMountAwaitingExpiry`'s
    /// `mono_ms_fn`, but at heartbeat-gate granularity — one GC round is one observation tick).
    /// Everything else in the round stays deterministic/clock-free.
    ///
    /// `log_` is the logger every round-engine log line is emitted through; pass a disk/srid-scoped
    /// logger (e.g. `CasGcScheduler`'s own `log`, built from a `fmt::format("{}::...", storage_path)`
    /// name) so a multi-disk process's GC logs are attributable. Defaults to the process-global
    /// `getLogger("CasGc")` for callers with no natural scope of their own (tests, one-shot commands).
    Gc(PoolPtr store_, UInt128 gc_id_, std::function<uint64_t()> now_ms_fn_ = {},
       std::function<uint64_t()> mono_ms_fn_ = {}, LoggerPtr log_ = nullptr);

    /// One full round. Returns acquired_lease=false (nothing else done) if another leader is alive.
    /// `on_lease_acquired`, if set, is invoked ONCE, synchronously, immediately after the lease is
    /// acquired/renewed and BEFORE the (potentially long) fold begins - the scheduler uses this to
    /// mark itself leader and fire the first advisory heartbeat pulse right away: a new
    /// leader's first round must not run unprotected for the whole fold before the pacing thread's
    /// post-round bookkeeping would otherwise have set it). Never called when the lease is not held;
    /// exceptions from the callback propagate like any other round failure (the caller is expected to
    /// keep it advisory/non-throwing, matching pulseHeartbeat's own contract).
    ///
    /// `allow_steal` (default true — the paced background loop's semantics, unchanged): gates the
    /// observation-window protocol's steal branch (see acquireOrRenewLease). The window's safety
    /// argument requires the two observations that flag an incumbent "frozen" to be spaced by real
    /// wall time (>= the heartbeat cadence H) so a live incumbent gets a chance to pulse in between —
    /// a guarantee only the loop's own interval-paced ticks provide. A caller with no such guarantee
    /// (e.g. a manual `SYSTEM ... GC` command, where two calls can land microseconds apart) must pass
    /// `false`: it may still acquire a FREE lease or renew ITS OWN, but never executes the steal CAS —
    /// dead-incumbent recovery stays the loop's job.
    ///
    /// `policy` is the destructive gate's universe seam — see `UniversePolicy`. Production passes
    /// nothing; a gtest that has constructed a CLOSED universe passes `AuthoritativeForTest` here, and
    /// this parameter is the ONLY way to reach the per-namespace frontier logic.
    RoundReport runRegularRound(std::function<void()> on_lease_acquired = {}, bool allow_steal = true,
                                UniversePolicy policy = UniversePolicy::kDefault);

    /// Advisory heartbeat: bump <prefix>/gc/hb to {gc_id, hb_seq+1}. Best-effort (a lost CAS is
    /// harmless — the next pulse retries). Touches NO Gc instance state. Static by design.
    static void pulseHeartbeat(Pool & store, UInt128 gc_id);


    /// One deletion that the next regular round would preview, with the reason it is eligible. This is
    /// diagnostic data only and does not carry the durable token or authorization for a delete by itself.
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

    /// Raw baseline rebuild — the `gc/state` disaster-recovery command. Recomputes
    /// the in-degree snapshot from raw owner state (committed refs + live precommits + the unowned
    /// not-provably-dead over-protection), mints round/generation above every surviving ack/number,
    /// publishes EMPTY retired lists, and CASes gc/state. Live-conservative; fail-closed refusals.
    RebuildReport rebuildBaseline(bool force);

    /// Install (or clear) the per-phase sink for the round that is about to run. `CasGcScheduler`
    /// installs it around one `runRegularRound` and clears it afterwards, so the sink never outlives the
    /// scheduler locals it captures. Not thread-safe, like the rest of `Gc`: one pacing thread drives one
    /// instance, and the scheduler holds `gc_round_mutex` across both the install and the round.
    void setPhaseSink(GcPhaseSink sink) { phase_sink = std::move(sink); }

    void setRebuildEdgeBudgetForTest(uint64_t n) { rebuild_edge_budget_override = n; }

    /// TEST SEAM: disable the round's journal trim so a folded event stays in the journal
    /// across rounds — exactly the lazy-trim / partial-trim-after-crash condition under which the
    /// next round's fold MUST recover the exact sealed cursor (else it re-folds the event and double-counts
    /// blob in-degree). Production never calls this; trim is always enabled.
    void setTrimEnabledForTest(bool enabled) { trim_enabled = enabled; }

    /// Abandoned-precommit cleanup belongs to the writer: it appends exact `owner_transition` removals in
    /// ref logs. GC never invents a ref transition, because doing so could make the fold disagree with the
    /// writer's durable ownership history.

    /// DELETE-SITE INVARIANT: the round's pre-CAS
    /// redelete phase in `runRegularRound` holds the ONLY content (blob reachability) delete in
    /// the whole core, restricted to previously-published delete_pending entries.
    /// The recheck's manifest-body exact-token delete (after its decrements are sealed), the retired-set
    /// drop, the resume path (GC metadata), dropNamespace (verbatim files), and the capability probe
    /// (throwaway keys) remove non-content objects they own. Adding a second content-delete site is a
    /// protocol defect.

private:
    /// Lease acquire/renew/steal per the documented observation protocol. On success `state` holds the
    /// committed gc/state (with our lease) and `state_token` its backend token. `allow_steal=false`
    /// suppresses only the steal CAS (see runRegularRound's doc comment) — acquiring a free lease and
    /// renewing our own are unaffected.
    bool acquireOrRenewLease(GcState & state, Token & state_token, bool allow_steal);

    /// What one fold produced. The blob deltas are sealed
    /// into a write-once generation; `fold_seal` is the durable index of WHAT WAS FOLDED (a CasFoldSeal),
    /// `root_shards` the discovered universe, `mf_cleanup` the part-manifest cleanup work keyed by
    /// ManifestId (owner-removed bodies whose exact-token delete is deferred until their decrements are
    /// sealed), and `retired_merge` the per-gc-shard ack-floor retired-cursor outcome.
    struct FoldResult
    {
        CasFoldSeal fold_seal;
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;
        std::map<ManifestId, Token> mf_cleanup;
        /// Ack-floor one-pass round: the retired-cursor outcome per gc-shard (settled entries, new
        /// condemnations, floor-passed pendings, and the prior pendings to delete pre-CAS).
        std::vector<RetiredMergeResult> retired_merge;
        /// The round's one global ref LIST, grouped per table. Reused post-CAS for
        /// ref-object cleanup (covered logs / superseded snapshots) so a second LIST is never issued.
        std::map<String, RefTableListing> ref_tables;

        /// The round's per-namespace `_ckpt.checkpoint`, read ONCE by the intake walk (its second,
        /// hint-independent witness) and reused post-CAS by `cleanupRefObjects` for its delete ranges --
        /// the same DRY reason `ref_tables` is carried here rather than re-listed. A namespace whose
        /// `_ckpt` is present but UNDECODABLE has no entry, and the walk either HELD it or -- when it
        /// offered no position to walk from -- RECORDED AN ANOMALY for it. Either way an absent entry
        /// widens the delete boundaries, so only the shut destructive gate makes that absence safe.
        std::map<String, RefTxnId> checkpoints;
        /// THE DESTRUCTIVE GATE for this round, computed ONCE in `fold()` and threaded everywhere so no
        /// two destructive sites can disagree about it:
        ///
        ///     suppress_destructive = any anomaly this round
        ///                         OR carriedHolds() is non-empty
        ///                         OR the frontier is incomplete
        ///
        /// The second term is STRUCTURAL, not decorative. Every hold the fold seals also records an
        /// anomaly today, so the first term happens to cover the second — but that is a coincidence of
        /// the current code, not the invariant. The invariant is the HOLD SET: a hold means some
        /// namespace's frontier is unproven, and no proof taken elsewhere can license destruction while
        /// one stands. Reading the seal directly is what keeps a future change to anomaly recording from
        /// silently opening the gate.
        ///
        /// STAGE B'S NARROWING OF THIS GATE TO PER-NAMESPACE MUST CARRY TWO THINGS, NOT ONE. The first
        /// is the hold-set term just described. The second is the namespace whose `_ckpt` is
        /// undecodable AND which offers the walk no position to hold at: it mints no hold on purpose (a
        /// fabricated `offending_position` would become a durable false witness), so its fail-close
        /// rests on `recordAnomaly` plus `frontier_incomplete` and on nothing else -- the only
        /// per-namespace failure in this file whose gate rests entirely on that pair. A per-namespace
        /// gate that carried only the hold set would license destruction against a namespace whose own
        /// checkpoint could not be read.
        bool suppress_destructive = false;

        /// Whether EVERY namespace in this round's universe reached a proven frontier — see
        /// `UniversePolicy` for what the universe is and why Stage A refuses to trust it. False under
        /// `StageA_Suppressed` no matter what the per-namespace probes found.
        bool frontier_complete = false;

        /// The universe's size and how much of it this round proved, for the `fold_ref_intake` row: a
        /// round that suppressed everything owes the reader the two numbers that explain why.
        uint64_t frontier_namespaces = 0;
        uint64_t frontier_proven = 0;
        /// Namespaces the round KNEW about (a cursor in the adopted seal) but did not probe at all,
        /// because the round's frontier-probe budget ran out first. Each one is an unproven frontier.
        uint64_t frontier_unprobed_budget = 0;

        /// Every hold this round SEALED, as `(cursor key, hold)` — both the ones it detected and the
        /// ones it carried from the parent seal because their offending position is still unresolved.
        /// It reads the seal that is about to become durable, so it is the round's final answer rather
        /// than an intermediate one.
        ///
        /// This is the input to the destructive-round suppression rule (`suppress_destructive` =
        /// current anomalies OR every carried hold): a hold means some namespace's frontier is
        /// unproven, and no frontier proof taken this round can license destruction while one stands.
        /// Every hold the fold seals also records an anomaly today, so the two agree; the accessor
        /// exists because that coincidence is not the invariant — the hold set is.
        std::vector<std::pair<String, RefHold>> carriedHolds() const
        {
            std::vector<std::pair<String, RefHold>> out;
            for (const auto & [key, cov] : fold_seal.per_ns_shard)
                if (cov.hold)
                    out.emplace_back(key, *cov.hold);
            return out;
        }
    };

    /// Per changed root shard, stream the one ordered
    /// RootOwnerEvent journal in transition_version order and dispatch each event by comparing
    /// old_binding.manifest_ref to new_binding.manifest_ref:
    ///   - EQUAL (an owner move, e.g. a promote Precommit->Committed at the SAME ref) => NO blob delta,
    ///     NO part-manifest cleanup (the activating PrecommitAdd was folded earlier — see the barrier);
    ///   - TRUE REMOVAL (old present, the ref not owned afterwards) => read the OLD body, emit -1 per
    ///     blob entry + queue the body for cleanup (an old precommit never activated emitted no edges);
    ///   - ACTIVATION (new present) => read the NEW body, emit +1 per blob entry, SUBJECT TO THE FOLD
    ///     BARRIER: do not advance the durable fold cursor past a `RootOwnerEvent` that
    ///     leaves a LIVE precommit binding whose manifest body is not present+valid; re-read each round.
    /// 404 RULE: a body that is PRESENT-but-invalid (ref/namespace mismatch) is genuine corruption =>
    /// CORRUPTED_DATA (hard). A MISSING body (404) is handled by where it appears: a precommit
    /// activation new missing body => no edges + barrier holds the cursor; a committed/promote new
    /// missing body or a true-removal old body missing at removal-fold => fail-closed FOR THAT DECISION
    /// (clamp the shard's last_folded_ref_id below it, record the anomaly, stop folding THIS shard) —
    /// never guess a delta and never wedge the round on a missing body.
    /// On success `state` carries the committed snap_generation and `state_token` the committed gc/state
    /// token. The committed pair is THREADED into retire, never re-read (zombie-steal protection).
    /// Round-paced graduation: `current_round` (= state.round + 1, the SAME basis condemn_round is
    /// stamped at) is the threshold the fold's two-cursor merge graduates/condemns against — an entry
    /// graduates once `condemn_round < current_round`, i.e. it survived at least one full round after
    /// being condemned. The fold no longer CASes gc/state — it sets (snap_generation, snap_attempt)
    /// in-memory; the SINGLE round CAS commits them.
    /// `ref_scan` is the round's ONE enumeration of `cas/refs/` (see `RefScanSummary`); the fold regroups
    /// its keys strictly rather than listing the prefix again.
    FoldResult fold(GcState & state, Token & state_token, RoundReport & report, uint64_t current_round,
                    const RefScanSummary & ref_scan, UniversePolicy policy);

    /// The round's `_ckpt.checkpoint` witness per namespace — the SECOND, hint-independent witness the
    /// walk decides its absents against. ONE call site, in the fold, right where the hint is grouped.
    ///
    /// The listing cannot be the sole witness source: it is a snapshot, so a record that became durable
    /// after the enumeration is invisible to that round's probes, and an absent expected-next then reads
    /// as a frontier when it is really a gap. `_ckpt.checkpoint` is the namespace's own durable tail,
    /// read by the fold anyway for cleanup ranges, and it decides the same question without asking the
    /// listing anything — so it is read by EXACT KEY, never gated on `RefTableListing::has_ckpt`.
    ///
    /// It takes BOTH of the round's namespace sources because it owes a witness to both: `ref_tables`
    /// is the hint, and `parent_cursors` is where a carried hold names a namespace the hint may have
    /// stopped mentioning — precisely the namespace whose witness matters most. An absent `_ckpt`, and a
    /// present one with no `checkpoint_snapshot_id`, both contribute no entry.
    struct CheckpointWitnesses
    {
        /// namespace -> `_ckpt.checkpoint_snapshot_id`, for the namespaces that published one.
        std::map<String, RefTxnId> witnesses;

        /// namespace -> the decode failure's message, for the namespaces whose `_ckpt` is PRESENT and
        /// UNREADABLE. Separate from an absent entry because the two mean opposite things: an absent
        /// entry says "this namespace published no checkpoint", which the walk may treat as no witness,
        /// while an entry here says "this namespace HAS a checkpoint and we cannot read it", which it
        /// may not — an unread witness is not an absent one. Both consumers of `witnesses` read it per
        /// namespace, so the damage is confined to the namespace that owns the object: the walk holds it
        /// (`HoldReason::CheckpointUndecodable`) and folds every other namespace normally.
        std::map<String, String> undecodable;
    };
    CheckpointWitnesses readCheckpointWitnesses(const std::map<String, RefTableListing> & ref_tables,
                                                const std::map<String, ShardCoverage> & parent_cursors);

    /// What ONE generation's prefix says about itself: whether the generation exists at all, and the
    /// greatest attempt under it whose key is one `foldSealKey` would have produced.
    struct GenerationSealProbe
    {
        bool generation_exists = false;
        std::optional<uint64_t> seal_attempt;
    };
    GenerationSealProbe probeGenerationForSeal(uint64_t generation);

    /// The newest fold-seal OBJECT in the pool by `(generation, attempt)`, or absent when the pool has
    /// never sealed a baseline. Used whenever `gc/state` names no adopted baseline: holds live in the
    /// SEAL, not in the pointer to it, so losing the POINTER must not silently produce a hold-free
    /// baseline while an unreadable SEAL refuses.
    ///
    /// The pool-wide enumeration is a HINT, never the answer: one that omitted the true newest seal
    /// would hand back an older one and lose every hold detected since — the same hole one layer up.
    /// Two NARROW single-generation probes above the listing's maximum ask whether it lied, and a seal
    /// found there THROWS a refusal rather than being adopted; a store that misreports its own
    /// enumeration during disaster recovery does not get a second guess.
    ///
    /// That is DETECTION, not proof, and the implementation says so at the site: the generation half of
    /// the question is arithmetic (generations are dense in minting), while the attempt half is an
    /// enumeration within ONE directory, because a seal key's attempt component is a lease sequence
    /// number with no arithmetic successor to point-read.
    ///
    /// THROWS `CORRUPTED_DATA` on that refusal, and on a pool whose enumeration yielded no seal but
    /// which is not provably new. Returning `nullopt` is the VIRGIN verdict — logged at WARNING with
    /// its evidence enumerated and counted by `CasGcRebuildVirginByEnumeration`, because it rests on
    /// enumeration alone and no point read can prove it.
    std::optional<std::pair<uint64_t, uint64_t>> newestFoldSealRef();

    /// Read ONE part manifest named by `id`, validate it, and append sign*(+1) blob deltas for each
    /// blob entry to `deltas`. On sign<0 queue (id -> token) into mf_cleanup. Returns whether a body was
    /// read+validated: false => ABSENT body (404; the caller decides per the 404 rule). A body that is
    /// PRESENT but fails refMatchesBody / manifestNamespaceMatches throws CORRUPTED_DATA.
    /// `txn_ordinal` stamps every delta this call pushes with the round-local ordinal of the ref
    /// transaction that emitted it (probe B2 — see `TxnApplyLedger`).
    bool foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup, uint32_t txn_ordinal);




    /// Ref-object cleanup: delete each table's ref logs
    /// covered by BOTH the durable fold cursor AND a durable snapshot, and snapshots older than the newest
    /// observed one, in batches of <=1000 exact keys. A remove_namespace log is retained until its
    /// namespace-cleanup item is durably Completed; ONCE Completed, that item's `remove_txn_id` is the
    /// covering snapshot of the whole tail and its logs are cleaned in the same round. Runs post-CAS
    /// (durable cursor), after `runNamespaceCleanupPasses` has
    /// made the `Removed` snapshot durable, and only on a clamp-free round (`suppress_destructive == false`).
    /// Acts only on keys THIS round's scan returned (reused via `folded.ref_tables`), so a covering snapshot
    /// -- observed or just-republished -- is always durable before any deletion it authorizes.
    ///
    /// The round's `folded.checkpoints` TIGHTENS both boundaries and can never widen either: a log is
    /// deletable only at or below `min(newest covering snapshot, checkpoint, cursor)`, and a snapshot
    /// only STRICTLY BELOW `min(newest covering snapshot, checkpoint)` -- so the snapshot the checkpoint
    /// itself names always survives. A namespace with no entry (no `_ckpt` yet, or one that has never
    /// carried a `checkpoint_snapshot_id`) degrades to the cursor/snapshot boundaries alone.
    void cleanupRefObjects(const FoldResult & folded, bool suppress_destructive);

    /// Namespace-cleanup item passes: for each item in the committed seal, a Pending item
    /// runs a bounded exact-key enumerate-and-delete pass over the removed namespace's physical `@cas@`
    /// prefixes (manifest bodies + verbatim files); a Completed item publishes the `_cleanup` marker and
    /// republishes the constant-size `Removed` snapshot (both idempotent `putIfAbsent`). Runs post-CAS.
    /// A stale leader (deposed after its round CAS but still executing) must never delete a namespace a
    /// successor round Completed and the writer recreated: the Pending pass re-reads gc/state (aborting
    /// unless `durable.round == new_round` under our lease) and aborts on the `_cleanup` marker's presence
    /// HEAD'd PER KEY -- the exact recreation precondition for both a cold (successor-epoch) and a warm
    /// (same-epoch) recreation. A cheap greater-epoch skip on manifest keys is kept
    /// as defense-in-depth.
    /// `ref_tables` is this round's own global LIST: the `Removed`-snapshot republication is gated on it
    /// so a snapshot a recreated namespace already superseded is never re-created (the per-round churn).
    void runNamespaceCleanupPasses(const CasFoldSeal & seal, const std::map<String, RefTableListing> & ref_tables,
                                   uint64_t new_round, bool suppress_destructive);

    /// Whether a namespace's physical `@cas@` metadata prefixes (manifest bodies + verbatim files) hold no
    /// object -- the Pending->Completed condition for its namespace-cleanup item. LIST-only, no delete.
    bool namespacePhysicallyEmpty(const RootNamespace & ns);

    /// Best-effort cursor-paced orphan part-manifest sweep. This is cleanup-only state: a lost CAS only
    /// discards cursor progress and must not fail the already-completed GC round. Returns the page's
    /// counters so the `orphan_sweep` phase row can report what the pass actually did — including the
    /// §6 premise's per-reason retention classes, which on this path are the only report there is.
    ManifestSweepResult runManifestSweepCursorPass(GcState & state, Token & state_token);

    /// Emit the sweep's per-pass retention rollup, throttled (see `last_retain_rollup`). Separate from
    /// the pass itself so the phase row and the log line read the SAME counters rather than two
    /// independently-derived views of the page.
    void reportSweepRetention(const ManifestSweepResult & result);

    /// Discover the present (namespace, shard) pairs from LIST(cas/refs/); shared by the fold and the
    /// resume re-fence. Returns only shards that have a backend object (absent shards not included).
    /// Fresh pool (no shards yet) => empty result.
    ///
    /// This is a HINT and its mechanism is unchanged: a store that omits a namespace from the
    /// enumeration still omits it here. What changed around it is that the fold UNIONS this result with
    /// the namespaces the adopted seal already carries a cursor for, so a quiet hint can only ever ADD
    /// to the frontier obligation, never shrink it.
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverse();

    /// The two cheap pre-fold GC round-defer signals — both computed from
    /// state already reachable before the fold's snapshot merge (O(retired)/O(shards), no snapshot
    /// read), so `runRegularRound` can decide DEFER-vs-FOLD before paying the fold's cost.
    ///
    /// True iff a graduation is due this round, read ZERO-I/O from the adopted fold seal's per-shard
    /// `condemned_summary` (retired-in-snapshot): `∃ shard: pending_total > 0 ||
    /// oldest_nonpending_condemn_round < current_round`. `snap_generation == 0` (fresh pool) => false.
    /// This is the load-bearing safety signal: it forces a fold before any
    /// destructive decision. FAIL-CLOSED: a missing / undecodable seal, or a summary not TOTAL over
    /// gc_shards, is corrupt GC bookkeeping — returns TRUE (forces a fold so the round's own fail-closed
    /// path surfaces it); a round must never silently defer on corrupt bookkeeping.
    bool graduationDue(const GcState & state, uint64_t current_round);

    /// One full enumeration of `cas/refs/`: the raw keys plus a lenient per-namespace index of the
    /// Log-kind ids among them. A malformed key lands in `keys` and is not indexed; `groupRefKeys` in
    /// the fold does the strict validation and the round-abort. The one exception to that leniency is
    /// a key that IS a ref object but names no life (the un-incarnated shape): `parseRefObjectKey`
    /// refuses it here with `CORRUPTED_DATA`, so the enumeration aborts rather than carrying a key no
    /// catalog entry can ever claim.
    RefScanSummary enumerateRefPrefix();

    /// The round's ONE hint enumeration (`enumerateRefPrefix`) plus the DEFER signal computed from it
    /// (`RefScanSummary::changed_shards` -- the number of tables with at least one ref log above their
    /// sealed durable cursor, compared against the per-table cursors in the adopted fold seal at
    /// `state.snap_generation`/`snap_attempt`).
    RefScanSummary listRefPrefix(const GcState & state);

    /// THE STORE-QUALITY DETECTOR (historically "probe A"). SAMPLED on a deterministic cadence
    /// (`PoolConfig::gc_probe_a_period`): on a due round it performs ONE extra full enumeration of
    /// `cas/refs/` and compares it against the round's own (`round_scan`).
    ///
    ///   an id present in one enumeration, absent from the other, and STRICTLY BELOW the other
    ///   enumeration's maximum id for that same namespace CANNOT be a concurrent append.
    ///
    /// It ABORTS NOTHING AND GATES NOTHING: it records no anomaly (an anomaly would suppress the
    /// round's destructive steps), holds no namespace, and moves no cursor. The signal it carries is
    /// about the STORE, and the fold is immune to it by construction — the intake reads by exact key.
    /// Its whole output is a report: the `ref_list_probe` phase row (`due`/`performed`/`skipped`/
    /// `holes`), `gc_anomaly` audit rows, `CasGcProbeA*` ProfileEvents, and the text log.
    void sampleRefListQuality(const RefScanSummary & round_scan, uint64_t current_round);

    /// (reclaimDroppedShards was removed with the snapshot+log ref model: there is no mutable per-namespace
    /// shard object to tombstone+reclaim; physical namespace reclamation is the namespace-cleanup item.)


    /// Attempt-scoped generation retention. The sole reclaimer — bounded per round and fail-open on a benign
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
    ///
    /// `suppress_destructive` is the round's gate: a suppressed round prunes NOTHING and leaves
    /// `snap_pruned_through` where it was. The cursor must not advance either -- it is a monotone
    /// high-water mark that the wholesale prune never revisits, so advancing it over a generation this
    /// round declined to delete would strand that generation's whole prefix permanently.
    void pruneSupersededGenerations(uint64_t adopted_generation, uint64_t attempt, GcState & next,
                                    const std::set<uint64_t> & referenced_generations,
                                    bool suppress_destructive);

    /// Read the fold seal for (generation, attempt) (nullopt when absent). Used by resume + parent-cursor reads.
    std::optional<CasFoldSeal> readFoldSeal(uint64_t generation, uint64_t attempt);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    /// Submit one per-hash freshness-meta op (condemn/spare/delete) to the bounded `meta_pool`.
    /// NEVER throws: `job` is wrapped in its own try/catch (an exception counts into the
    /// `CasGcMetaWriteAnomaly` profile event + a log line); if scheduling itself fails (e.g. resource
    /// exhaustion) the op runs inline rather than being silently lost. Callers must capture every value
    /// `job` touches BY VALUE (never by reference to a loop-local like the fold's `cur_blob`, which
    /// mutates across iterations while this job may still be queued).
    void scheduleMetaJob(std::function<void()> job);

    /// Schedule the async per-hash condemn-marker write for a (ref, token) entering or being carried in
    /// the retired set; when `writeCondemnedMeta` reports durable Condemned evidence, the in-process
    /// confirmation for the exact (ref, token) is recorded (the graduation gate's fast path). A swallowed
    /// write records nothing — the entry stays unconfirmed and graduation carries it (triage §3.4).
    void scheduleCondemnMarkerWrite(const BlobRef & ref, const Token & token,
                                    uint64_t condemn_round, uint64_t size);

    /// The in-process condemn-marker confirmation registry (see `condemn_markers_confirmed`).
    void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
    void forgetCondemnMarker(const BlobRef & ref, const Token & token);

    PoolPtr store;
    /// Where `GcPhaseTimer` sends one record per GC phase. Empty unless a `CasGcScheduler` installed one
    /// for the current round, in which case every phase of that round emits a row.
    GcPhaseSink phase_sink;
    UInt128 gc_id{};   /// this leader's identity (random u128, never 0)
    /// Every round-engine log line goes through this logger (see the constructor's `log_` doc
    /// comment) so multi-disk processes can attribute GC logs to the disk/srid that produced them.
    LoggerPtr logger;
    uint64_t rebuild_edge_budget_override = 0;   /// tests force tiny batches
    std::function<uint64_t()> now_ms_fn;   /// wall-clock ms; injected (tests), defaults to system_clock
    /// The heartbeat gate's own observation clock —
    /// monotonic on this process, injected (tests), defaults to `Pool::bootMs()`. Never compared
    /// against another node's clock; see `computeHeartbeatFloor`.
    std::function<uint64_t()> mono_ms_fn;
    bool trim_enabled = true;     /// TEST SEAM ONLY: production always trims; see setTrimEnabledForTest
    /// Leader-local, in-memory count of consecutive deferred
    /// rounds since the last FOLD. NOT persisted (a fresh/stolen leader starts at 0 -- conservative:
    /// it may fold one round sooner than a long-lived leader would, never later). Reset to 0 whenever
    /// a round folds; incremented on every DEFER. Bounds batching via `gc_fold_max_defer_rounds`.
    uint64_t rounds_since_last_fold_ = 0;

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
    /// Heartbeat pair observed alongside the lease (gates the steal): a steal requires the lease
    /// tuple AND this (owner, hb_seq) pair to be frozen across a full window. `hb_seq` is compared
    /// only when the remembered hb owner matches; an owner change counts as movement (alive).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;

    /// The heartbeat gate's cross-round, per-srid
    /// write-token observation (`computeHeartbeatFloor`'s `obs` argument). In-memory only — a new
    /// leader (after a steal, or a process restart) starts empty, which only delays fencing an
    /// already-dead mount by one extra round (safe: never fences early).
    MountObservationMap mount_obs;

    /// Throttle for the orphan sweep's retention rollup. In Stage A the premise retains on nearly every
    /// pass, so an unconditional `LOG_INFO` would emit the same sentence every round forever and train
    /// operators to filter out the channel carrying the answer they need. It is reported when the
    /// verdict CHANGES (a different top reason class, or a different count), and otherwise re-stated
    /// every `kRetainRollupRepeatPasses` passes so a newly-arrived operator is never left with silence
    /// they would have to read as "nothing to report". Leader-owned and in-memory: a fresh leader
    /// re-states once, which is the harmless direction.
    static constexpr uint64_t kRetainRollupRepeatPasses = 64;
    std::optional<std::pair<SweepRetainClass, uint64_t>> last_retain_rollup;
    uint64_t retain_rollup_passes_since_report = 0;

    /// Bounded pool for the round's per-hash freshness-meta writes (condemn/spare/delete);
    /// sized from `PoolConfig::gc_meta_pool_size` (constructed in the ctor, after the null/id checks --
    /// never touches a possibly-null `store` at member-init time). A `unique_ptr` (not a plain member)
    /// so construction can happen in the ctor body, after validating `store`.
    std::unique_ptr<ThreadPool> meta_pool;

    /// Cumulative counts of the per-hash freshness-meta jobs this leader handed to `meta_pool` and of
    /// those that finished. The `meta_pool_wait` phase reports the ROUND's deltas: that phase's work runs
    /// on other threads, so its `ProfileEvents` delta is empty BY CONSTRUCTION, and these two numbers are
    /// what distinguish "the queue was deep" from "the endpoint was slow" when read next to its duration.
    /// Atomic because a pool thread increments `meta_jobs_completed_` while the round thread reads it.
    std::atomic<uint64_t> meta_jobs_scheduled_{0};
    std::atomic<uint64_t> meta_jobs_completed_{0};

    /// In-process confirmations of durable condemn-marker writes, keyed (blob, exact incarnation-token
    /// value): inserted by `scheduleCondemnMarkerWrite`'s completion (and the rebuild's synchronous
    /// marker publish) when `writeCondemnedMeta` reports durable Condemned evidence; consulted by the
    /// graduation gate (the delete-authorizing edge, triage §3.4); pruned when the entry settles
    /// (redelete / spare / supersede). In-memory only — after a restart or leader change the graduation
    /// gate falls back to a synchronous `loadMeta` re-check (the marker itself is durable). Guarded by
    /// `condemn_marker_mutex`: meta-pool completions insert concurrently with the fold thread's reads.
    std::mutex condemn_marker_mutex;
    std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;

    /// Probe B1's two numbers for the round: the ref-log POSITIONS the sealed coverage declares covered
    /// (counted arithmetically over each namespace's cut -- not by listed ids, which under arithmetic
    /// intake say nothing about what was applied), and the ref logs that actually folded. They are EQUAL
    /// on every committed round (the fold throws otherwise) -- carrying them is what turns "they are
    /// always equal" from an assumption into an observable property once the per-phase GC row emitter
    /// reports them on the `fold_ref_intake` row.
    /// Both stay 0 on a ref-folding abort, where the identity does not apply.
    uint64_t logs_accounted_this_round = 0;
    uint64_t logs_applied_this_round = 0;

    /// Probe B2's verdict for the round: ref transactions the round folded and merged but whose blob
    /// deltas never reached a shard reducer. 0 on every COMMITTED round -- a nonzero value is
    /// accompanied by the fold's fail-closed throw, so the value only ever exists as the forensic
    /// record of a round that then failed.
    uint64_t txns_unapplied_this_round = 0;

public:
    /// TEST SEAM: expose LIST-based namespace/shard discovery so unit tests can assert the
    /// discovered universe equals the set of present ref shards without driving a full round.
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverseForTest()
    {
        return discoverUniverse();
    }

    /// TEST SEAM: expose the two cheap pre-fold GC round-defer signals so unit tests can
    /// assert them directly without driving a full round.
    bool graduationDueForTest(const GcState & state, uint64_t current_round)
    {
        return graduationDue(state, current_round);
    }

    /// Test-only access to the round's ref-prefix enumeration (its `changed_shards` is the defer signal).
    RefScanSummary listRefPrefixForTest(const GcState & state)
    {
        return listRefPrefix(state);
    }

    /// TEST SEAM: drive one namespace-cleanup pass directly so a test can
    /// stage the exact stale-leader interleaving (a Pending item whose namespace a successor Completed +
    /// recreated) without racing a live round. Production drives this only through `runRegularRound`.
    void runNamespaceCleanupPassesForTest(const CasFoldSeal & seal,
                                          const std::map<String, RefTableListing> & ref_tables,
                                          uint64_t new_round, bool suppress_destructive)
    {
        runNamespaceCleanupPasses(seal, ref_tables, new_round, suppress_destructive);
    }

};

}
