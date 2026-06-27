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

    /// B167 per-server build watermark: start a fresh GC round's watermark observation. Clears the
    /// PER-ROUND caches; the ACROSS-ROUND maps persist (the K=2 frozen-seq crash detector's memory).
    void beginWatermarkRound();

    /// B171 precommit reclaim: while folding a precommit shard, drop the precommits of ABANDONED builds
    /// (the owning build is no longer live). The precommit namespace is sharded like any namespace, so
    /// one shard holds MANY precommit refs each named `std::to_string(build_seq)`. A build is DEAD iff
    /// the server has no watermark, is judged not-live this round (K=2 frozen-seq crash detector), or
    /// `build_seq < min_active`. Each dead ref is dropped + a removal RootOwnerEvent appended in ONE CAS
    /// on the shard already in hand.
    void reclaimAbandonedPrecommit(const RootNamespace & ns, uint64_t shard, const RootShard & root,
                                   uint64_t round);

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

    /// R3 (spec §Global Fence): CAS the NAMESPACE REGISTRY's fence_round first (the ordering point for
    /// namespace creation), then CAS fence_round := round (monotone max) into EVERY root shard of every
    /// namespace in the FENCE-TIME registry (minting fence-only manifests for absent shards). Record
    /// each shard's committed shard_version (and the registry's committed version under "_registry") in
    /// gc/state.fence_version[round] AND in folded.completion_seal.fence_positions. The manifest CAS
    /// totally orders the fence against publishes on that shard. One fence covers the whole round.
    void fence(GcState & state, Token & state_token, FoldResult & folded);

    /// R4 (spec §Recheck And Delete): fold every fenced shard's owner transitions in (sealed_cursor,
    /// fence_version] into a completion generation merged on top of the fold generation (FoldedThroughFence
    /// — defends SabotageCutOverclaim #12); per retired blob spare (in-degree > 0 in the completion
    /// generation) or exact-token delete (THE SINGLE CONTENT-DELETE SITE); then exact-token-delete the
    /// owner-removed manifest bodies whose decrements are now sealed (control #11). Seals the completion
    /// generation + advances the pointer + drops the round's retired sets in one CAS.
    void recheck(GcState & state, Token & state_token, FoldResult & folded, const RetireResult & retired,
                 RoundReport & report);

    /// Trim (INV-JOURNAL-COVERAGE): drop owner events with transition_version <= the sealed folded_cursor
    /// (the generation carrying those deltas is durable: fold/recheck sealed it before trim runs). Shards
    /// with nothing to trim are not touched. The trim's own CAS bumps shard_version with no event.
    void trim(const FoldResult & folded, uint64_t round);

    /// Discover the namespace universe from the registry (namespaces x ALL root_shards shards); shared by
    /// the fold and the resume re-fence. Absent registry => empty (fresh pool).
    std::vector<std::pair<RootNamespace, uint64_t>> discoverUniverse();

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
    void writePartManifestCleanupBundle(uint64_t generation, uint64_t owner_shard,
                                        const std::map<ManifestId, Token> & cleanup, std::vector<RunRef> & out);

    /// Read the fold seal for `generation` (nullopt when absent). Used by resume + parent-cursor reads.
    std::optional<CasFoldSeal> readFoldSeal(uint64_t generation);

    /// The cursor a prior fold sealed for (ns, shard): the parent generation's fold seal's coverage, or
    /// 0 when absent (fresh pool / first fold).
    uint64_t sealedCursorOf(const CasFoldSeal & parent, const String & cursor_key);

    /// Update the remembered observation (steal protocol step 3/4).
    void rememberObservation(const GcLease & lease);

    /// B167: fetch the owning server's watermark for THIS round (cached). On the first fetch of a server
    /// in a round, this also computes that server's liveness verdict ONCE (frozen-seq K=2). Returns
    /// nullptr if the server has no watermark object (an unrecognized owner).
    const ServerWatermark * watermarkOf(UInt128 server_id);

    StorePtr store;
    UInt128 gc_id{};              /// this leader's identity (random u128, never 0)

    /// the contender's observation window (steal protocol)
    bool has_observation = false;
    UInt128 last_seen_owner{};
    uint64_t last_seen_seq = 0;
    /// B160: the heartbeat observed alongside the lease (gates the steal).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;

    /// B167 per-server build watermark caches. The PER-ROUND maps are cleared by beginWatermarkRound;
    /// the ACROSS-ROUND maps persist (the K=2 frozen-seq crash detector's memory).
    std::map<UInt128, ServerWatermark> watermark_cache;   /// per-round: server_id -> its watermark
    std::map<UInt128, bool> server_live_this_round;       /// per-round: liveness verdict, computed once
    std::map<UInt128, uint64_t> last_seen_server_seq;     /// across rounds: last seq observed per server
    std::map<UInt128, uint64_t> server_frozen_rounds;     /// across rounds: consecutive rounds seq unchanged
};

}
