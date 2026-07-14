#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefStateMachine.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Common/CacheBase.h>
#include <Common/CurrentMetrics.h>
#include <Common/HashTable/Hash.h>
#include <Common/ProfileEvents.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace DB::Cas
{

/// Whether a root-shard mutation originates from the writer path (user-visible publish/drop/precommit)
/// or from GC/maintenance. Diagnostic-only (`toString`, event logging): recorded on the mutation item.
enum class RootMutationOrigin : uint8_t
{
    Writer,
    Gc,
};

/// The write-scope of one `appendRefOps` call (ref-append-lane batching): which part of the table the
/// call touches. The flat-combining batch builder admits at most ONE mutation per ref name into a
/// single flush (per-ref durable histories stay bit-identical to the unbatched protocol) and flushes
/// `WholeShard` calls SOLO (dropNamespace and anything touching multiple refs wholesale).
struct MutationScope
{
    enum class Kind : uint8_t { Ref, WholeShard };
    Kind kind = Kind::WholeShard;
    String ref_name;   /// set iff kind == Ref

    static MutationScope ref(String name) { return {Kind::Ref, std::move(name)}; }
    static MutationScope wholeShard() { return {Kind::WholeShard, {}}; }
};

/// Kind of mutation being applied, used in diagnostic logging and metrics. Does not affect behaviour.
enum class RootMutationKind : uint8_t
{
    Publish,
    Drop,
    Precommit,
    Promote,
    Abandon,
    UpdateRefPayload,
    DropNamespace,
    ReclaimPrecommit,
};

/// Human-readable name for `RootMutationOrigin` (diagnostic logging).
inline std::string_view toString(RootMutationOrigin origin)
{
    switch (origin)
    {
        case RootMutationOrigin::Writer: return "Writer";
        case RootMutationOrigin::Gc:     return "Gc";
    }
    return "Unknown";
}

/// Human-readable name for `RootMutationKind` (diagnostic logging).
inline std::string_view toString(RootMutationKind kind)
{
    switch (kind)
    {
        case RootMutationKind::Publish:           return "Publish";
        case RootMutationKind::Drop:              return "Drop";
        case RootMutationKind::Precommit:         return "Precommit";
        case RootMutationKind::Promote:           return "Promote";
        case RootMutationKind::Abandon:           return "Abandon";
        case RootMutationKind::UpdateRefPayload:  return "UpdateRefPayload";
        case RootMutationKind::DropNamespace:     return "DropNamespace";
        case RootMutationKind::ReclaimPrecommit:  return "ReclaimPrecommit";
    }
    return "Unknown";
}

struct PoolConfig
{
    String pool_prefix;
    UInt128 server_id{};                      /// owner token (ServerUUID) — provenance + watermark
    /// Explicit, configured identity of the layout subtree this server owns (spec §mount-safety,
    /// Phase 0). Required + validated (clean relative path); `ServerUUID`/`server_id` is demoted to an
    /// owner token. Validated via `Cas::validateServerRootId`.
    String server_root_id;
    uint64_t blob_header_len = 256;           /// creation-time only; the pool is authoritative on reopen
    /// CAS mixed-algo pools (Phase 3 T4, design 2026-07-11-cas-mixed-algo-pools-design.md §5): the
    /// NODE-LOCAL algo this Store writes NEW content with (`Store::writeAlgo()`). NOT durable pool
    /// state -- two live nodes may intentionally write with different (already-admitted) algos, so
    /// no single truthful pool-wide value exists. `PoolMeta::createOrValidate` accepts it with no
    /// write when it is already a member of the pool's `algos_used`; otherwise it is admitted via
    /// the CAS-union below (opt-in, `blob_hash_allow_new`) or refused (BAD_ARGUMENTS, the default --
    /// a changed config alone must never silently turn a pool mixed). Default `CityHash128` keeps
    /// every existing pool's hash byte-for-byte unchanged.
    BlobHashAlgo blob_hash_algo = BlobHashAlgo::CityHash128;
    /// Opt-in for `blob_hash_algo` to be ADMITTED into the pool's `algos_used` when it is not
    /// already a member (spec §5: "admission of a NEW algo is EXPLICIT OPT-IN"). Consulted only on
    /// the FIRST open that would otherwise refuse -- once admitted, `algos_used` membership alone is
    /// the steady-state check and this flag is not needed again for the same algo. Default `false`
    /// (fail-closed, matching the Phase 1/2 default-refuse behavior).
    bool blob_hash_allow_new = false;
    /// P1 (dedup cache): byte ceiling for the per-disk known-present blob-hash LRU set. 0 disables the
    /// cache (every create misses → P2-only). A hint cache; correctness never depends on it (a stale
    /// hit is caught by the mandatory HEAD in putBlob — design 2026-06-20, B168).
    uint64_t dedup_cache_bytes = 64ULL << 20;        /// 64 MiB
    /// P2 (HEAD-before-PUT): on a dedup-cache MISS, a blob whose body is >= this many bytes is written
    /// HEAD-first (a cheap HEAD avoids streaming a body that would 412). 0 disables the size trigger.
    uint64_t dedup_head_first_min_bytes = 1ULL << 20;   /// 1 MiB
    /// Phase-5 (part-folder cache spec): byte bound for the manifest DECODE cache. The old cache
    /// was count-bounded only (16384 entries) — decoded manifests carry inline bytes, so the worst
    /// case was multi-GB. 0 disables decode caching (every read decodes fresh — diagnostic mode).
    uint64_t manifest_decode_cache_bytes = 128ULL << 20;
    /// B174 (gc/snap retention): how many superseded snap generations to retain. After committing
    /// generation G, generations <= G - this are pruned (bounded per round). 0 = keep ALL
    /// (debug/forensics — replay GC's in-degree view as-of a past round). Default 3 = the safety
    /// margin covering any in-flight/resuming leader (a leader more than `keep` generations behind
    /// has lost its lease; its round-commit CAS fails).
    uint64_t gc_snap_generations_to_keep = 3;
    /// Blob target shards for GC (spec §Sharding Model). Default 1 (single-shard equivalence to
    /// Phase 1d). Creation-time only; the pool is authoritative on reopen. This is the
    /// BLOB-HASH-prefix reducer axis.
    uint64_t gc_shards = 1;
    /// Phase 2 cursor-paced orphan part-manifest sweep. The LIST budget bounds cold-prefix enumeration
    /// per completed GC round; the delete budget separately bounds exact-token destructive work.
    uint64_t manifest_sweep_list_budget_keys = 1000;
    uint64_t manifest_sweep_delete_budget_keys = 100;
    /// Phase-4 skip-unchanged (spec 2026-07-06-cas-gc-round-skip-unchanged): a GC round may DEFER
    /// (re-adopt the sealed in-degree generation instead of rebuilding it) when fewer than this many
    /// shards changed since the last fold AND no destructive decision is due. Default 1 = fold as soon
    /// as anything changed (batching off; only idle rounds defer). > 1 batches small deltas.
    uint64_t gc_fold_threshold = 1;
    /// Liveness bound for batching: force a FOLD after this many consecutive DEFER rounds even below
    /// the threshold. Inert at gc_fold_threshold == 1 (an idle defer has nothing to fold). Default 8.
    uint64_t gc_fold_max_defer_rounds = 8;
    /// gc-rebuild (spec 2026-07-03): max in-memory edges per gc-shard batch during rebuildBaseline
    /// (~32 B each => default ~256 MB); each full batch folds into the next attempt number with the
    /// previous attempt's runs as priors, so memory is O(budget), never O(edges).
    uint64_t rebuild_edge_budget = 8000000;
    /// Task 5 (spec 2026-07-09 §raw-body-refinement, v3): bounded pool size for the per-hash freshness
    /// meta writes GC schedules at condemn/spare/delete (mass-DROP: a round condemning ~1M blobs would
    /// take hours sequential). Every job internally catches its own exceptions (never wedges the round;
    /// feedback_ca_gc_never_throw_on_404) and `Gc::runRegularRound` waits for the round's whole batch
    /// before the round's single gc/state CAS, so the meta writes are durable before that CAS commits.
    uint64_t gc_meta_pool_size = 16;
    bool background_watermark = false;       /// tests drive renewOnce explicitly; gates the merged heartbeat's background thread

    /// Mount-lease TTL (spec §mount-safety): how long a freshly-renewed mount lease is valid. The local
    /// write fence's monotonic deadline is `renew_time + this`, so a superseded/paused writer is fenced
    /// once `this` elapses with no successful renew. The background renewer runs every
    /// `mount_renew_period` (default ttl/3) so a healthy mount renews well before expiry.
    std::chrono::milliseconds mount_lease_ttl_ms{30000};
    std::chrono::milliseconds mount_renew_period{10000};   /// = ttl/3 by default
    bool read_only = false;                   /// observe-only open: skip the mutating capability probe; reads only

    /// Boot-time "start now, fix later": skip the access-check-class part of the capability probe
    /// (the `_probe/` read/write/delete/list round trip and the store-precondition check) while
    /// STILL opening writable — a mistyped bucket / transient DNS blip at mount should not hard-fail
    /// the disk when the operator asked to defer the access check (U#5), mirroring `checkAccess`'s
    /// `skip_access_check` gate for other disk types. Does NOT skip the single-attempt conditional-
    /// write gate (`checkConditionalWriteSingleAttemptSupport`, RFC cas-s3-timeout-retry-control):
    /// that guards every conditional write this writable mount will ever issue against running under
    /// the disk's ~500-attempt transparent retry policy, a correctness hazard rather than a preflight
    /// convenience, so `Store::open` still runs it unconditionally whenever the mount is writable.
    bool skip_access_check = false;

    /// The CAS retry controller's budget (RFC cas-s3-timeout-retry-control), validated against
    /// `mount_lease_ttl_ms` at writable open (`Store::open` calls `validateCasRequestBudget`) — an
    /// inconsistent budget refuses the mount rather than silently retrying unsafely. Defaults are
    /// consistent with the default `mount_lease_ttl_ms` above; a caller that raises the lease TTL may
    /// keep these defaults, but a caller that LOWERS it must revisit this budget too.
    CasRequestBudget cas_request_budget{};

    /// The write-fence deadline clock (CLOCK_BOOTTIME milliseconds; see `MountFence`). Empty = the real
    /// boot clock (`Store::bootMs`); injected by tests to drive the fence deadline deterministically.
    std::function<uint64_t()> boot_ms_fn = {};

    /// rev.6 Task 6 (design §Late Predecessor PUT / §token-stability observation): the CONDITIONAL
    /// wait `Store::open` pays after reclaiming a mount whose predecessor's death was NOT proven clean
    /// (`MountClaimResult::prior` is `Fenced` or `UncleanObserved`) -- long enough for a still-in-flight
    /// conditional PUT from that predecessor to either land or be dropped by its own exhausted retry
    /// budget before this incarnation trusts its recovery listings. A `Clean` farewell (Task 5's drained
    /// dtor) already proves no such write can still be in flight, so it pays nothing; lowering this
    /// value increases the risk that a late-materializing predecessor write is dropped or observed
    /// inconsistently by a reader racing this incarnation's early mutations.
    uint64_t materialization_grace_ms = 30000;   /// T_mat; lowering increases the risk that a
                                                  /// late-materializing predecessor write is dropped
    /// Test hook for open/remount waits: `Store::waitSleep` -- the mount-claim observation loop's poll
    /// AND the `materialization_grace_ms` wait above -- routes through this function when set instead
    /// of a real `std::this_thread::sleep_for`, so a test observes every wait without actually
    /// blocking. Empty (the production default) sleeps for real.
    std::function<void(uint64_t)> wait_sleep_fn = {};   /// test hook for open/remount waits

    /// Task 11 (spec §writer-snapshot-publication): a table becomes a publish candidate once its
    /// PUBLISHABLE tail -- entries aged past `snapshot_min_log_age_ms` AND strictly above the newest
    /// published snapshot -- exceeds either threshold (their count / the sum of their encoded bytes),
    /// or right after recovery replays a tail already above one (the mount-time trigger). Publication
    /// is background and never blocks an append (see `Store::maybeScheduleSnapshotPublish`).
    /// The count default trades write-side PUT volume against read-side cold-fold cost: every publish
    /// re-encodes and PUTs the FULL snapshot, so a low threshold under sustained load degenerates into
    /// a near-continuous full-snapshot PUT stream, while on the read side a cold fold pays one GET per
    /// log the newest snapshot does not cover -- 256 bounds that at 256 extra GETs, each far cheaper
    /// than the snapshot churn it avoids.
    uint64_t snapshot_log_count_threshold = 256;
    uint64_t snapshot_log_bytes_threshold = 1ULL << 20;   /// 1 MiB
    /// Grace age (spec §Late Predecessor PUT): a candidate snapshot id `X` never covers a log younger
    /// than this many milliseconds -- documented wall-clock risk reduction for the cross-epoch race,
    /// not a proof. This writer times its OWN appends exactly (`Store::bootMsNow()` at commit); a
    /// mount-replayed (pre-existing) log's age is measured from this writer's OWN recovery-completion
    /// time instead of its true creation time, since this backend generation's `list()` carries no
    /// per-key last-modified timestamp (`ListedKey` has none) -- conservative: it can only DELAY a
    /// replayed log's eligibility, never advance it prematurely.
    uint64_t snapshot_min_log_age_ms = 5000;
    /// C4 (spec §writer-snapshot-publication): bounded per-table backoff arming a dispatch cooldown after
    /// a NON-Committed publish outcome (an S3 timeout / uncertain PUT). Without it, a saturated backend
    /// turns every ref read into a re-dispatched full-snapshot encode+PUT (the read-triggered PUT storm),
    /// since a non-Committed publish deliberately does not prune the tail (that would be data loss) and so
    /// leaves the threshold trigger latched. The interval doubles from `initial` up to `max` per
    /// consecutive failure and resets on the next durable publish; combined with the single-in-flight gate
    /// and the candidate-advance skip, it bounds publish dispatch to O(failures), not O(reads).
    uint64_t snapshot_publish_backoff_initial_ms = 200;
    uint64_t snapshot_publish_backoff_max_ms = 30000;
    /// S13 fix (DANGLING-PRECOMMIT re-opened; triage `.superpowers/sdd/s13-triage-report.md`): bounded
    /// per-table cooldown between FAILED stale-precommit sweep attempts. A failed/partial sweep re-arms
    /// `needs_stale_precommit_sweep` instead of consuming the once-per-mount shot (one attempt burned in
    /// the post-restart error window used to leave a dead incarnation's precommit bindings -- and the
    /// manifests they protect from the GC orphan sweep -- live forever on a long-lived mount); this
    /// cooldown keeps the retry from storming a saturated backend, exactly like the C4 publish backoff.
    /// Doubles from `initial` up to `max` per consecutive failure; reset by a verified-clean sweep.
    uint64_t precommit_sweep_backoff_initial_ms = 200;
    uint64_t precommit_sweep_backoff_max_ms = 30000;

    /// Task 13 (spec §Byte, Memory, And CPU Budget): resident-memory ceiling for the writer's
    /// whole-table ref cache (`Store::ref_tables`). Phase 1 has no row overlay, so eviction is
    /// WHOLE-TABLE: when the summed estimated weight of cached tables exceeds this, whole tables are
    /// dropped (never rows) and the next touch re-recovers them from the durable snapshot+log objects
    /// (spec §Startup And Recovery: "Evicting the table drops the entire object; the next access repeats
    /// recovery"). A table with a wedged append lane, a nonempty pending queue, or any in-flight
    /// caller/publish (its un-persisted lane/queue state is not reconstructable) is never evicted, and
    /// neither is the table whose recovery just triggered the pass -- so the effective floor is one
    /// table. 0 = unbounded (eviction disabled). The estimate is the base snapshot body size plus the
    /// retained log-tail bytes; both are already tracked, so a mutation costs no extra encode.
    uint64_t ref_table_cache_bytes = 256ULL << 20;   /// 256 MiB
};

struct Resolved
{
    /// The namespace-qualified identity of the part manifest this ref names. The owning RootNamespace
    /// + the ref's manifest_ref form the ManifestId (the ref carries no namespace itself — that comes
    /// from the owning root context, spec §Object Identity And Ownership).
    ManifestId manifest_id;
    uint64_t manifest_size = 0;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};

/// The `{mutable_files, published_at_ms}` carrier `updateRefPayload`'s mutator edits in place: the one
/// set_payload transaction replaces the complete mutable payload without touching the manifest edge, so
/// the mutator has no way to express (and no need to reject) a `manifest_ref` change -- a reachability
/// change goes through publish/drop instead.
struct RefMutableFilesUpdate
{
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};

/// Task 10: the deterministic encoding of a committed ref's mutable payload (`Resolved::mutable_files` /
/// `RefMutableFilesUpdate::mutable_files`) inside one `RefCommittedRow.payload` -- an OPAQUE blob from the
/// ref-log/snapshot codecs' point of view (spec §Snapshot Format: "mutable_files: deterministic ref
/// payload"). Shared by `Store`'s own dropRef/updateRefPayload/resolveRef/listRefs and by `Build::promote`
/// (the `set_payload` op that installs a precommit's initial payload). Defined in CasStore.cpp.
String encodeMutableFilesPayload(const std::map<String, String> & files);
std::map<String, String> decodeMutableFilesPayload(const String & payload);

struct BlobLocation
{
    String key;
    uint64_t offset = 0;                      /// payload start within the object
    uint64_t length = 0;
};

struct BuildInfo
{
    std::optional<String> intended_ref;       /// "ns/ref" forensics for the envelope (diagnostic)
    /// The owning root namespace, set EXPLICITLY by the wiring. When present it is authoritative for
    /// the manifest's owning namespace (Build::manifestNamespace), so a ref that itself contains '/'
    /// (the `detached/<part>` fold, B181) is staged in the TABLE namespace — NOT in a spurious
    /// `<ns>/detached` namespace produced by splitting intended_ref on the last '/'. Absent ⇒ fall
    /// back to splitting intended_ref on the last '/' (the diagnostic-only path used by Core tests).
    std::optional<RootNamespace> intended_namespace;
    ProvenanceOp op = ProvenanceOp::Other;
};

class Build;
using BuildPtr = std::shared_ptr<Build>;
class Gc;
class Store;
using StorePtr = std::shared_ptr<Store>;

/// Local write fence (spec §write-fence, Phase 0 Task 6). A PURELY LOCAL, in-memory check — never a
/// per-write S3 read. The renewer (the MountLeaseKeeper, Task 7) is the only thing that touches S3 for
/// the lease; on each successful renew it refreshes `deadline_boot_ms` (an S3 `expires_at_ms`
/// translated to a CLOCK_BOOTTIME instant, so a wall-clock change cannot extend it), and on any
/// supersession (a foreign uuid / a newer writer_epoch / an unrenewable expired lease) it latches
/// `lost`. A mutable op proceeds only while `!lost` AND the deadline has not passed. `writer_epoch` is
/// the fencing token.
///
/// WHY BOOTTIME, NOT MONOTONIC: `CLOCK_MONOTONIC` does not advance while a VM is suspended, so a
/// resumed sleeper would compute the same "not yet expired" verdict it had before the nap even though
/// wall time (and the GC leader's fence-out) moved far ahead — it could mutate the shared state under
/// a live writer. `CLOCK_BOOTTIME` includes suspend time, so a resumed sleeper sees its fence expired.
/// Container pause is already safe under either clock (the process is frozen, so no local check runs).
struct MountFence
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    /// Permissive default (set in the Store ctor): until something arms a real lease deadline, the
    /// fence allows mutations — so existing tests and pre-Task-7 behavior are unchanged. UINT64_MAX =
    /// unarmed (never expires); otherwise a CLOCK_BOOTTIME-milliseconds instant.
    std::atomic<uint64_t> deadline_boot_ms{std::numeric_limits<uint64_t>::max()};
    std::atomic<bool> lost{false};
};

/// One content-addressed pool. open is FAIL-CLOSED: capability probe + pool-format check; any
/// failure refuses the pool (design §6). The read side has no GC awareness and no tokens (spec §6).
class Store : public std::enable_shared_from_this<Store>
{
    /// Build drives ref mutations through the private appendRefOps lane: the gate logic
    /// (W-PUBLISH-GATE) is Build's responsibility (it owns the per-hash freshness-meta point-read), but
    /// the append/commit loop itself is the verified Store loop — reused, never duplicated.
    friend class Build;
    /// Gc reaches the ref-log lane and Store internals through the public accessors and this friendship —
    /// reused, never duplicated.
    friend class Gc;

public:
    static StorePtr open(BackendPtr backend, PoolConfig config);
    ~Store();

    /// ---- per-server watermark surface (spec 2026-06-16-ca-build-watermark) ----
    /// process_epoch: random nonzero per Store (process). GC checks epoch EQUALITY, never ordering.
    uint64_t epoch() const { return process_epoch.load(std::memory_order_acquire); }
    /// The durable-monotone writer_epoch allocated at writable open (spec §writer-epoch-alloc). On a
    /// writable Store this is the value bridged into `process_epoch` (so the watermark + the manifest
    /// manifest ref carries it); on a read-only open the random `process_epoch` is unchanged and
    /// no durable epoch is allocated. A self-remount re-establishes this to the fresh incarnation's
    /// writer_epoch (kept equal to `liveWriterEpoch`). Phase 2's epoch-aware sweep reads this value.
    uint64_t writerEpoch() const { return process_epoch.load(std::memory_order_acquire); }
    /// The GC floor: the oldest in-flight build_seq, or next_build_seq when no build is active (so a
    /// quiescent server's watermark floor advances to the next-to-be-allocated seq). Locks builds_mutex.
    uint64_t minActive();
    /// Test/assertion accessor for the next-to-allocate build_seq under the lock.
    uint64_t peekNextBuildSeq();
    /// Renew the merged heartbeat once (bump seq, refresh min_active from the live callback, stamp a
    /// fresh expires_at_ms). The build-watermark floor rides this beat. In production this is driven by
    /// the background renewer (background_watermark).
    void renewWatermarkOnce();

    /// ---- local write fence (spec §write-fence, Phase 0 Task 6) ----
    /// A purely local, in-memory check — NEVER a per-write S3 read. True iff the fence has not latched
    /// `lost` and the monotonic deadline has not passed. Permissive until armed: a Store that has not
    /// armed the fence (the default deadline is steady_clock::time_point::max()) always allows mutations.
    bool mayMutate() const;
    /// Latch the fence to lost (once lost, stays lost). Called by the renewer (Task 7) on a superseded
    /// or foreign observation; the gated mutate chokepoints then fail closed.
    void tripMountLost();
    /// Refresh the write-fence deadline (a CLOCK_BOOTTIME-milliseconds instant; release). Task 7's
    /// keeper renew calls this on success.
    void setMountDeadline(uint64_t deadline_boot_ms);
    /// Arm the fence at startup (Task 7): set (uuid, epoch, deadline), clear `lost`.
    void armMountFence(UInt128 server_uuid, uint64_t writer_epoch, uint64_t deadline_boot_ms);
    /// The fence clock: CLOCK_BOOTTIME in milliseconds (includes VM-suspend time, unlike
    /// CLOCK_MONOTONIC — see `MountFence`). Consults the injected `config.boot_ms_fn` if set (tests),
    /// otherwise `bootMs`.
    uint64_t bootMsNow() const;
    /// The real boot clock: CLOCK_BOOTTIME in milliseconds. Static so tests can compose it.
    static uint64_t bootMs();

    /// ---- write side ----
    BuildPtr startBuild(BuildInfo info);                          /// W-HEARTBEAT durable before return

    /// ---- read side (spec §6) ----
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false);
    /// Read the single immutable part manifest named by `id`. Derives the key via CasLayout::manifestKey,
    /// decodes the body, and fails CLOSED: a committed ref naming a missing body throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE surfaced on the read path); a body whose `ref` ≠ id.ref (refMatchesBody) or whose
    /// `root_namespace_id` ≠ id.root_namespace (manifestNamespaceMatches) throws CORRUPTED_DATA — the
    /// ref is addressing the wrong object, or a cross-namespace dangle. Token-gated decode cache below.
    PartManifest readManifest(const ManifestId & id);
    /// Identical to `readManifest` (same mandatory HEAD, same fail-closed validation, same decode
    /// cache) but returns the SHARED immutable decode the manifest cache holds — no per-call copy.
    /// The wiring read path uses this variant (spec 2026-07-08-cas-part-folder-cache).
    std::shared_ptr<const PartManifest> readManifestShared(const ManifestId & id);
    BlobLocation locate(const ManifestEntry & entry) const;       /// Blob placement only
    std::map<String, Resolved> listRefs(const RootNamespace & ns);
    /// Namespaces with the given prefix: a LIST of `cas/refs/` ∪ `roots/`, results are UNORDERED.
    /// A dropped namespace's ref-shard objects linger until GC reclaims them (Task 6).
    std::vector<String> listNamespaces(const String & prefix);

    /// Scoped LIST of the mirrored subtree (design §5.3): the distinct next-path-segment names under
    /// `roots/<prefix>` (a loose LIST used by browse only; callers re-check `listRefs`/`getFileSize`
    /// before showing an entry). NOT authoritative — GC uses LIST-based discovery (`cas/refs/`). `prefix`
    /// is a server-relative or shadow-relative path ending in '/'.
    std::vector<String> listMirroredChildren(const String & prefix);

    /// ---- ref lifecycle (Task 10: persisted on the snapshot+log protocol, spec §Writer Algorithms) ----
    void dropRef(const RootNamespace & ns, const String & ref_name);            /// one owner_transition removal txn
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefMutableFilesUpdate &)> mutator);   /// one set_payload txn
    /// Task 11 (spec §Namespace Removal): one ref-log transaction naming every owner's exact removal
    /// followed by `remove_namespace`, then a best-effort publish of the constant-size `Removed`
    /// snapshot. Performs NO physical deletion (no verbatim-file deletes, no tombstones) -- that is
    /// GC's namespace-cleanup item, Task 12.
    void dropNamespace(const RootNamespace & ns);

    /// True iff this namespace's ref-table lifecycle is durably `Removed` — a table that `dropNamespace`
    /// removed and that has NOT been recreated (distinguished from a never-born namespace, whose default
    /// `Removed` lifecycle carries no `remove_txn_id`). Recovers a cold runtime; the warm path is a
    /// cached-state read. Readers consult this to treat a dropped table's namespace files as absent while
    /// GC has not yet physically reclaimed them (deferred-GC removal, Task 12); a never-born namespace is
    /// NOT reported removed (fail-closed — only a KNOWN-removed table hides its files). A SAME-namespace
    /// recreation flips this back to `false` only once its first ref op forces a fresh `namespace_birth`
    /// — unreachable under normal `Atomic` DDL (a recreated table always mints a fresh UUID, hence a
    /// fresh namespace), and itself gated on this namespace's `_cleanup` marker (spec §Namespace Birth).
    bool namespaceIsRemoved(const RootNamespace & ns);

    /// ==== writer ref-log append lane (Task 10, spec §Writer Algorithms) ====
    ///
    /// The ONE entry point every ref mutation funnels through -- Store's own dropRef/updateRefPayload
    /// above, and (as a friend) Build's precommitAdd/promote/abandon. This is the SOLE ref-persistence
    /// lane now: the legacy per-(ns,shard) mutable manifest format was removed once GC/sweep/fsck/inspect
    /// were rewired onto the snapshot+log ref protocol (Task 12).
    ///
    /// `build_ops(state)` is invoked from the per-namespace flush leader with the table's CURRENT cached
    /// state (reflecting every earlier item of the SAME batch already applied) -- exactly the atomicity
    /// the old per-shard closure got from running inside the shard's own CAS loop. It may perform
    /// arbitrary caller-side I/O (Build's blob revalidation) and throw to reject ONLY this item; a
    /// LOGICAL_ERROR/ABORTED/etc it throws propagates to the item's own caller without touching any
    /// other queued item. It returns the ops this call contributes to the batch's one transaction.
    /// `scope` reuses `MutationScope` (Ref(name) may co-batch; WholeShard runs solo -- used here for
    /// `namespace_birth`, which the flush forces automatically whenever the cached state is not `Live`).
    ///
    /// Wedge semantics (spec §Writer-Side Linearization): at most one unresolved `PUT` per table. An
    /// `Unresolved` outcome wedges this namespace's lane -- no later id is allocated until the SAME
    /// (key, bytes) resolves durable (applied to cache before the next id), or the process unmounts.
    /// Every item in the failing batch receives the SAME uncertainty exception (ABORTED); items already
    /// wedged from an EARLIER flush are retried by the NEXT call into this namespace's queue.
    RefTxnId appendRefOps(const RootNamespace & ns, MutationScope scope,
                         std::function<std::vector<RefOp>(const RefTableState &)> build_ops,
                         RootMutationOrigin origin, RootMutationKind kind);

    /// Task 11 (spec §Namespace Birth): whether namespace `ns`'s recovery observed the exact
    /// `_cleanup/<remove_txn_id>` completion marker for `remove_txn_id` -- the SOLE gate on recreating
    /// a `Removed` namespace (an empty physical prefix is never sufficient). Recovers the table
    /// (idempotent) if not already cached. Used by `Build::precommitAdd`'s auto-birth guard.
    bool observedNamespaceCleanupMarker(const RootNamespace & ns, const RefTxnId & remove_txn_id);

    /// Task 11 (spec §writer-snapshot-publication): the synchronous core of one publish attempt --
    /// picks the newest grace-age-eligible candidate id `X` from the retained tail, encodes
    /// `Replay(snapshot_base_state, tail through X)`, and `putIfAbsentControlled`s it. Returns true iff
    /// a NEW snapshot was confirmed durable this call (false covers "nothing eligible yet", "nothing
    /// new to cover", and every non-Committed outcome -- all harmless per the Failure Handling table:
    /// "Snapshot create fails: keep all logs; writer recovery remains unchanged"). Public so tests can
    /// drive one attempt deterministically without depending on the background dispatch's timing;
    /// production reaches it only through `maybeScheduleSnapshotPublish`.
    bool trySnapshotPublishOnce(const RootNamespace & ns);

    /// ---- verbatim namespace files (format_version.txt, ...) — plain keys, never content-addressed ----
    void putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes);
    std::optional<String> getNamespaceFile(const RootNamespace & ns, const String & name);
    std::vector<String> listNamespaceFiles(const RootNamespace & ns);
    /// Exact-token delete of one verbatim file (no-op when absent). Verbatim files are never
    /// content-addressed, so a mid-life delete (a pruned mutation entry, a stale tmp) must reclaim
    /// the object NOW - the reachability GC never scans them.
    void removeNamespaceFile(const RootNamespace & ns, const String & name);

    /// ---- plain mountpoint objects (loose, non-content-addressed disk files; design §5.2) ----
    /// A loose disk file (the startup write probe; anything written outside a `@cas@` archive) is a
    /// plain object at its mirrored path `roots/<key>`. No manifest, no journal, no dedup. GC never
    /// scans these (it deletes only content and folds only registered namespaces); they are owned by
    /// their path and removed only by `removeMountpointObject`.
    void putMountpointObject(const String & key, const String & bytes);
    std::optional<String> getMountpointObject(const String & key);
    /// Existence check for a loose mountpoint object WITHOUT reading its body. Directory-safe: a HEAD
    /// routes through the backend's metadata path (B38: a directory reports as not-an-object), so probing
    /// a directory-shaped pool path (e.g. `store`, system.remote_data_paths traversal) returns false
    /// instead of a body read that would throw "Is a directory" (EISDIR).
    bool mountpointObjectExists(const String & key);
    void removeMountpointObject(const String & key);

    /// Internal surface for Build (same TU family; not for the wiring):
    const PoolConfig & poolConfig() const { return config; }
    const PoolMeta & poolMeta() const { return meta; }
    const Layout & layout() const { return pool_layout; }
    Backend & backend() { return *pool_backend; }

    /// CAS mixed-algo pools (Phase 3 T4/T5, design 2026-07-11-cas-mixed-algo-pools-design.md §5):
    /// the NODE-LOCAL algo this Store mints NEW content with (`PoolConfig::blob_hash_algo` -- never
    /// durable pool state, see the field comment). Every write-mint site uses this, never a bare
    /// `poolMeta()` field (the pool no longer records one truthful write algo).
    BlobHashAlgo writeAlgo() const { return config.blob_hash_algo; }

    /// Whether `algo` is a member of the pool's `algos_used`, per this Store's MONOTONE in-memory
    /// cache (seeded from `algos_used` at open time, unioned by `refreshAdmittedAlgos` -- never
    /// shrinks). This is the validation-protocol fast path (spec §5.1): a hit needs no I/O. A miss
    /// for an algo this build KNOWS about must be followed by `refreshAdmittedAlgos()` before
    /// concluding the algo is genuinely not admitted (a long-running fold can overlap a later
    /// registration by another node) -- callers at the manifest-read boundary do this (Task 5).
    bool isAlgoAdmitted(BlobHashAlgo algo) const;

    /// Re-reads `_pool_meta` and unions its CURRENT `algos_used` into the in-memory admitted-algo
    /// cache (mutex-guarded; monotone -- a concurrent shrink is impossible since `algos_used` is
    /// itself append-only). Returns the refreshed cache as a sorted vector, for callers that want to
    /// render it (error messages, diagnostics). THE stale-cache-race fix (spec §5.1/§9.8): call this
    /// on every admission-check miss, not just once at open.
    std::vector<uint8_t> refreshAdmittedAlgos();

    /// The writer_epoch of the LIVE mount incarnation. Bumped by `tryRemountOnce` (self-remount
    /// after a GC fence-out) — a `Build` minted under an older epoch fails closed on its next step.
    uint64_t liveWriterEpoch() const { return live_writer_epoch.load(std::memory_order_acquire); }

    /// Self-remount after a GC fence-out (liveness counterpart of the fence-out safety rule): the
    /// OLD incarnation may never write again (the keeper never re-mints), but a FRESH incarnation —
    /// durable writer_epoch bump + mount reclaim + re-armed write fence — is exactly what a server
    /// restart would create, so a live server may create it in place. Runs the same claim machinery as
    /// `Store::open` (S13). Returns false (and changes nothing durable beyond the epoch bump) when the
    /// mount cannot be claimed (foreign owner / a genuinely live twin) — the caller retries. Safe to
    /// call concurrently (serialized internally); also the synchronous test seam.
    bool tryRemountOnce();

    /// Test seam: drive the (private) self-remount arm/refuse path directly — in production the
    /// keeper's on_lost callback calls `scheduleRemount`, otherwise reachable only via the background
    /// renewer's cadence. Returns true iff a recovery thread is armed after the call.
    bool scheduleRemountForTest();
    /// Test seam: latch `remount_shutting_down` exactly as `~Store()` does at its top, WITHOUT tearing
    /// the Store down, so a test can assert `scheduleRemount` refuses to spawn once teardown has begun.
    void beginShutdownForTest();

    /// rev.6 Task 6: sticky once a writable `open` reclaimed a mount over an unclean predecessor
    /// (`MountPriorState::Fenced` or `UncleanObserved` -- see `materialization_grace_ms`). Never
    /// cleared for the life of this incarnation. Task 8 reads the underlying flag; this is the test seam.
    bool uncleanEpochBoundarySeenForTest() const { return unclean_epoch_boundary_seen.load(std::memory_order_relaxed); }

    /// P1 known-present blob-hash cache (design 2026-06-20, B168). A HINT only — correctness never
    /// depends on it: a hit just makes putBlob go HEAD-first, and a stale hit is caught by that HEAD.
    /// No-ops when disabled (dedup_cache_bytes == 0). Keyed on the full `BlobRef` pair (Phase 3 T2):
    /// a bare digest is never the blob identity, and the same digest value under two algos is two
    /// different objects.
    bool dedupCacheContains(const BlobRef & ref) const;
    void dedupCacheAdd(const BlobRef & ref);
    /// Test seam: retained bytes of the manifest decode cache (0 when disabled).
    size_t manifestDecodeCacheBytesForTest() const { return manifest_cache ? manifest_cache->sizeInBytes() : 0; }

    /// ---- B170 event audit (system.content_addressed_log) ----
    /// The wiring injects a sink (CasEvent -> SystemLog row) when the log is configured; null sink
    /// (unit tests, log disabled) makes emitEvent a no-op single branch. Build/Gc reach this via
    /// their owning Store. `reason`/`detail` on the event carry the decision's full rationale.
    void setEventSink(CasEventSink sink) { event_sink_ = std::move(sink); }
    void emitEvent(const CasEvent & e) const { if (event_sink_) event_sink_(e); }
    /// Cheap predicate so query-frequency hooks can skip constructing the CasEvent (+ its detail map)
    /// entirely when the log is disabled (sink null) — a true no-op on the production hot path.
    bool hasEventSink() const noexcept { return static_cast<bool>(event_sink_); }

    /// Task 5: read the current GC round from `gc/state`. Returns 0 when `gc/state` is absent (pool
    /// never GC'd). Best-effort: its one remaining caller is `tryRemountOnce`'s MountRemount audit
    /// event, which reports round 0 on any read failure rather than let the error escalate.
    uint64_t currentGcRound() const;

private:

    Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_);

    CasEventSink event_sink_;   /// B170: null = disabled (emitEvent no-op)

    /// Allocate a strictly-increasing build_seq and add it to the active set (called by startBuild).
    uint64_t allocateBuildSeq();
    /// Remove a build_seq from the active set; idempotent (safe from publish/abandon/dtor).
    void retireBuildSeq(uint64_t seq);

    /// ---- plain-object CAS helpers (shared by namespace-file and mountpoint-object paths) ----
    /// head + putIfAbsent/putOverwrite loop (bounded by MAX_CAS_ATTEMPTS); throws ABORTED on live-lock.
    void casPutObject(const String & full_key, const String & bytes);
    /// Plain get → bytes; returns nullopt when absent.
    std::optional<String> casGetObject(const String & full_key);
    /// head + deleteExact loop; no-op when absent. Throws ABORTED on live-lock.
    void casRemoveObject(const String & full_key);

    /// ==== writer ref-log append lane (Task 10, spec §Writer Algorithms / §Local Batching Queue) ====

    /// At most one outstanding uncertain `PUT` per table (spec §Writer-Side Linearization). Retained
    /// on the table's runtime until resolved durable (applied to cache first) or definitely rejected.
    struct RefAppendWedge
    {
        RefTxnId txn_id;
        String key;
        String bytes;
    };

    /// One queued `appendRefOps` caller. `build_ops` is invoked at most once, from inside the flush,
    /// and returns the ops it contributes rather than mutating storage directly.
    struct RefMutationItem
    {
        MutationScope scope;
        std::function<std::vector<RefOp>(const RefTableState &)> build_ops;
        RootMutationOrigin origin = RootMutationOrigin::Writer;
        RootMutationKind kind = RootMutationKind::Publish;
        bool done = false;                       /// guarded by ref_queue_mutex
        std::exception_ptr error;                /// guarded by ref_queue_mutex
        RefTxnId committed_id{};                  /// written by the leader before done = true
    };

    /// The whole-table runtime (spec §Startup And Recovery / §Table State): one coherent decoded
    /// `RefTableState` per namespace, evicted only as a whole (never populated here -- Phase 1 has no
    /// eviction trigger yet, only lazy recovery on first touch). `state_mutex` is SEPARATE from
    /// `ref_queue_mutex` (which only ever guards `pending`/`leader_active`) so a reader (resolveRef/
    /// listRefs) can observe `state` without contending with the flush leader's network round trip --
    /// the leader only holds `state_mutex` for the brief copy-out-before-validate and the
    /// apply-after-commit steps, never for the `putIfAbsentControlled` call itself.
    struct RefTableRuntime
    {
        std::mutex state_mutex;
        bool recovered = false;
        RefTableState state;
        /// Retained `_cleanup/<remove-txn-id>` markers observed at recovery (Task 11's recreation
        /// gate consumes these via `observedNamespaceCleanupMarker`).
        std::set<RefTxnId> cleanup_markers;
        std::optional<RefAppendWedge> wedge;
        uint64_t recovery_restarts = 0;           /// diagnostic: LIST/GET restarts forced by a vanished object
        /// Per-table admission budgets (spec §Snapshot Format): the raw hard limits minus this table's
        /// own `4 + ns.size()` wire overhead and a fixed safety margin, computed once at recovery.
        uint64_t snapshot_budget = 0;
        uint64_t removal_budget = 0;

        /// Task 11 (spec §writer-snapshot-publication): one applied txn strictly above
        /// `newest_snapshot_id`, retained so a grace-age-eligible candidate `X <= greatest_applied` can
        /// be replayed from `snapshot_base_state` without re-fetching anything. `observed_at_ms` is
        /// this writer's own commit time (own appends) or the table's recovery-completion time
        /// (mount-replayed logs -- see `PoolConfig::snapshot_min_log_age_ms`); `encoded_bytes` avoids
        /// re-encoding on prune. Pruned up through `X` once a snapshot covering `X` is confirmed durable.
        struct TailLogEntry
        {
            RefLogTxn txn;
            uint64_t observed_at_ms = 0;
            uint64_t encoded_bytes = 0;
        };
        std::vector<TailLogEntry> tail_since_snapshot;
        /// ATOMIC (relaxed): `enforceRefTableCacheBudget`'s pre-candidacy `total` loop sums the weight of
        /// EVERY cached table under `ref_queue_mutex` alone -- including hot tables whose append lane
        /// (holding only `state_mutex`) is concurrently mutating this field. That cross-lock read is a
        /// data race on a plain `uint64_t` (formal UB, TSan-detectable); an atomic makes it well-defined.
        /// The gated candidate loop and every other reader/writer already hold `state_mutex`, so relaxed
        /// ordering is sufficient (this counter carries no happens-before for other state).
        std::atomic<uint64_t> tail_bytes_since_snapshot{0};
        /// The state as of `newest_snapshot_id` (the never-born/empty state if this table has never had
        /// a snapshot published) -- the base every `tail_since_snapshot` entry replays forward from.
        RefTableState snapshot_base_state;
        std::optional<RefTxnId> newest_snapshot_id;
        /// Task 13 (spec §Byte, Memory, And CPU Budget): whole-table cache-weight bookkeeping for
        /// `enforceRefTableCacheBudget`. `base_snapshot_bytes` is the encoded body size of
        /// `snapshot_base_state`'s snapshot (0 for a never-born base), captured for free from the
        /// recovered/published snapshot body -- refreshed only when the base changes (recovery + each
        /// publish), never per mutation. The estimated resident weight is
        /// `base_snapshot_bytes + tail_bytes_since_snapshot`. `base_snapshot_bytes` is ATOMIC (relaxed)
        /// for the same cross-lock `total`-loop read as `tail_bytes_since_snapshot` above. `last_touch_tick`
        /// is the monotonic access stamp (`Store::ref_table_access_tick`) used to evict least-recently-
        /// touched tables first; it is read only in the `use_count()==1`-gated candidate loop (no
        /// concurrent writer there), so it stays a plain `uint64_t`.
        std::atomic<uint64_t> base_snapshot_bytes{0};
        uint64_t last_touch_tick = 0;
        /// Set true by recovery (spec §Clean Up Old Precommits); cleared when a sweep attempt is
        /// dispatched (so the sweep's own nested `appendRefOps` calls do not recurse) and PERMANENTLY
        /// only once an attempt completes VERIFIED CLEAN (a full pass over the live state found zero
        /// stale bindings). S13 fix: any failed/partial attempt re-arms it (with the
        /// `precommit_sweep_backoff_*` cooldown), so a later read/mutation trigger retries until clean --
        /// a single attempt burned in the post-restart error window must not leave a dead incarnation's
        /// precommit bindings protected from GC forever on a long-lived mount.
        bool needs_stale_precommit_sweep = false;
        /// S13 fix: per-table retry cooldown for the stale-precommit sweep (guarded by `state_mutex`),
        /// mirroring `publish_backoff_*` below: `until` is the boottime instant before which
        /// `maybeSweepStalePrecommits` refuses to re-attempt; `ms` is the current exponential interval
        /// (0 = no failure yet, or reset by the last verified-clean sweep).
        uint64_t precommit_sweep_backoff_until_ms = 0;
        uint64_t precommit_sweep_backoff_ms = 0;
        /// Test-observability + graceful settling for the background snapshot-publish dispatch (see
        /// `maybeScheduleSnapshotPublish`): the count of in-flight publish attempts for this table, and
        /// the condvar (guarded by `state_mutex`) a test waits on via `waitForSnapshotPublishSettleForTest`.
        std::atomic<int> pending_snapshot_publishes{0};
        std::condition_variable publish_settle_cv;
        /// C4 (spec §writer-snapshot-publication): per-table publish-dispatch backoff (guarded by
        /// `state_mutex`). `publish_backoff_until_ms` is the boottime instant before which
        /// `maybeScheduleSnapshotPublish` refuses to dispatch; `publish_backoff_ms` is the current
        /// exponential interval (0 = not backing off), doubled on each consecutive non-Committed publish
        /// outcome and reset to 0 on the next durable publish.
        uint64_t publish_backoff_until_ms = 0;
        uint64_t publish_backoff_ms = 0;

        std::deque<std::shared_ptr<RefMutationItem>> pending;    /// guarded by ref_queue_mutex
        bool leader_active = false;                               /// guarded by ref_queue_mutex
        std::condition_variable cv;

        /// Set true when a self-remount detaches this runtime from the cache (`quiesceRefTablesForRemount`):
        /// the fresh incarnation re-recovers each table under the new epoch on next touch, so any leader
        /// still holding THIS (now-orphaned) runtime must fail closed instead of allocating an id / applying
        /// against its stale cache once the re-armed fence re-opens the gate. Stored with release BEFORE the
        /// remount re-arms the fence, so a lane that observes `mayMutate` true also observes this flag
        /// (release/acquire through the fence) -- there is no interleaving where a stale runtime both passes
        /// the fence and reads this flag false.
        std::atomic<bool> superseded_by_remount{false};
    };
    static constexpr size_t kMaxRefBatch = 128;
    static constexpr size_t kRefRecoveryMaxRestarts = 3;          /// spec: "bounded (3) and counted"
    /// rev.6 Task 8 (spec §recovery-seal): a failed recovery-seal PUT (`putIfAbsentControlled` not
    /// `Committed`) is a SEPARATE fail-closed throw -- it is not one of the restarts counted here
    /// (those are only LIST/GET restart-on-vanish loops). It leaves `rt.recovered` false, so the table
    /// stays unrecovered/non-writable and the NEXT touch restarts recovery from a fresh attempt 0
    /// (re-LIST, re-replay, re-seal), never resuming this bounded loop mid-way.
    /// Fixed Phase-1 safety margin subtracted (alongside the per-table `4 + ns.size()` overhead) from
    /// the raw `ref_snapshot_max_bytes`/`ref_removal_max_bytes` hard limits before calling `admits`.
    static constexpr uint64_t kRefAdmissionSafetyMargin = 4096;

    std::mutex ref_queue_mutex;
    std::map<String, std::shared_ptr<RefTableRuntime>> ref_tables;
    /// Task 13 (spec §Byte, Memory, And CPU Budget): monotonic access stamp for whole-table cache LRU
    /// eviction; bumped on every table touch, recorded in `RefTableRuntime::last_touch_tick`.
    std::atomic<uint64_t> ref_table_access_tick{0};
    /// rev.6 Task 5 (spec §clean-release drain): latched by `drainRefLanesForShutdown` BEFORE it
    /// snapshots `ref_tables`/waits on each table's queue -- every ordinary ref mutation (`appendRefOps`)
    /// checks this under the SAME `ref_queue_mutex` critical section it uses to enqueue its item, so the
    /// check-and-enqueue is atomic with the drain's snapshot-and-wait: a caller either enqueues strictly
    /// before the drain observes this table (and the drain then waits for it), or observes this flag
    /// already true and never enqueues at all. No caller can land a NEW item after the drain has decided
    /// this table is idle.
    std::atomic<bool> shutting_down{false};

    /// Store-wide strictly-increasing counter (spec §Ordered Ref Transaction Identifier): shared by
    /// every table of this mounted writer; a fresh writer_epoch (a new Store) restarts it at one.
    std::atomic<uint64_t> next_ref_sequence{1};
    /// The epoch component is the LIVE mount incarnation's writer_epoch, not the open-time
    /// `process_epoch`: a self-remount allocates a strictly-greater durable writer_epoch, so every ref
    /// transaction stamped after the remount sorts strictly ABOVE any (dead-incarnation or twin) log
    /// still durable under an older epoch. `RefTxnId` compares epoch first, so the epoch bump alone
    /// guarantees the pagination premise "a new log is never inserted at or below an already durable
    /// table log id" (spec §Ordered Ref Transaction Identifier / §Step 1).
    RefTxnId allocateRefTxnId() { return RefTxnId{liveWriterEpoch(), next_ref_sequence.fetch_add(1)}; }

    /// The CAS-owned retry controller (Task 5) this Store's ref-log writer path uses for every
    /// conditional log/snapshot `PUT` and uncertain-result resolution. Also shared (via the Build
    /// friendship) by `Build::stageManifest`'s part-manifest body `PUT` (chaos-tolerance-report
    /// §Task B) and by `Build::uploadFromSource`'s blob-body create — both the streaming
    /// `putIfAbsentStream` PUT and `promoteStaged`'s conditional server-side copy — via
    /// `conditionalCreateControlled` (availfix). The controller is stateless per call (immutable
    /// budget/clock/sleep — the sleep fn mutates only through the test-only seam, before traffic), so
    /// concurrent lanes and builds use the one instance safely.
    std::unique_ptr<CasRequestController> ref_request_controller;

    /// RFC pre-attempt fence check: extends `mayMutate` with the REMAINING budget check -- an attempt
    /// is not even started unless there is enough of the mount lease left for one more attempt_timeout
    /// plus the lease safety margin. Passed as `fence_ok` to every `CasRequestController` call the
    /// ref-log writer path makes.
    bool refAppendFenceOk() const;

    /// Get-or-create the namespace's runtime (map access only; does not recover). `ensureRefTableRecovered`
    /// performs the actual one-`LIST`-plus-tail-`GET`s recovery, idempotently, under `rt->state_mutex`.
    std::shared_ptr<RefTableRuntime> getRefTableRuntime(const RootNamespace & ns);
    void ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt);

    /// Task 13 (spec §Byte, Memory, And CPU Budget): whole-table cache-budget enforcement, called right
    /// after a NEW table is materialized in `ensureRefTableRecovered`. When the summed estimated weight
    /// of cached tables exceeds `config.ref_table_cache_bytes` (0 = disabled), it drops whole tables,
    /// least-recently-touched first, until back under budget. A table is evictable only when it is
    /// IDLE -- its map slot holds the sole `shared_ptr` (`use_count() == 1`, so no in-flight caller,
    /// queued append, or background publish holds it), it has no active queue leader or pending item,
    /// and (checked under its own `state_mutex`) no wedged append lane. `keep_ns` (the table whose
    /// recovery triggered this pass) is never evicted. The `use_count() == 1` gate is the load-bearing
    /// one: it makes eviction of a runtime any caller currently holds impossible, so the ref-append
    /// lane can never split-brain across an old (evicted) and a fresh runtime for one table.
    void enforceRefTableCacheBudget(const RootNamespace & keep_ns);

    /// Self-remount re-incarnation (spec §Startup And Recovery / §write-fence): drain in-flight
    /// background publishers, mark every cached table `superseded_by_remount`, and drop the runtimes so
    /// the fresh incarnation re-recovers each table under the new `live_writer_epoch` on next touch. Runs
    /// on the remount path while the fence is still lost and AFTER `live_writer_epoch` is bumped but
    /// BEFORE the fence is re-armed, so no append allocates an id and no publisher commits from a stale
    /// cache across the swap. Any in-memory wedge is discarded with its runtime -- by this point
    /// `refLanesSettledForRemount` has already consulted it (rev.6 Task 7: `tryRemountOnce` pays
    /// `materialization_grace_ms` above when it was unresolved), so dropping the map slot here is a pure
    /// cache detach, not a certification of the wedge's outcome -- the recovery seal (Task 8) is what
    /// makes the dropped-epoch region uniformly invisible to readers regardless. Respects the same idle
    /// invariants the eviction pass uses: it never mutates a runtime a leader still holds (the superseded
    /// flag makes that leader fail closed instead).
    void quiesceRefTablesForRemount();

    /// rev.6 Task 7 (spec §Self-remount): the self-remount counterpart of `drainRefLanesForShutdown` --
    /// same wait mechanics (snapshot the cached tables, wait each one's queue idle bounded by a budget,
    /// then check every table's `wedge` under its own `state_mutex`) -- but WITHOUT the `shutting_down`
    /// admission latch: self-remount mutations are already refused by the tripped mount fence, not an
    /// admission check, so there is nothing to latch. Budget is exactly one attempt's worth
    /// (`cas_request_budget.attempt_timeout_ms + lease_safety_margin_ms`), long enough for an in-flight
    /// leader to observe the tripped fence and settle, never unbounded. Returns true ("settled") only
    /// when every queue went idle within budget AND no table is left wedged -- i.e. no in-flight ref-log
    /// conditional `PUT` from the dying epoch can still land. `tryRemountOnce` pays
    /// `materialization_grace_ms` only when this returns false.
    bool refLanesSettledForRemount();

    /// Leader loop / one flush for the ref-log append lane (recovery, wedge resolution, per-item
    /// validation, admission budget, the controlled `PUT`, and applying the committed transaction to
    /// `rt.state`).
    void runRefQueueLeader(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt,
                           const std::shared_ptr<RefMutationItem> & own);
    void flushRefBatch(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// rev.6 Task 5 (spec §clean-release drain): the certificate the clean-release farewell marker
    /// depends on. Latches `shutting_down` (so every ref mutation from here on is refused at admission,
    /// see `appendRefOps`'s entry check), then waits -- bounded by `wait_budget_ms` -- for every cached
    /// table's append queue to go idle (`pending.empty() && !leader_active`), and finally checks each
    /// table's `wedge` under its own `state_mutex`: a queue can go idle with a wedge still recorded (the
    /// wedged item's OWN caller already got its error and the leader bookkeeping reset -- see
    /// `flushRefBatch`'s `Unresolved` case -- while the underlying `PUT` itself stays genuinely
    /// unresolved). Returns true ("drained") ONLY when every queue went idle within budget AND no table
    /// is wedged -- i.e. no in-flight ref-log conditional `PUT` from this incarnation can still land.
    /// Fail-closed: a timeout or any remaining wedge returns false, and the caller (`~Store`) must skip
    /// the farewell marker rather than certify a death this incarnation cannot actually prove.
    bool drainRefLanesForShutdown(uint64_t wait_budget_ms);

    /// See `setRefPreCarveHookForTest`. Null in production (a no-op call site).
    std::function<void()> ref_pre_carve_hook_for_test;

    /// ==== Task 11: snapshot publication + stale-precommit successor cleanup ====

    /// Cheap threshold/lifecycle check (lock + comparison, no I/O); if triggered, dispatches
    /// `trySnapshotPublishOnce` onto a detached background thread (spec: "off the mutation hot path
    /// and never blocks appends" -- `trySnapshotPublishOnce` never touches the append queue, so this
    /// can never deadlock against a flush leader). Called after every commit in `flushRefBatch` (the
    /// threshold trigger) and once per table from `appendRefOps`'s top level right after recovery (the
    /// mount-time trigger).
    void maybeScheduleSnapshotPublish(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// C4 per-table publish-dispatch backoff (caller holds `rt.state_mutex`). `advance` arms/doubles the
    /// backoff after a non-Committed publish outcome (bounded by `snapshot_publish_backoff_max_ms`);
    /// `reset` clears it after a durable publish. See `RefTableRuntime::publish_backoff_*`.
    void advancePublishBackoff(RefTableRuntime & rt);
    void resetPublishBackoff(RefTableRuntime & rt);

    /// S13 fix: per-table retry cooldown for the stale-precommit sweep (caller holds `rt.state_mutex`).
    /// `advance` arms/doubles the cooldown after a FAILED sweep attempt (bounded by
    /// `precommit_sweep_backoff_max_ms`); `reset` clears it after a verified-clean sweep. See
    /// `RefTableRuntime::precommit_sweep_backoff_*`.
    void advancePrecommitSweepBackoff(RefTableRuntime & rt);
    void resetPrecommitSweepBackoff(RefTableRuntime & rt);

    /// Cheap flag check (lock + bool, no I/O) at the top of `appendRefOps`; if this table's stale
    /// (older-epoch) precommits haven't been verified clean yet this mount -- and no failure cooldown is
    /// pending -- clears the flag and runs `sweepStalePrecommitsNow` SYNCHRONOUSLY on the CALLING thread.
    /// Safe only because it runs BEFORE that caller enqueues its own item or becomes a queue leader --
    /// the sweep's own `appendRefOps` calls are therefore a separate top-level invocation, never nested
    /// inside a leader's flush stack (which would deadlock the leader against itself). S13 fix: the
    /// clear-before-attempt is for RE-ENTRANCY only, never consumption -- ANY failure (exception,
    /// uncertain PUT, partial progress) re-arms the flag with a bounded backoff and rethrows, so a later
    /// read/mutation trigger retries until a sweep completes verified clean.
    void maybeSweepStalePrecommits(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    /// The actual chunked removal loop (spec §Clean Up Old Precommits): repeatedly re-reads the live
    /// state, gathers up to `ref_txn_max_ops` stale precommits (`manifest_ref.writer_epoch <
    /// liveWriterEpoch()`), and appends one bounded exact-removal transaction per chunk, until none remain.
    void sweepStalePrecommitsNow(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);
    /// The READ-side wrapper `resolveRef`/`listRefs` call instead of
    /// `maybeSweepStalePrecommits` directly -- catches any failure (an uncertain PUT propagated as
    /// ABORTED), counts it (`CasRefSweepDeferred`), logs once, and lets the read proceed. Swallowing
    /// here does NOT drop the sweep (S13 fix): the failed attempt already re-armed
    /// `needs_stale_precommit_sweep` inside `maybeSweepStalePrecommits`, so a later trigger retries. A
    /// mutation path (`appendRefOps`'s own hoisted call) must not use this: it calls
    /// `maybeSweepStalePrecommits` directly and keeps propagating, since a mutation must not proceed
    /// past a wedged lane.
    void sweepStalePrecommitsForRead(const RootNamespace & ns, const std::shared_ptr<RefTableRuntime> & rt);

    /// Publishes the constant-size `Removed` snapshot for a namespace whose `remove_namespace` is
    /// already durable (spec §Namespace Removal). Best-effort / idempotent: a non-Committed outcome, or
    /// this namespace not (yet) being `Removed`, is silently a no-op -- Task 12's namespace-cleanup item
    /// republishes it if this writer stops first.
    void publishRemovedSnapshotNow(const RootNamespace & ns);

public:
    /// Test seams (Task 10): observe recovery-restart counting and wedge state without a private-member
    /// friend hack. Recovers the table (like any real read) if not already cached.
    uint64_t refRecoveryRestartsForTest(const RootNamespace & ns);
    bool refLaneWedgedForTest(const RootNamespace & ns);
    /// (I1) The object key of the current wedge for `ns`, or empty when the lane is not wedged -- lets a
    /// test land a DIFFERENT object at the exact wedged key to exercise resolve-time CORRUPTED_DATA.
    String wedgedKeyForTest(const RootNamespace & ns);
    /// S13 fix: whether this table still owes a stale-precommit sweep (armed by recovery; re-armed by a
    /// failed attempt; cleared permanently only by a verified-clean sweep). Recovers the table (like any
    /// real read) if not already cached.
    bool needsStalePrecommitSweepForTest(const RootNamespace & ns);

    /// Number of ref-append lanes currently wedged (an uncertain PUT exhausted its retry budget and
    /// the lane blocks until the same key resolves durable or is conclusively rejected). Per-disk GC
    /// health for system.content_addressed_mounts (B3). O(live tables); takes each runtime state lock.
    size_t wedgedRefLaneCount();

    /// Task 11 test seam: blocks until every background snapshot-publish attempt dispatched so far for
    /// `ns` has settled. Needed only by tests that exercise the REAL background dispatch (production
    /// concurrency); tests that just want deterministic publish-logic coverage call
    /// `trySnapshotPublishOnce` directly instead.
    void waitForSnapshotPublishSettleForTest(const RootNamespace & ns);

    /// C4 test seam: the count of in-flight background snapshot-publish attempts for `ns` (the
    /// single-in-flight gate holds this at <= 1). Recovers the table (idempotent) if not already cached.
    int pendingSnapshotPublishesForTest(const RootNamespace & ns);

    /// Task 11 test seam: the id of the newest snapshot this runtime has confirmed durable (recovered
    /// or published), or `nullopt` if none. Recovers the table (idempotent) if not already cached.
    std::optional<RefTxnId> newestPublishedSnapshotIdForTest(const RootNamespace & ns);

    /// Task 11 test seam: count of applied txns retained above `newestPublishedSnapshotIdForTest` (the
    /// tail a snapshot candidate would replay from).
    size_t tailSinceSnapshotCountForTest(const RootNamespace & ns);

    /// Test-only hook: called by `flushRefBatch`
    /// right before it carves a batch, i.e. AFTER the table is already recovered -- the one otherwise
    /// untestable timing window `BlockingGetBackend`-style backend tricks cannot reach, since a warm
    /// flush performs no I/O between becoming leader and carving. A test blocks here to let a second
    /// caller's item join `rt->pending` before the carve, forcing deterministic co-batching.
    void setRefPreCarveHookForTest(std::function<void()> hook) { ref_pre_carve_hook_for_test = std::move(hook); }

    /// Test-only: replace the request controller's inter-attempt backoff sleep (e.g. with a no-op) —
    /// for tests that drive a persistent conditional-write fault to budget exhaustion through a fully
    /// wired Store/disk and must not serve the production capped-exponential sleeps for real (see
    /// `CasRequestController::setSleepFnForTest`). Call before driving traffic; empty restores the
    /// real sleep.
    void setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn);

    /// Queue depth for the ref-append-lane tests (mirrors `shardQueuePendingForTest`): how many
    /// `appendRefOps` callers are enqueued for `ns` right now.
    size_t refQueuePendingForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it == ref_tables.end() ? 0 : it->second->pending.size();
    }

    /// Task 13 cache-eviction test seams: how many whole ref tables are cached right now, and whether a
    /// specific table's runtime is currently materialized (recovered) in the cache -- a table that was
    /// evicted reports false until its next touch re-recovers it.
    size_t refTablesCachedCountForTest()
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        return ref_tables.size();
    }
    bool refTableCachedForTest(const RootNamespace & ns)
    {
        std::lock_guard<std::mutex> g(ref_queue_mutex);
        const auto it = ref_tables.find(ns.string());
        return it != ref_tables.end() && it->second->recovered;
    }

private:
    BackendPtr pool_backend;
    PoolConfig config;
    PoolMeta meta;

    /// CAS mixed-algo pools (Phase 3 T4): monotone in-memory cache of `algos_used`, seeded from
    /// `meta.algos_used` at open. Guards `isAlgoAdmitted`/`refreshAdmittedAlgos` -- ITS OWN mutex,
    /// not `meta`'s (there is no other mutable access to `meta` post-open; this avoids taking a
    /// wider lock than the cache needs). Kept sorted (a plain vector; membership is a handful of
    /// entries, no need for a set).
    mutable std::mutex admitted_algos_mutex;
    std::vector<uint8_t> admitted_algos;

    /// P1 known-present cache: a bytes-bounded LRU set of blob hashes confirmed present in the pool.
    /// Value is a 1-byte presence marker; DedupWeight charges a fixed per-entry byte estimate so the
    /// configured `dedup_cache_bytes` is an honest memory ceiling. nullptr ⇔ disabled.
    struct DedupPresent {};
    struct DedupWeight { size_t operator()(const DedupPresent &) const { return 64; } };
    using DedupCache = CacheBase<BlobRef, DedupPresent, BlobRefHash, DedupWeight>;
    std::unique_ptr<DedupCache> dedup_cache;
    Layout pool_layout;
    /// Per-server build watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random
    /// nonzero u64 minted once at open: GC checks it for EQUALITY (an object stamped with a different
    /// epoch is from a dead incarnation), never for ordering. next_build_seq is a strictly-increasing
    /// per-process counter (monotonicity is load-bearing — a seq is never reused or lowered);
    /// active_build_seqs holds the seqs of in-flight builds, so minActive yields the GC floor. After
    /// the ack-floor merge (spec 2026-07-02) the floor is published by the merged `mount_keeper`
    /// beat (there is no standalone watermark object anymore). ATOMIC because a self-remount re-stamps it
    /// (kept equal to `live_writer_epoch`) off the background remount thread while `epoch`/`writerEpoch`
    /// may observe it; the ref-lane hot readers were moved to `liveWriterEpoch`, so this now backs only
    /// the identity accessors.
    std::atomic<uint64_t> process_epoch{0};
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;
    std::set<uint64_t> active_build_seqs;
    /// In-flight builds keyed by build_seq (mirrors `active_build_seqs`' lifecycle: populated in
    /// `startBuild`, removed in `retireBuildSeq`). `dropNamespace` upgrades these weak_ptrs AFTER its
    /// removal transaction is durable and cancels those targeting the removed namespace (spec §Namespace
    /// Removal: "cancels local builds"). weak_ptr because the wiring owns the shared_ptr; an expired entry
    /// (a build already destroyed) is simply skipped. Guarded by builds_mutex.
    std::map<uint64_t, std::weak_ptr<Build>> inflight_builds;

    /// Mount-lease heartbeat (spec §mount-safety, Phase 0 Task 7). Constructed + started on a writable
    /// open AFTER the owner/epoch/mount startup protocol; renews the mount lease async off the write
    /// path and drives the local write fence (deadline on each successful renew, `tripMountLost` on a
    /// superseded/foreign touch). The dtor stops it, whose terminate() retires the lease (so a
    /// same-server reopen can immediately reclaim). Null on a read-only open.
    std::unique_ptr<MountLeaseKeeper> mount_keeper;

    /// Self-remount machinery: `scheduleRemount` (called from the keeper's renew-failure path in
    /// production; gated on `background_watermark` like every background thread) runs
    /// `tryRemountOnce` with exponential backoff until it succeeds or the Store tears down.
    void scheduleRemount();
    std::atomic<uint64_t> live_writer_epoch{0};
    std::mutex remount_mutex;              /// serializes tryRemountOnce
    std::mutex remount_thread_mutex;       /// guards the thread handle below
    std::atomic<bool> remount_running{false};
    std::atomic<bool> remount_stop{false};
    std::atomic<bool> remount_shutting_down{false};   /// latched at ~Store() top; scheduleRemount refuses to re-arm during teardown
    std::condition_variable remount_cv;
    std::mutex remount_cv_mutex;
    ThreadFromGlobalPool remount_thread;

    /// Local write fence (spec §write-fence). Permissive by default (deadline = time_point::max,
    /// lost = false), so mayMutate is true until Task 7 arms it with a real lease deadline and the
    /// renewer trips it. Gates the ref-append mutate chokepoint.
    MountFence mount_fence;

    /// rev.6 Task 6: latched true the moment a writable `open` reclaims a mount whose predecessor's
    /// death was NOT proven clean (`MountPriorState::Fenced` or `UncleanObserved`). Sticky for the life
    /// of this incarnation -- Task 8 consumes it; see `uncleanEpochBoundarySeenForTest`.
    std::atomic<bool> unclean_epoch_boundary_seen{false};

    /// rev.6 Task 6 / S13 observation loop: the ONE seam both `Store::open`'s mount-claim observation
    /// poll and the `materialization_grace_ms` wait go through. Routes through `config.wait_sleep_fn`
    /// when a test injected one, else a real `std::this_thread::sleep_for` (production, unchanged).
    void waitSleep(uint64_t ms) const;

    /// Phase 1c manifest decode cache: (ManifestId, Token) -> decoded immutable PartManifest. Part
    /// manifests are immutable single-owner objects, so a token match guarantees identical bytes; the
    /// Token component lets the cache fail closed if the backend object is re-incarnated under the same
    /// id. Unlike the old content-hash tree cache there is NO cross-id sharing — each publish has a
    /// unique ManifestId (spec §Read Path Scope: per-instance cache, less sharing, intentional). The
    /// read path resolves `route` per file, so caching makes a repeated same-part read O(1) decodes.
    /// Phase 5 (part-folder cache spec): byte-weighted LRU (`ManifestDecodeCache` below) instead of the
    /// old count-only bound, since decoded manifests carry inline bytes and can each be megabytes.
    struct ManifestCacheKey
    {
        ManifestId manifest_id;
        Token token;
        bool operator==(const ManifestCacheKey &) const = default;
    };
    struct ManifestCacheKeyHash
    {
        size_t operator()(const ManifestCacheKey & k) const;
    };
    /// Phase 5 (part-folder cache spec): byte-weighted so a server that reads very many parts (each
    /// decode carrying megabytes of inline bytes) has an honest memory ceiling instead of the old
    /// count-only bound (16384 entries, multi-GB worst case). Same key, same fail-closed token
    /// semantics as before; nullptr <=> decode caching disabled (manifest_decode_cache_bytes == 0).
    struct PartManifestWeight
    {
        size_t operator()(const PartManifest & m) const
        {
            size_t bytes = 256;
            for (const auto & e : m.entries)
                bytes += e.path.size() + e.inline_bytes.size() + 96;
            return bytes;
        }
    };
    using ManifestDecodeCache = CacheBase<ManifestCacheKey, PartManifest, ManifestCacheKeyHash, PartManifestWeight>;
    std::unique_ptr<ManifestDecodeCache> manifest_cache;

    /// NOTE (M-C2): the ref-log is never trimmed here — trimming needs GC's fold state
    /// (`last_folded_ref_id`, INV-JOURNAL-COVERAGE), which is GC state landing in M-C3.
};

}
