#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <map>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// The logical (GC-bookkeeping) size of a retired object: blobs subtract the pool's fixed
/// blob_header_len (a blob OBJECT smaller than the fixed header is corrupt — CORRUPTED_DATA,
/// fail closed, never a wrapped-around size); trees/packs account whole-object. Sizes feed
/// cost/health accounting only — no protocol decision ever reads them.
uint64_t retiredLogicalSize(ObjectKind kind, uint64_t object_size, uint64_t blob_header_len);

/// What one runRegularRound did (counters are health metrics, not protocol state).
struct RoundReport
{
    bool acquired_lease = false;  /// false => another leader is alive; nothing else was done
    uint64_t round = 0;
    uint64_t candidates = 0;      /// retired entries WRITTEN this round (absent candidates are skipped)
    uint64_t deleted = 0;
    uint64_t absent = 0;
    uint64_t replaced = 0;        /// 412-saves - a health metric (spec §7)
    uint64_t spared = 0;
    uint64_t cascaded = 0;        /// children freed by the cascade this round
    uint64_t forgotten_on_delete = 0;  /// P9: nodes pruned from `known` because GC deleted them (cascade)
    uint64_t forgotten_absent = 0;     /// P9: nodes pruned because a retire HEAD found them already gone (404)
};

/// Leader-paced regular GC (spec §7): fold -> retire -> fence -> recheck -> exact-token delete ->
/// cascade -> trim. The lease is WORK DEDUP ONLY - every step is idempotent and split-brain-safe
/// (monotone gc/state, append-by-unique-path retire/outcome logs, exact-token deletes); the TLA+
/// model (CaIncarnationCore.tla) proves the round safe with NO leadership assumption at all. A
/// stale leader can only duplicate work, never roll back state or mis-delete.
///
/// LEASE / STEAL WINDOW (deterministic - this class NEVER reads a clock). The lease lives inside
/// gc/state as {owner, seq} and moves only by CAS on the whole gc/state object. Each Gc instance
/// remembers the last (owner, seq) it OBSERVED on gc/state. On runRegularRound:
///
///   1. Read gc/state. Absent => fresh GcState with our lease = {gc_id, 1}, create-if-absent CAS.
///      Committed => we lead. Conflict => a racer created it first => re-read, fall through.
///   2. state.lease.owner == gc_id => RENEW: lease.seq++, CAS against the observed token.
///      Committed => we lead. Conflict => someone moved the lease (a steal happened) => re-read
///      once; if the owner is still us, retry the renew once; else not-acquired.
///   3. Foreign owner whose (owner, seq) DIFFERS from our remembered observation (or we have no
///      prior observation) => the incumbent is ALIVE (it renewed since our last look) => record
///      the new observation, return not-acquired (back off).
///   4. Foreign owner whose (owner, seq) is IDENTICAL to our remembered observation => the
///      incumbent did NOT renew across our full observation window - one whole prior
///      runRegularRound attempt of OURS, "the contender's own waiting window" (spec §8) => STEAL:
///      lease.owner = gc_id; lease.seq++; fence_seq++ - a new leadership epoch, so the new
///      leader's retire/outcome paths (<round>.<fence_seq>) never collide with the old leader's
///      (spec §4 append-by-unique-path). CAS against the observed token. Committed => we lead.
///      Conflict => a racer stole first => re-read, record the observation, not-acquired.
///
/// A lost CAS NEVER yields leadership within the same attempt beyond the single bounded renew
/// retry above (max 2 CAS attempts per call) - a contender that loses a steal race backs off and
/// re-enters the observation protocol from the freshly read state.
///
/// PRODUCTION PACING: the wiring's GC scheduler thread calls runRegularRound on a timer and
/// sleeps between calls. "Observed non-renewal across the contender's window" therefore means
/// the incumbent failed to renew for one full scheduler period - wall-clock enters ONLY through
/// the caller's pacing, never through this class (so unit tests drive the window by simply
/// calling runRegularRound, with no sleeps and no clock).
///
/// NOT thread-safe: one pacing thread drives a Gc instance. gc_id uniqueness across instances
/// (a random u128) is a CALLER obligation - duplicate ids make two leaders indistinguishable.
class Gc
{
public:
    Gc(StorePtr store_, UInt128 gc_id_);

    /// One full round. Returns acquired_lease=false (nothing else done) if another leader is alive.
    RoundReport runRegularRound();

    /// B160 advisory heartbeat: bump <prefix>/gc/hb to {gc_id, hb_seq+1}. Best-effort (a lost CAS is
    /// harmless — the next pulse retries). Touches NO Gc instance state, so the scheduler's separate
    /// heartbeat thread may call it concurrently with the round thread. Static by design.
    static void pulseHeartbeat(Store & store, UInt128 gc_id);

    /// One previewed deletion the next regular round would make, with the reason it is eligible.
    struct PreviewEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        UInt128 hash{};
        String key;
        uint64_t size = 0;
        String reason;   /// "unreachable" (zero in-degree, present)
    };

    /// WRITE-FREE preview of the next round's deletes, derived from the DURABLE gc/snap + gc/state.
    /// Diagnostic / cross-check ONLY — its output must never feed a real delete. It reads the durable
    /// snap WITHOUT folding new journal records, so at NON-QUIESCENCE it can OVER-REPORT a node that a
    /// since-landed publish re-referenced (the real round folds first and would spare it). The
    /// {preview} ⊆ {genuinely-unreachable} guarantee therefore holds ONLY at quiescence (no journal
    /// records past the folded cursor); run it then for an exact picture. No CAS/delete on any path.
    std::vector<PreviewEntry> previewDeletes();

    /// B167 per-server build watermark (spec 2026-06-16-ca-build-watermark): start a fresh GC round's
    /// watermark observation. Clears the PER-ROUND caches (watermark_cache, server_live_this_round);
    /// the ACROSS-ROUND maps (last_seen_server_seq, server_frozen_rounds) persist — they are the K=2
    /// frozen-seq crash detector's memory. Call once before the retire observe loop.
    void beginWatermarkRound();

    /// The condemn guard's co-liveness clause: is `meta`'s owning build still in-flight? Reads the
    /// owner triple ("cas_owner" = "<server_hex>:<epoch>:<build_seq>") and the owning server's
    /// watermark (cached per round). Protected IFF the server is live this round, the epoch matches,
    /// build_seq >= min_active, and the server is not retired. Absent/malformed owner or absent
    /// watermark => false (unprotected — the pre-watermark default, fail-safe-to-delete is the
    /// publish gate's job, not this guard's).
    bool protectedByLiveBuild(const ObjectMeta & meta);

    /// full-GC walk + debris reclaim: deferred (M-F); API slot reserved.
    ///
    /// DELETE-SITE AUDIT (closeout invariant; grep `deleteExact` under Core/): the recheck holds
    /// the ONLY content (reachability) delete in the whole core, behind the four INV-NO-LOSS
    /// gates. Every other deleteExact caller removes a non-content object it owns: the recheck's
    /// retired-set drop and the resume path (GC metadata), HeartbeatKeeper discard (its own
    /// heartbeat), dropNamespace (verbatim files, never content-addressed), and the capability
    /// probe (throwaway probe keys). Adding a second content-delete site is a protocol defect.

private:
    /// Lease acquire/renew/steal per the documented observation protocol. On success `state` holds
    /// the committed gc/state (with our lease) and `state_token` its backend token.
    bool acquireOrRenewLease(GcState & state, Token & state_token);

    /// What one R1 fold produced (all derived from durable state; `transitioned` is the only
    /// in-memory-transition artifact and it is REPORT/CROSS-CHECK ONLY - retire (Task 7) derives
    /// its candidates STATELESSLY from the durable snap via GcSnap::zeroInDegreeKnown, so a
    /// crash-replayed round sees the same candidates as the round that folded).
    struct FoldResult
    {
        std::map<uint64_t, GcSnap> snap;                              /// snap_shard -> loaded+updated shard
        std::vector<Candidate> transitioned;                          /// nodes that zeroed during THIS fold
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;  /// discovered present manifests
    };

    /// R1 (spec §7; the model's GFold): per root shard, stream-merge the journal records in
    /// (folded_cursor, shard_version] into the snap shards. Add => last-op-wins root edge (the
    /// displaced old target is collected into `transitioned` - the Task-3 republish carryover) +
    /// once-per-tree expansion (read the tree ONCE, add its child edges into each CHILD's snap
    /// shard, set the marker in the TREE's home shard); Remove => drop the root edge only (the
    /// cascade strip belongs to the delete pipeline, Task 10). Fresh uploads are invisible by
    /// construction (no journal record until publish).
    ///
    /// DURABLE-BEFORE-CURSOR: the updated snap goes durable FIRST (ALL shards, even unchanged
    /// ones - simplest correct v1; skipping byte-identical shards is a possible later
    /// optimization), only then does ONE gc/state CAS advance snap_generation against `state_token`.
    /// Generation objects are write-once (putIfAbsent) and the write generation PROBES UPWARD from
    /// snap_generation+1: per generation, every shard Done or byte-equal (our own crash-replay) =>
    /// adopt; any shard divergent => abandon that generation and try one higher - a FIXED generation
    /// would wedge GC forever once an orphan plus new journal records make the bytes unmatchable.
    /// Abandoned partials are harmless orphans (full-GC cleans them in M-F); generations need not
    /// be dense - the gc/state pointer is authoritative. A gc/state CAS Conflict means another
    /// leader advanced state - throw ABORTED (retry next round). snap_shards != 1 is refused
    /// (NOT_IMPLEMENTED): cross-shard last-op-wins displacement is undesigned in M-C3.
    /// NOTE: folded_cursor is NOT advanced by fold (B140-dangle fix): it now lives in the snap
    /// (GcSnap::folded_cursor) and is advanced by cascadeAndPersist together with the edges.
    ///
    /// On success `state` carries the committed snap_generation and `state_token` the committed
    /// gc/state token. The committed pair is THREADED into retire, never re-read: a post-fold
    /// re-read would open a zombie window - a lease steal landing between the fold CAS and the
    /// re-read hands this (now stale) leader the thief's state with its bumped fence_seq, letting
    /// stale-snap retire sets land inside the thief's epoch paths.
    FoldResult fold(GcState & state, Token & state_token);

    /// R2 (spec §7; the model's GRetire): the round being executed is state.round + 1 (state.round
    /// = "highest round whose retire sets are durable"). Per candidate — derived STATELESSLY from
    /// the durable snap via GcSnap::zeroInDegreeKnown, the model guard `present ∧ everEdged ∧
    /// InDeg = 0` — ONE HEAD observes the object's CURRENT token; an absent object is SKIPPED
    /// (no token to condemn — never fabricate; a crashed prior round's landed delete or debris).
    /// The per-shard retire sets are written by unique path (gc/retired/<round>.<fence_seq>/<shard>,
    /// putIfAbsent; ANY decodable occupant at our path is OUR OWN crashed prior attempt — adopted,
    /// write-once preserved; an undecodable occupant is ABORTED). The sets go durable BEFORE one gc/state CAS
    /// advances .round — the durable "retire phase complete" marker (INV-MONOTONE-GC ordering:
    /// a writer whose RetireView refreshes at the new round is guaranteed to see the entries).
    /// On success `state`/`state_token` carry the committed round. Returns the retired entries
    /// grouped by snap shard (the input to R4 recheck). Retired ≠ dead: the entries are the
    /// writer-facing "resurrect, don't reuse" barrier.
    std::map<uint64_t, RetiredSet> retire(GcState & state, Token & state_token,
                                          std::map<uint64_t, GcSnap> & snap, RoundReport & report);

    /// R3 (spec §7 as amended 2026-06-12; the model's GFenceShard): CAS the NAMESPACE REGISTRY's
    /// fence_round first (the ordering point for namespace creation — W-REGISTER's gate floor),
    /// then CAS fence_round := round (monotone max) into EVERY root shard of every namespace in
    /// the FENCE-TIME registry — the registry decoded in the COMMITTED registry-fence attempt,
    /// never the fold-time universe (a namespace registered between the fold's registry read and
    /// the registry-fence CAS would otherwise fall between the two horns: below the registry
    /// fence, so its writer observes no floor, yet absent from the fold-time universe, so its
    /// shards are never fenced or rechecked — a dangle window). Shards are fenced present or
    /// ABSENT via the verified mutateShard loop (the create-if-absent CAS MINTS a fence-only
    /// manifest for an absent shard; the create race against a first publish into that shard is
    /// the required total order). Record each shard's committed shard_version (and the registry's
    /// committed version under the reserved "_registry" key) in gc/state.fence_version[round] —
    /// the durable fence positions the recheck folds through (provable coverage; the model's
    /// fencePos[s]). The manifest CAS totally orders the fence against publishes on that shard —
    /// exactly the ordering the spec's no-return argument rests on. One fence covers the whole
    /// round's candidate set; one gc/state CAS persists the whole vector. On success
    /// `state`/`state_token` carry the committed fence_version[round].
    void fence(GcState & state, Token & state_token);

    /// What one R4 recheck decided. `deleted_trees` are the trees whose entries confirmed Deleted
    /// or Absent-while-held (our own crashed delete provably landed) — the cascade's input
    /// (Task 10). Derived from the FINAL (written-or-adopted) outcome logs, never from this
    /// attempt's in-memory tallies, so a crash-replayed recheck cascades the same trees.
    struct RecheckResult
    {
        std::map<uint64_t, OutcomeLog> outcomes;   /// snap_shard -> the durable outcome log
        std::vector<UInt128> deleted_trees;
        std::vector<Candidate> deleted_nodes;      /// P9: EVERY confirmed-gone node (trees AND
                                                   /// blobs/packs); the cascade forgets each from `known`.
        bool fence_window_records_folded = false;  /// the recheck's fold-through-fence saw records
    };

    /// R4 (spec §7; the model's GRecheckDelete + the synchronous half of Land): re-fold every
    /// fenced shard's journal THROUGH its recorded fence version (records in
    /// (folded_cursor, fence_version] — exactly the publishes that raced the fence; provable
    /// coverage, the FoldedThroughFence guard) into the in-memory snap, then per retired entry:
    ///   in-degree > 0      => outcome Spared (a pre-fence publish re-pinned it — horn 1);
    ///   else deleteExact   => THE SINGLE CONTENT-DELETE SITE (exact observed token):
    ///       Deleted        => outcome Deleted (trees: cascade input);
    ///       NotFound       => outcome Absent (a prior crashed delete landed; held trees cascade);
    ///       TokenMismatch  => outcome Replaced (a resurrection won — the 412-save health metric).
    /// deleteExact is synchronous: its return IS the model's Land (the confirmed outcome). The
    /// outcome logs are written here (append-by-unique-path with crashed-attempt adoption like
    /// retire — an occupant log is the durable truth of what a prior attempt's deletes DID and
    /// replaces this attempt's tallies); the entry drop / strip / persist tail belongs to
    /// cascadeAndPersist.
    RecheckResult recheck(const GcState & state, std::map<uint64_t, GcSnap> & snap,
                          const std::map<uint64_t, RetiredSet> & retired, RoundReport & report);

    /// The CASCADE pipeline step (spec §7; the model's Land does the strip atomically with the
    /// landing): strip every confirmed-deleted (or absent-while-held) tree's child edges + clear
    /// its marker; PERSIST the post-strip snap (including the recheck's fence-window fold) at a
    /// probe-upward generation — writing the fence versions into the snap's folded_cursor (the
    /// B140-dangle fix: (edges, cursor) are one write-once unit, cursor lives in snap shard 0);
    /// ONE gc/state CAS advances snap_generation and erases fence_version[<= round] — the ordering
    /// that closes the cascade-vs-recreate race (a recreate lands ABOVE the fence version, so the
    /// next round folds its Add on a snap whose marker the strip already cleared ⇒ re-expansion
    /// re-pins the children); THEN drop the round's retired-set objects on their confirmed outcomes.
    /// The freed children become zero-in-degree known nodes — the NEXT round's stateless candidate
    /// scan retires and deletes them (LIVE-RECLAIM: ~2 regular rounds).
    void cascadeAndPersist(GcState & state, Token & state_token, std::map<uint64_t, GcSnap> & snap,
                           const RecheckResult & rechecked,
                           const std::map<uint64_t, RetiredSet> & retired, RoundReport & report);

    /// Discover the namespace universe from the registry (namespaces x ALL root_shards shards);
    /// shared by the fold and the resume's re-fence. Absent registry => empty (fresh pool).
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverse();

    /// B171: the shard numbers GC must visit for a namespace. A normal (table) namespace uses the
    /// fixed shard fan-out [0, root_shards) — the spec's static shard model. A BUILD-ROOT namespace
    /// (`_builds/<server>`) has ONE shard per in-flight build keyed by the per-process monotone
    /// `build_seq`, which has no relation to root_shards and routinely exceeds it; its present shards
    /// are discovered by LISTing the namespace prefix and parsing the numeric tails. This is the only
    /// place the build-root namespace deviates from the ordinary shard machinery (it is identical
    /// key-wise; `tryParseRootShardKey` classifies `_builds/<server>/<seq>` correctly).
    std::vector<uint64_t> shardsToVisit(const RootNamespace & ns);

    /// Load the durable snap generation (absent shard objects => empty snaps; the fresh-pool case).
    std::map<uint64_t, GcSnap> loadSnap(const GcState & state);

    /// CRASH-RESUME (the model: "replay of a crashed round re-runs both steps from the durable
    /// outcome log over set semantics - no-ops"). An INCOMPLETE round is detectable from durable
    /// state alone: retired sets still present at (state.round, fence_seq) - they drop only at the
    /// very end of a completed round. The resume re-runs the round TAIL from what is durable:
    ///   - fence_version[state.round] missing => the crash hit between retire and the fence's
    ///     gc/state CAS (or after the cascade erased it but before the sets dropped) => RE-FENCE -
    ///     monotone max re-application; the re-recorded (higher) versions are MORE conservative
    ///     coverage, and re-folding the larger window can only spare more;
    ///   - recheck: deletes are exact-token idempotent (NotFound => Absent), and an existing
    ///     outcome log is ADOPTED as the durable truth of what the crashed attempt's deletes DID;
    ///   - cascade: strips from the FINAL logs over set semantics (no-ops when already applied);
    ///   - the sets drop, the round completes, the report carries the resumed round.
    /// Returns true if a resume ran (the caller returns its report; the next call runs round+1).
    /// Lingering sets from an OLDER LEADERSHIP EPOCH (a thief never probes the old fence_seq) stay
    /// until full GC (M-F) - conservative: they only condemn, forcing resurrects, never deleting.
    bool tryResumeIncompleteRound(GcState & state, Token & state_token, RoundReport & report);

    /// Journal trim (INV-JOURNAL-COVERAGE): drop journal records with at_version <= the DURABLE
    /// folded_cursor (the cascade CAS advanced it into the snap before this runs). The cursor is
    /// read from the snap (the single source of truth since B140-dangle fix). A manifest CAS may
    /// trim records ONLY after the corresponding cursor advance is durable - "compact the manifest
    /// for size" is never a reason to trim. Shards with nothing to trim are not touched (no
    /// pointless version bumps). The trim's own CAS bumps shard_version with no journal record -
    /// vacuously covered, same argument as the fence bump.
    void trim(const std::map<uint64_t, GcSnap> & snap,
              const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards,
              uint64_t round);

    /// Fold one root shard's journal records with at_version in (lo, hi] into `snap` — the shared
    /// R1/R4 record semantics: last-op-wins root edges, once-per-tree expansion with the
    /// displaced-later lookahead, Remove drops the root edge. Returns the nodes that transitioned
    /// to zero in-degree (report/cross-check only; decisions read the snap statelessly).
    std::vector<Candidate> foldShardRecords(std::map<uint64_t, GcSnap> & snap, const GcState & state,
                                            const RootNamespace & ns, const String & cursor_key,
                                            const RootShard & root,
                                            uint64_t lo_exclusive, uint64_t hi_inclusive);

    /// B140-dangle FAIL-CLOSED coherence guard. Run at round start, AFTER the fold produces
    /// (snap, root_shards) and BEFORE any retire/delete. For every root shard whose committed
    /// folded_cursor (read from snap shard 0, the single source of truth) is non-zero, every
    /// latest-per-ref journal `Add` at or below that cursor MUST have its tree expanded into the
    /// committed snap (`isKnown(Tree, ...)`). If a folded, still-live `Add`'s tree is ABSENT from
    /// the snap, the cursor ran ahead of the edges the snap records (the cursor-skip under-count
    /// that lost the live `T_cur -> B` edge): refuse the round (CORRUPTED_DATA) rather than let a
    /// stale in-degree drive a wrong delete. In normal operation (edges, cursor) are one write-once
    /// unit and this can never fire; it is the last line of defence against an injected/out-of-model
    /// incoherent durable state.
    void assertSnapJournalCoherent(const std::map<uint64_t, GcSnap> & snap,
                                   const std::vector<std::pair<RootNamespace, uint64_t>> & root_shards);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    /// B167: fetch the owning server's watermark for THIS round (cached). On the first fetch of a
    /// server in a round, this also computes that server's liveness verdict ONCE (frozen-seq K=2)
    /// and stores it in server_live_this_round. Returns nullptr if the server has no watermark
    /// object (an unrecognized owner — unprotected).
    const ServerWatermark * watermarkOf(UInt128 server_id);

    StorePtr store;
    UInt128 gc_id{};              /// this leader's identity (random u128, never 0)

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
    /// B160: the heartbeat observed alongside the lease (gates the steal — a frozen lease.seq with an
    /// ADVANCING heartbeat means the incumbent is alive mid-round, so do not steal).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;

    /// B167 per-server build watermark caches. The PER-ROUND maps are cleared by beginWatermarkRound;
    /// the ACROSS-ROUND maps persist (the K=2 frozen-seq crash detector's memory).
    std::map<UInt128, ServerWatermark> watermark_cache;   /// per-round: server_id -> its watermark
    std::map<UInt128, bool> server_live_this_round;       /// per-round: liveness verdict, computed once
    std::map<UInt128, uint64_t> last_seen_server_seq;     /// across rounds: last seq observed per server
    std::map<UInt128, uint64_t> server_frozen_rounds;     /// across rounds: consecutive rounds seq unchanged

    /// Pillar A1 resident-snap read-cache. The Gc instance is long-lived (one per scheduler thread,
    /// CasGcScheduler::loop), so this survives across rounds. A snap generation is write-once
    /// (putIfAbsent + byte-equal adoption), so a matching generation guarantees identical bytes —
    /// reusing the resident copy when resident_generation == state.snap_generation needs NO HEAD/GET
    /// and is self-validating: if another leader advanced the generation while we did not hold the
    /// lease, the next state read shows a different snap_generation => mismatch => reload.
    std::optional<std::map<uint64_t, GcSnap>> resident_snap;
    uint64_t resident_generation = 0;
};

}
