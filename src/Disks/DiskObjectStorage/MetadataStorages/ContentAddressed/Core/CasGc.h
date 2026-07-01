#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcOutcomes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <map>
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
struct RoundReport
{
    bool acquired_lease = false;  /// false => another leader is alive; nothing else was done
    uint64_t round = 0;
    uint64_t candidates = 0;      /// retired entries WRITTEN this round (absent candidates are skipped)
    uint64_t deleted = 0;
    uint64_t absent = 0;
    uint64_t replaced = 0;        /// 412-saves — a health metric
    uint64_t spared = 0;
    uint64_t manifests_deleted = 0;  /// owner-removed manifest bodies deleted (B11 — distinct from blob deletes)
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

/// Leader-paced regular GC (spec rev. 15 §Round Protocol): fold -> retire -> fence -> recheck ->
/// exact-token delete -> trim, over the root-local part-manifest model. The lease is WORK DEDUP ONLY —
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
    Gc(StorePtr store_, UInt128 gc_id_);

    /// One full round. Returns acquired_lease=false (nothing else done) if another leader is alive.
    RoundReport runRegularRound();

    /// B160 advisory heartbeat: bump <prefix>/gc/hb to {gc_id, hb_seq+1}. Best-effort (a lost CAS is
    /// harmless — the next pulse retries). Touches NO Gc instance state. Static by design.
    static void pulseHeartbeat(Store & store, UInt128 gc_id);

    /// Phase-2 token-diff: whether the next round's discover step should skip the body read for a
    /// root shard (the listed token equals the persisted folded token, so durable state covers it)
    /// or must read and re-fold the body (token advanced, missing, or no prior coverage).
    enum class DiscoverDecision : uint8_t
    {
        Skip = 0,   /// listed token == sealed folded_token => body already covered; skip the GET
        Read = 1,   /// token advanced / missing / no prior coverage / !supportsListTokens => must read
    };

    /// WRITE-FREE TEST SEAM: the per-shard discover decisions the NEXT round would make, derived from
    /// the durable sealed `folded_token` (recorded by `recheck` from the POST-FENCE shard token) plus a
    /// single LIST sweep over the roots prefix. Returns a map keyed by "ns/shard" (the same format as
    /// `CasFoldSeal::per_ns_shard`). The universe is always the REGISTRY universe — LIST cannot shrink it
    /// (registry authority). No CAS, no delete, no fold. Mirrors the write-free contract of `previewDeletes`.
    std::map<String, DiscoverDecision> discoverDecisionsForTest();

    /// One previewed deletion the next regular round would make, with the reason it is eligible.
    struct PreviewEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        UInt128 hash{};
        String key;
        uint64_t size = 0;
        String reason;   /// "unreachable" (zero in-degree, present)
    };

    /// WRITE-FREE preview of the next round's deletes, derived from the DURABLE sealed in-degree
    /// generation + gc/state. Diagnostic / cross-check ONLY — its output must never feed a real delete.
    /// It reads the durable generation WITHOUT folding new owner events, so at NON-QUIESCENCE it can
    /// OVER-REPORT a blob a since-landed publish re-referenced (the real round folds first and spares
    /// it). The {preview} ⊆ {genuinely-unreachable} guarantee holds ONLY at quiescence. No CAS/delete.
    std::vector<PreviewEntry> previewDeletes();

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

    /// B8 precommit reclaim (converged-shard model): while folding ANY table shard, drop the precommit
    /// bindings of ABANDONED builds (the owning build is no longer live). `precommitAdd` writes a precommit
    /// owner binding into the FUTURE COMMITTED REF's OWN table shard (keyed by `final_ref_name`, kind
    /// `OwnerKind::Precommit`) — there is no `_precommits` namespace. The reclaim therefore enumerates the
    /// LIVE precommit bindings from the shard's folded owner state (the journal owner-state replay), and for
    /// each derives `(server, build_seq)` from the binding's `manifest_ref`
    /// (`writer_epoch`, `build_sequence`). A build is DEAD iff the server has
    /// no watermark, is judged not-live this round (K=2 frozen-seq crash detector), or
    /// `build_sequence < min_active`. Each dead binding is removed by appending a `PrecommitRemove`
    /// RootOwnerEvent (old = the precommit binding, new = none — the SAME encoding `Build::abandon` /
    /// `Store::dropRef` / the legacy reclaim use) in ONE CAS on the shard already in hand. CONSERVATIVE: a
    /// live build is never reclaimed; a wrongful reclaim is caught by the promote guard (fail closed).
    void reclaimAbandonedPrecommit(const RootNamespace & ns, uint64_t shard, uint64_t round);

    /// DELETE-SITE AUDIT (closeout invariant; grep `deleteExact` under Core/): the recheck holds the
    /// ONLY content (blob reachability) delete in the whole core, behind the four INV-NO-LOSS gates.
    /// The recheck's manifest-body exact-token delete (after its decrements are sealed), the retired-set
    /// drop, the resume path (GC metadata), dropNamespace (verbatim files), and the capability probe
    /// (throwaway keys) remove non-content objects they own. Adding a second content-delete site is a
    /// protocol defect.

private:
    /// Lease acquire/renew/steal per the documented observation protocol. On success `state` holds the
    /// committed gc/state (with our lease) and `state_token` its backend token.
    bool acquireOrRenewLease(GcState & state, Token & state_token);

    /// What one R1 fold produced (spec rev. 15 §Fold Owner Transitions). The blob deltas are sealed
    /// into a write-once generation BEFORE retire; `fold_seal` is the durable index of WHAT WAS FOLDED
    /// (a CasFoldSeal; fence/recheck/trim write the separate CasCompletionSeal), `completion_seal` is
    /// populated by fence/recheck/trim and written write-once at completion, `root_shards` the
    /// discovered universe (for fence/trim), and `mf_cleanup` the part-manifest cleanup work keyed by
    /// ManifestId (owner-removed bodies whose exact-token delete is deferred until their decrements are
    /// sealed — spec §Retire / §Recheck).
    struct FoldResult
    {
        CasFoldSeal fold_seal;
        CasCompletionSeal completion_seal;
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;
        std::map<ManifestId, Token> mf_cleanup;
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
    FoldResult fold(GcState & state, Token & state_token, RoundReport & report);

    /// Read ONE part manifest named by `id`, validate it, and append sign*(+1) blob deltas for each
    /// blob entry to `deltas`. On sign<0 queue (id -> token) into mf_cleanup. Returns whether a body was
    /// read+validated: false => ABSENT body (404; the caller decides per the 404 rule). A body that is
    /// PRESENT but fails refMatchesBody / manifestNamespaceMatches throws CORRUPTED_DATA.
    bool foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup);

    /// What one R2 retire produced. `blobs` is the per-shard retired set (the input to R4 recheck);
    /// `mf_cleanup` carries the owner-removed manifest bodies' tokens straight through from the fold
    /// (captured at fold time; their exact-token delete is deferred to recheck, after decrements sealed).
    struct RetireResult
    {
        std::map<uint64_t, RetiredSet> blobs;
        std::map<ManifestId, Token> mf_cleanup;
    };

    /// R2 (spec §Retire): the round being executed is state.round + 1. Blob candidates are the
    /// zero-in-degree blobs in the sealed fold generation; ONE HEAD per candidate observes its CURRENT
    /// token (an absent object is SKIPPED — never fabricate a token; never GET a condemned body). The
    /// per-shard retired sets are written by unique path (putIfAbsent, byte-equal crashed-attempt
    /// adoption) BEFORE one gc/state CAS advances .round — the durable "retire phase complete" marker
    /// (ViewableRound). On success `state`/`state_token` carry the committed round.
    RetireResult retire(GcState & state, Token & state_token, const FoldResult & folded, RoundReport & report);

    /// R3 (spec §Global Fence, Task 4 LIST-based): CAS fence_round := round (monotone max) into every
    /// PRESENT root shard discovered by LIST(cas/refs/). Absent shards (namespaces with no prior publish)
    /// are not fenced — a first publish into a brand-new namespace creates the shard and stamps incarnation;
    /// promote's unconditional blob revalidation guards against condemned blobs in the brand-new shard.
    /// Record each shard's committed shard_version in gc/state.fence_version[round] AND in
    /// folded.completion_seal.fence_positions. One fence covers the whole round.
    void fence(GcState & state, Token & state_token, FoldResult & folded);

    /// R4 (spec §Recheck And Delete): fold every fenced shard's owner transitions in (sealed_cursor,
    /// fence_version] into a completion generation merged on top of the fold generation (FoldedThroughFence
    /// — defends SabotageCutOverclaim #12); per retired blob spare (in-degree > 0 in the completion
    /// generation) or exact-token delete (THE SINGLE CONTENT-DELETE SITE); then exact-token-delete the
    /// owner-removed manifest bodies whose decrements are now sealed (control #11). Seals the completion
    /// generation + advances the pointer + drops the round's retired sets in one CAS.
    void recheck(GcState & state, Token & state_token, FoldResult & folded, const RetireResult & retired,
                 RoundReport & report);

    /// Trim (INV-JOURNAL-COVERAGE): drop owner events with transition_version <= the sealed fold cursor,
    /// sourced from `CasFoldSeal::per_ns_shard[cursorKey].folded_cursor` (the generation carrying those
    /// deltas is durable: fold/recheck sealed it before trim runs). A shard absent from the sealed
    /// coverage is not trimmed (no fallback to a looser cursor). The cursor used per shard is recorded
    /// into `folded.completion_seal.trim_cursors` for the in-round audit log.
    void trim(FoldResult & folded, uint64_t round);

    /// Best-effort cursor-paced orphan part-manifest sweep. This is cleanup-only state: a lost CAS only
    /// discards cursor progress and must not fail the already-completed GC round.
    void runManifestSweepCursorPass(GcState & state, Token & state_token);

    /// Discover the present (namespace, shard) pairs from LIST(cas/refs/); shared by the fold and the
    /// resume re-fence. Returns only shards that have a backend object (absent shards not included).
    /// Fresh pool (no shards yet) => empty result.
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverse();

    /// Phase-2 token-diff: do ONE LIST sweep over `<prefix>/roots/` and return, for each listed key
    /// whose backend surfaced an incarnation token, that token. Only meaningful when
    /// `supportsListTokens()` is TRUE; the result is an accelerator — a missing key here means Read
    /// (fail closed). Key format: the full backend key (e.g. `p/roots/ns/0`); callers strip the roots
    /// prefix to get the "ns/shard" cursor-key form when looking up in `per_ns_shard`.
    ///
    /// AMBIGUITY DETECTION: a key seen more than once across all pages is ambiguous (a backend anomaly
    /// or a racing write that landed between two pages). Ambiguous keys are reported into
    /// `ambiguous_keys` so that `computeDiscoverDecisions` can force those shards to Read (fail closed).
    std::map<String, Token> listRootShardTokens(std::set<String> & ambiguous_keys);

    /// Phase-2 token-diff: compute the per-shard `DiscoverDecision` for the upcoming round. The universe
    /// is ALWAYS the REGISTRY universe (LIST is only an accelerator and can never remove a shard). The
    /// default decision is Read (fail closed; the spec, not a hidden fallback). A shard is Skip IFF:
    ///   `supportsListTokens()` is TRUE, AND
    ///   `sealed.per_ns_shard` has prior coverage for the key, AND
    ///   the LIST sweep surfaced a token for the key, AND
    ///   that listed token == the sealed `folded_token` (the post-fence token recheck recorded), AND
    ///   the shard is NOT clamped: if `fence_positions` has the key with `fence_pos > 0` and
    ///   `folded_cursor + 1 < fence_pos`, the previous fold was barrier-clamped (unfolded events may
    ///   exist) => forced Read.
    std::map<String, DiscoverDecision> computeDiscoverDecisions(
        const CasFoldSeal & sealed,
        const std::map<String, uint64_t> & fence_positions);

    /// The shard numbers GC must visit for a namespace: the static fan-out [0, root_shards) (every
    /// namespace — table AND precommit — uses it). The fence mints fence-only manifests for absent ones.
    std::vector<uint64_t> shardsToVisit(const RootNamespace & ns);

    /// CRASH-RESUME (the model: idempotent replay of a crashed round's tail). An INCOMPLETE round is
    /// detectable from durable state alone: a fold_seal exists for the latest generation with no
    /// completion_seal (and retired sets at (state.round, fence_seq) are present). The resume re-runs the
    /// round TAIL (fence -> recheck -> trim) from the durable fold_seal; exact-token deletes are
    /// idempotent (NotFound => Absent). Returns true if a resume ran. WHICH seal exists IS the durable
    /// phase marker (completion_seal => done; fold_seal only => resume at recheck; neither => redo fold).
    bool tryResumeIncompleteRound(GcState & state, Token & state_token, RoundReport & report);

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
    void pruneSupersededGenerations(uint64_t adopted_generation, uint64_t attempt, GcState & next);

    /// Read the fold seal for (generation, attempt) (nullopt when absent). Used by resume + parent-cursor reads.
    std::optional<CasFoldSeal> readFoldSeal(uint64_t generation, uint64_t attempt);

    /// Read the completion seal for (generation, attempt) (nullopt when absent).
    std::optional<CasCompletionSeal> readCompletionSeal(uint64_t generation, uint64_t attempt);

    /// M1: the per-(ns,shard) fold cursor coverage as of `generation`, read from the LATEST seal there
    /// (completion seal's folded_cursors if the round finished, else the fold seal's per_ns_shard, else
    /// empty). This is what the next fold keys its parent cursor off — recovered with NO dependence on
    /// trim having run, so a folded-but-untrimmed event is never re-folded from 0 (no in-degree double-count).
    std::map<String, ShardCoverage> readSealedCursors(uint64_t generation, uint64_t attempt);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    StorePtr store;
    UInt128 gc_id{};              /// this leader's identity (random u128, never 0)
    bool trim_enabled = true;     /// TEST SEAM ONLY (M1): production always trims; see setTrimEnabledForTest
    bool maintenance_trim = false; /// B12 TEST SEAM: bypass lazy-trim thresholds for one round (full compaction); see setMaintenanceTrimForTest

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
    /// B160: the heartbeat observed alongside the lease (gates the steal).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;

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

};

}
