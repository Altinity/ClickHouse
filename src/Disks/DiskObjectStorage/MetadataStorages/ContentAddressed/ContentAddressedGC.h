#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage_fwd.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DB::ContentAddressed
{

class GcCompaction;
struct WriteSession;

/// The whole garbage-collection concern for a content-addressed pool lives here: the pure
/// reachability/sweep algorithms, the pool enumeration primitives, the un-wired refcount seam, and
/// the sweep driver `ContentAddressedGC`. The threading/lifecycle driver is `ContentAddressedGCThread`.

/// ==== Pool enumeration (was PoolScan) ====

/// The ref-object payload is a versioned struct on the shared codec (B28): `MAGIC(4) + version(1) +
/// part_id` (the part id as a length-prefixed string). The magic is `CARF` ("Content-Addressed ReF").
/// Version 1 holds only the part id; later versions append additive fields (B1's
/// `ReplicatedMergeTreePartHeader`, the per-ref mutable fields) WITHOUT breaking older readers, which
/// stop after the part id. The format is reserved for that growth on purpose.
constexpr FormatMagic kRefPayloadMagic = makeMagic("CARF");
constexpr uint8_t kRefPayloadVersion = 1;

/// Serialize a ref payload naming `part_id` (the publishing write path's payload). The SINGLE writer
/// paired with the SINGLE parser `partIdFromRefPayload` below.
std::string serializeRefPayload(const PartId & part_id);

/// Resolve a ref object payload into the part id it names. Parses the versioned `serializeRefPayload`
/// format EXACTLY: a wrong magic is `CORRUPTED_DATA` and a version newer than this build understands
/// is `NOT_IMPLEMENTED` (both fail-close — a published ref must name a part, and an unknown version
/// must never be misinterpreted). This is the SINGLE ref-payload parser: both the GC live-set scan
/// (`listLivePartIds`) and the read path (`ContentAddressedMetadataStorage::readRefPartId`) resolve a
/// ref through it, so they cannot disagree on the part id by construction (B28).
PartId partIdFromRefPayload(const std::string & payload);

/// Enumerate the full set of LIVE part ids in a content-addressed pool: every published ref under
/// the pool's refs root (the store/server/uuid/refs/part layout) names a part id (its payload). These
/// are the GC roots — a part id is live iff at least one ref points at it. Any list or read error
/// PROPAGATES so the caller aborts the sweep: a partial scan must never drive deletion (fail-close).
std::set<PartId> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix);

/// List the object keys of every object under a pool root prefix (e.g. partsPrefix / blobsPrefix).
/// Thin wrapper over object_storage->listObjects; errors propagate. The returned strings are the raw
/// object keys; the GC wraps them into the right typed key (BlobObjectKey / PartObjectKey) at the one
/// well-marked boundary in runSweepOnce so reachable-vs-listed can only be compared in one key space.
std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix);

/// ==== Pure reachability/sweep algorithms (was Reachability) ====

using PartManifestResolver = std::function<PartManifest(const PartId & part_id)>;

/// Reachable blob object-key set from the live roots (refs -> part manifests -> blob object keys).
/// A manifest stores the BARE content hash in each `BlobEntry.key` (the production write path records
/// `BlobEntry{blob_hash, size, blob_hash}`), while the GC sweep enumerates FULL object keys under
/// `blobsPrefix(key_prefix)`. To make the two comparable the reachable set is built with the SAME
/// `blobKey(key_prefix, bare_hash)` fan-out the read path uses, so reachable == full blob object key.
/// The return type is `std::set<BlobObjectKey>`: the sweep can ONLY compare it against listed blobs
/// after wrapping those into `BlobObjectKey` too, so a bare-hash-vs-object-key mismatch cannot compile.
std::set<BlobObjectKey> markReachableBlobs(
    const std::string & key_prefix, const std::set<PartId> & live_part_ids, const PartManifestResolver & resolve);

/// Blob object keys pinned by LIVE cross-mounter write sessions (the cross-process generalization of
/// the in-process B52 pin). Lists `sessionsPrefix`, deserializes each `WriteSession`, and for every
/// session whose `lease_deadline_unix >= now` (an EXPIRED session is itself reclaimable — a crashed
/// writer must not pin blobs forever) projects its `pending` bare hashes to FULL blob object keys with
/// the SAME `blobKey` fan-out `markReachableBlobs` uses, so the result is directly comparable to the
/// reachable set (no bare-hash-vs-object-key mismatch — the historical data-loss bug class). A live
/// session's pinned keys are roots: a blob just uploaded but not yet referenced by a published ref must
/// not be reclaimed out from under the writer. Any list/read error PROPAGATES (fail-close): a partial
/// session scan must never drive deletion.
std::set<BlobObjectKey> sessionPinnedBlobs(
    const ObjectStoragePtr & object_storage, const std::string & key_prefix, int64_t now);

/// Part-manifest object keys pinned by LIVE cross-mounter write sessions. The write path's session
/// identifies its part by the writer's part NAME (the content part_id is only known at commit), so its
/// `part_id` field is not a manifest key and contributes nothing here. The fetch-by-relink path, by
/// contrast, opens its session over an ALREADY-COMMITTED `part_id` (CAS replication Phase 2a): it pins
/// not only the part's blobs but the `parts/<part_id>` MANIFEST object itself, because a relink onto
/// this server holds no ref yet and the source replica may concurrently drop its ref — after which a
/// sweep would see the manifest named by no ref and reclaim it in the window before this server's ref is
/// published (spec §4, the relink data-loss hole). So a live session whose `part_id` resolves to a real
/// `parts/` object pins that manifest as reachable. Mirrors `sessionPinnedBlobs`: an EXPIRED session is
/// itself reclaimable; list/read errors PROPAGATE (fail-close); a vanished session is simply skipped.
std::set<PartObjectKey> sessionPinnedPartKeys(
    const ObjectStoragePtr & object_storage, const std::string & key_prefix, int64_t now);

struct SweepResult
{
    std::vector<std::string> to_delete;
    std::unordered_map<std::string, int64_t> first_unreachable; /// updated timers (cleared for reachable-again)
};

/// `grace` is measured from first loss of reachability (NOT object age). Reachable-again clears the timer.
/// Operates on raw object-key strings — the GC has already reduced both blob and part keys to the
/// same unreferenced-object-key space before calling this, so no typed distinction is needed here.
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace);

/// ==== Un-wired refcount seam (was BlobRefIndex) ====

/// NOTE: BlobRefIndex is an un-wired future seam (B9), not on the M1 GC path.
/// Seam (B9): the delta refcount over content-addressed blob hashes.
/// M1 ships InMemoryBlobRefIndex; a RocksDB-backed impl plugs in here unchanged.
class IBlobRefIndex
{
public:
    virtual ~IBlobRefIndex() = default;
    virtual void addPart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual void removePart(const PartId & part_id, const PartManifest & manifest) = 0;
    virtual int64_t refcount(const BlobHash & blob_hash) const = 0;
    virtual std::set<BlobHash> unreferenced() const = 0;
};

class InMemoryBlobRefIndex : public IBlobRefIndex
{
public:
    void addPart(const PartId & part_id, const PartManifest & manifest) override;
    void removePart(const PartId & part_id, const PartManifest & manifest) override;
    int64_t refcount(const BlobHash & blob_hash) const override;
    std::set<BlobHash> unreferenced() const override;

    /// The set of bare blob hashes the index currently PINS (refcount > 0). The complement of
    /// `unreferenced` over the tracked hashes; used by the sweep's drift validator (CA GC S1) to build
    /// the index's reachable view in the same key space as the authoritative scan.
    std::set<BlobHash> referenced() const;

private:
    /// Commits (and drops) run concurrently across threads, so every mutator and observer locks `mtx`.
    /// The `applied_parts` idempotency guard makes add/remove safe to retry within the held lock.
    mutable std::mutex mtx;
    std::unordered_map<BlobHash, int64_t> counts;
    std::unordered_set<PartId> applied_parts; /// idempotency guard for add/remove
};

/// ==== Sweep driver ====

struct SweepStats
{
    size_t deleted_blobs = 0;
    size_t deleted_parts = 0;
};

/// Reachability garbage collector for one content-addressed pool (single process — see the M1 GC
/// safety invariants). It enumerates the live part ids (the published refs), computes the reachable
/// blob set from their manifests, and deletes ONLY manifests under parts/ and blobs under blobs/ that
/// have been continuously unreferenced for at least `grace` seconds. Refs, table-level files and
/// generic disk files are owned by the table and never touched here.
///
/// `first_unreachable` is the across-sweeps timer state (grace is measured from the first sweep that
/// found an object unreferenced, NOT from object age); reachable-again clears an object's timer.
/// It is held in memory for the process lifetime (M1): on restart it resets, which only makes grace
/// conservative (never premature deletion).
class ContentAddressedGC
{
public:
    /// `gc_lock_` is the per-pool in-process mutex shared with the transaction commit path (B49/B52).
    ///
    /// CA GC S4 (G1) — the lock is DEMOTED to a narrow CONTAINER GUARD: it no longer serializes the whole
    /// commit against the whole sweep. It now protects ONLY in-process mutation/read of the shared
    /// `in_flight_pinned_blobs` `std::set` (so concurrent insert/erase/read is not a data race). The sweep
    /// takes it briefly to snapshot that set (`snapshotInFlightPinnedBlobs`) and then runs lock-free; the
    /// commit takes it only around its pin insert/erase. The cross-process commit-vs-sweep safety that the
    /// old held-across-the-sweep lock provided is now carried by the durable two-flag §7 handshake (the
    /// writer raises the session pin BEFORE its tomb re-check and the `+`; the GC seals `.tombstone` BEFORE
    /// its fresh authoritative §6.2 re-check, which reads the live session set) plus the fence lease for
    /// GC-vs-GC. When null (legacy direct construction) a private mutex is created so the container guard is
    /// still internally consistent.
    /// `in_flight_pinned_blobs_` is the per-pool set of blob object keys staged by uncommitted
    /// transactions (B52), shared by reference and read under `gc_lock_`. The sweep treats every pinned
    /// key as reachable so a blob a dedup-skipping insert decided to reuse (and so did NOT re-upload)
    /// cannot be deleted in the window between that skip and the ref publish. May be null (legacy direct
    /// construction / tests that drive no concurrent inserts): then nothing is pinned.
    /// `blob_ref_index_` is the per-pool incremental reverse index (B9 / CA GC S1), shared by reference
    /// with the commit/drop path. The sweep VALIDATES it against the authoritative full-scan and LOGS
    /// drift — it never gates a deletion on it (the scan stays authoritative). May be null (legacy direct
    /// construction / unit tests that build their own index): then the drift validator is skipped.
    ContentAddressedGC(
        ObjectStoragePtr object_storage_,
        std::string key_prefix_,
        std::shared_ptr<std::mutex> gc_lock_ = nullptr,
        std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs_ = nullptr,
        std::shared_ptr<InMemoryBlobRefIndex> blob_ref_index_ = nullptr);

    /// Run one sweep. Deletes nothing if any step before the removal throws (fail-close): a missing
    /// manifest for a live ref (B18), or any list/read error, aborts the sweep with the pool intact.
    /// CA GC S4 (G1) — the sweep NO LONGER holds the `gc_lock` across mark+delete. It snapshots the
    /// in-flight pin set once under the narrow container guard, then runs lock-free; the sole cross-process
    /// commit-vs-sweep gate is the §7 handshake (seal `.tombstone` BEFORE the fresh §6.2 re-check, which
    /// reads the live session set the writer raised before its own tomb re-check). The fence lease still
    /// gates GC-vs-GC.
    ///
    /// Roots = live refs UNION every LIVE write-session's `pending` blobs (M8): a blob uploaded but not
    /// yet referenced by a published ref is pinned by its writer's session object in the bucket, so a
    /// remote sweep keeps it. A candidate is also RE-VALIDATED immediately before deletion (refs + live
    /// sessions re-read) to close the read-refs-then-list-objects enumeration race.
    ///
    /// `held` is the GC-leader lock this caller holds (when run by the coordinated background thread).
    /// When set, the fence-ownership guard re-confirms before each removal that `pool/gc.lock` STILL
    /// carries `held->fence_token`; if a successor stole leadership (a higher fence on disk), the sweep
    /// STOPS deleting (a paused holder must never delete after a higher fence took over). When nullopt
    /// (single-owner / unit tests) the guard is skipped.
    SweepStats runSweepOnce(int64_t now, int64_t grace, std::optional<GcLock> held = std::nullopt);

    /// CA GC S2 — the EXPLICIT reconciliation fallback (the rare full `parts/`+`blobs/` scan, spec §9).
    /// This is the PRE-S2 candidate source, kept ONLY as the recovery path for rebuild / abandoned-upload /
    /// orphan-drift: it lists every object under `parts/` and `blobs/`, computes reachability from the live
    /// refs + manifests + live sessions, and reclaims the unreferenced complement under the SAME grace +
    /// fence + delete primitive as the normal path. It is NOT on the normal path (the normal path is
    /// compaction-driven — `runSweepOnce`); it is scheduled by the bounded policy (`reconciliationDue`),
    /// run manually, or used as the total-loss fallback when the log+snapshot are gone. Same fail-close,
    /// re-validate-under-lock, and fence guarantees as `runSweepOnce`.
    SweepStats runReconciliationScan(int64_t now, int64_t grace, std::optional<GcLock> held = std::nullopt);

    /// §9 orphan-drift bound: set the cadence (in normal sweep rounds) at which `runSweepOnce` folds in the
    /// heavy reconciliation scan's candidates, so orphan drift the compaction cannot see (abandoned uploads,
    /// hand-seeded / externally-mutated objects with no `gc/log` delta) is bounded. 0 (the default) disables
    /// the scheduled scan: the normal path stays purely compaction-driven and reconciliation is then only
    /// manual / the total-loss fallback. A positive N runs reconciliation once every N rounds. Set from the
    /// disk config by the background thread.
    void setReconciliationCadenceRounds(int64_t rounds) { reconciliation_cadence_rounds = rounds; }

private:
    /// CA GC S3 — the GENERATION-AWARE seal -> grace -> fresh re-check -> {recover|drain|sweep} tail used by
    /// BOTH the compaction-driven normal path and the reconciliation fallback. `candidate_object_keys` are
    /// the generationed candidate object keys (`blobs/<H>/<g>` / `parts/<part_id>/<mg>`) whose folded count
    /// reached 0 (compaction), or the full-scan unreferenced complement (reconciliation). The tail, per
    /// candidate:
    ///   (a) SEAL — `condCreateIfAbsent(<g>.tombstone)`: a DURABLE, single-owner, fence-gated condemnation.
    ///       Once sealed no new attach may target `g` (the writer's tomb re-check routes reuse to `g+1`).
    ///       The seal — NOT an in-memory timer — is the persistent condemned-state, so a candidate
    ///       condemned in round 1 is re-discovered (via `collectSealedTombstoneCandidates`) and
    ///       processed in round N after grace, closing the S2 grace>0 liveness gap.
    ///   (b) GRACE — liveness ageing from the seal (NEVER a safety fence — the fresh re-check is the gate).
    ///   (c) FRESH authoritative re-check (§6.2) AFTER the seal: refs -> manifests reachability + live
    ///       sessions (S3 keeps this pass; the §6.2 sessions+compaction-only form is S4).
    ///   (d) BRANCH:
    ///       - no ref/session for `(id, g)` -> SWEEP: delete the gen object, best-effort reset `active` off
    ///         `g`, KEEP the `<g>.tombstone` gravestone forever.
    ///       - ref/session for `(id, g)` AND no successor generation exists -> RECOVER: delete the tombstone
    ///         (un-seal), re-open `g` as attachable.
    ///       - ref/session for `(id, g)` BUT a successor `g+1…` already exists -> DRAIN: KEEP the tombstone
    ///         AND the gen object (still referenced — do NOT delete), do NOT re-open `g`.
    /// CA GC S4 (G1): runs LOCK-FREE — the gc_lock is no longer held across the sweep. Reads in-flight pins
    /// only from the supplied lock-free pinned_snapshot (never the shared set).
    /// Symmetric for blobs and manifests (§9): both run the identical machinery on their
    /// `(identity, generation)` key family.
    SweepStats sweepCandidates(
        const std::set<std::string> & candidate_object_keys,
        const std::set<std::string> & pinned_snapshot,
        int64_t now,
        int64_t grace,
        const std::optional<GcLock> & held);

    /// CA GC S4 (G1) — take a CONSISTENT snapshot of the per-pool in-flight blob pin set (B49/B52) under the
    /// narrow container mutex (`gc_lock`, demoted from the old commit-vs-sweep serialization mutex to a pure
    /// container guard — see the class doc). With the lock no longer held across the whole sweep, the sweep
    /// must not read the shared `std::set` while a commit mutates it (a data race); instead it copies it ONCE
    /// here under the brief lock and runs the rest of the sweep lock-free against the copy. A pin that lands
    /// AFTER the snapshot is irrelevant: a freshly-pinned blob is a fresh g=0 the compaction cannot yet have
    /// emitted as a count-0 candidate, and the cross-process safety rests on the durable session + the §7
    /// handshake, not on this in-process pin.
    std::set<std::string> snapshotInFlightPinnedBlobs() const;

    /// CA GC S3 — enumerate every sealed-but-unswept tombstone in the pool as a candidate, so a generation
    /// condemned in an earlier round (its `<g>.tombstone` is durable) is re-processed every round until it
    /// is swept or recovered (closing the S2 grace>0 gap where a count-0 candidate emitted only in the
    /// crossing fold was forgotten before grace expired). Lists `blobsPrefix` + `partsPrefix`, keeps only
    /// `.tombstone` keys, and maps each back to its generation object key (`<g>`).
    /// CA GC S4 (G1): runs lock-free (the gc_lock is not held across the sweep).
    std::set<std::string> collectSealedTombstoneCandidates();

    /// CA GC S3 — resolve a part's manifest body at ANY present generation (§6.1/§9). A live ref pins the
    /// bare `part_id`, but the physical manifest may have been RESURRECTED to `mg>0` (the GC sealed `mg=0`
    /// and a contended commit re-created the manifest at `mg+1`), so the steady `parts/<id>/0` key can be
    /// absent for a perfectly-live part. Mirror the reader's `repairPartGenOn404`: try `mg=0` first (one
    /// HEAD/GET in the common case), and on a 404 LIST the per-id generation prefix and read the HIGHEST
    /// present generation. Throws `CORRUPTED_DATA` only when NO generation of a live part's manifest exists
    /// (a genuine dangling ref — fail-close). Used by both the reachability re-check and the reconciliation
    /// scan so the GC never mistakes a resurrected-manifest live part for a missing one.
    PartManifest resolveManifestAtAnyGeneration(const PartId & part_id) const;

    /// CA GC S3 — true iff a present generation OBJECT (not a tombstone / not `active`) with a generation
    /// strictly greater than `generation` exists under the identity's directory. A resurrection lands at
    /// max+1, so this is the "a successor `g+1…` exists" signal that splits DRAIN (keep `g`) from RECOVER
    /// (un-seal `g`). LISTs the per-identity generation prefix only.
    bool successorGenerationExists(bool is_blob, const std::string & identity, uint64_t generation) const;

    /// CA GC S3 (§6.1) — best-effort reset of the `active` hint off a generation being SWEPT: if `active`
    /// names `swept_generation`, repair it (plain PUT, not a CAS — a hint) to the highest surviving
    /// generation object, so a reader's default no longer points at a deleted object. Non-fatal on failure
    /// (the reader's 404 -> LIST fallback repairs a stale hint); leaves `active` untouched if it does not
    /// name the swept generation or no surviving generation remains.
    void resetActiveOffGeneration(bool is_blob, const std::string & identity, uint64_t swept_generation);

    /// The full `parts/`+`blobs/` scan's unreferenced complement — the reconciliation fallback's candidate
    /// source (spec §9), shared by `runReconciliationScan` and the scheduled orphan-drift fold inside
    /// `runSweepOnce`.
    /// CA GC S4 (G1/#5): runs lock-free; reads in-flight pins only from the supplied pinned_snapshot.
    std::set<std::string> collectReconciliationCandidates(int64_t now, const std::set<std::string> & pinned_snapshot);

    /// CA GC S4 — the session-until-folded REAPER (§5.1 rule 3, §7.3 mechanical rule). A write session is the
    /// durable handshake flag covering a `+`-before-fold gap (§7.1): it must be retained until EVERY one of
    /// its `+` deltas is folded into a durable snapshot, so `sessions ∪ folded-snapshot` always covers every
    /// live reference (the §6.2 gate's completeness premise). This reaper enumerates `sessions/` and deletes
    /// a session ONLY when the §7.3 mechanical rule is met: (a) its ref was never committed (`committed` is
    /// false — but those are dropped by the OWNER at abort, so a lingering uncommitted one is left to its
    /// lease), OR (b) EVERY `(shard, epoch)` in its `delta_epochs` is folded
    /// (`compaction.isEpochFolded`). NEVER on a bare timer for a committed-but-unfolded session — the
    /// foldedness watermark, not the lease, is the reap gate (the lease only reclaims a CRASHED writer's
    /// pin). Timer-safe: the seal/gravestone is permanent, so a reaped-then-resumed writer re-checks the
    /// tomb and resurrects (§7.3). Returns the number reaped.
    ///
    /// CA GC S4 (G1) — runs WITHOUT the `gc_lock` (it is no longer held across the sweep). The reaper is
    /// race-safe by construction against a concurrent commit raising a new session: it (a) skips a session
    /// that vanished between the LIST and the READ (an owner commit/abort deleted it), (b) skips any
    /// `committed == false` session (a just-raised, still-uploading pin), and (c) deletes only when EVERY
    /// delta epoch is provably folded. A session raised AFTER the LIST is simply not seen this round (it is
    /// reaped a later round once folded). So it can never delete a just-created or unfolded session.
    size_t reapFoldedSessions(GcCompaction & compaction);

    /// CA GC S4 (#2): persist a (possibly mutated) write session back to its key with a plain Rewrite PUT.
    /// Used by the sticky-session conversion in `reapFoldedSessions` to clear the sticky flag durably after
    /// the bounded `+` re-log lands, so a crash thereafter sees a normal committed session, not a sticky one.
    void rewriteSession(const std::string & session_key, const WriteSession & session);

    /// The bounded reconciliation policy (§9 "orphan-drift bound"): true iff the heavy reconciliation scan
    /// is due this round — every `reconciliation_cadence_rounds` rounds (a cadence knob), so orphan drift
    /// (over-counts, abandoned uploads the compaction cannot see) is bounded rather than left to a vague
    /// "rare". The orphan-byte threshold is a future knob; for S2 the cadence is the bound. Returns false
    /// when the cadence is 0 (disabled — tests / single-shot drive reconciliation explicitly).
    bool reconciliationDue();

    const ObjectStoragePtr object_storage;
    const std::string key_prefix;
    /// Per-pool in-process mutex, shared with ContentAddressedTransaction::commit (B49/B52). CA GC S4 (G1):
    /// demoted to a NARROW container guard — it protects only `in_flight_pinned_blobs` access, NOT the whole
    /// commit-vs-sweep ordering (that is the §7 handshake's job now). Never null after construction.
    const std::shared_ptr<std::mutex> gc_lock;
    /// Per-pool in-flight blob pins (B52), snapshotted under gc_lock; pinned keys are excluded from sweep.
    /// Null when the GC was built without a metadata storage (no concurrent inserts to protect).
    const std::shared_ptr<const std::set<std::string>> in_flight_pinned_blobs;
    /// Per-pool incremental reverse index (B9 / CA GC S1), validated against the authoritative scan and
    /// logged for drift only — NEVER gating a deletion. Null when no index was supplied (drift skipped).
    const std::shared_ptr<InMemoryBlobRefIndex> blob_ref_index;
    std::unordered_map<std::string, int64_t> first_unreachable;

    /// §9 orphan-drift bound: run the heavy reconciliation scan every Nth normal round. 0 disables the
    /// scheduled reconciliation (the normal path is purely compaction-driven; reconciliation is then only
    /// manual / total-loss fallback). A configurable knob — modest default so drift is bounded but the
    /// hot path stays scan-free. `rounds_since_reconciliation` is the cadence counter.
    int64_t reconciliation_cadence_rounds = 0;
    int64_t rounds_since_reconciliation = 0;
};

}
