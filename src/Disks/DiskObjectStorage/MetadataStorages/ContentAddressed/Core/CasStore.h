#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
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
/// or from GC/maintenance (trim/fence/reclaim). Writer mutations may be delayed for backpressure; GC
/// mutations bypass backpressure entirely so cleanup cannot delay itself.
enum class RootMutationOrigin : uint8_t
{
    Writer,
    Gc,
};

/// The write-scope of one `mutateShard` closure (shard-mutation-queue spec 2026-07-03): which part
/// of the shard the closure touches. The flat-combining batch builder admits at most ONE mutation
/// per ref name into a single flush (per-ref durable histories stay bit-identical to the unbatched
/// protocol) and flushes `WholeShard` closures SOLO (trim, GC fence, dropNamespace, reclaim —
/// anything touching multiple refs or the journal wholesale).
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
    Fence,
    Trim,
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
        case RootMutationKind::Fence:             return "Fence";
        case RootMutationKind::Trim:              return "Trim";
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
    /// Creation-time only; the pool is authoritative on reopen. Default WEIGHED 2026-07-03 (night
    /// forensics): per-shard journal tail = mutation_rate/N x fold-cursor age; with the
    /// flat-combining queue the contention argument for large N is gone, so N trades BODY SIZE
    /// (flush latency, read-modify-write bytes, object-store inline thresholds) against DISCOVERY
    /// keys (∝N per namespace per round) and queue batching (dies as N grows). 8 concentrated a
    /// hot table's writes badly (100 KB+ tails, 600 KB under cursor-lag storms); 128 over-shards
    /// (batching ~1x, discovery x16, tail already tiny at 64). 32 keeps a hot table's tail ~25 KB
    /// healthy / ~165 KB under a 10-minute storm with batching ~1.4x and discovery x4.
    uint64_t root_shards = 32;
    uint64_t blob_header_len = 256;           /// creation-time only; ditto
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
    /// Phase 1d). Creation-time only; the pool is authoritative on reopen, like `root_shards`. This
    /// is the BLOB-HASH-prefix reducer axis, distinct from the root-shard fence axis.
    uint64_t gc_shards = 1;
    /// Phase 2 cursor-paced orphan part-manifest sweep. The LIST budget bounds cold-prefix enumeration
    /// per completed GC round; the delete budget separately bounds exact-token destructive work.
    uint64_t manifest_sweep_list_budget_keys = 1000;
    uint64_t manifest_sweep_delete_budget_keys = 100;
    /// B12 lazy-trim: compact a root shard's journal ONLY when at least this many events lie at/below
    /// the sealed fold cursor (batch gate), OR the encoded shard body is at/above
    /// `gc_trim_body_soft_limit` (soft-cap gate), OR `Gc::setMaintenanceTrimForTest(true)` is set.
    /// The two hard gates guarantee bounded journal growth: a shard that accumulates enough events or
    /// grows large enough is ALWAYS compacted regardless of batch size. 0 disables the batch gate
    /// (compact on >= 1 event — the previous eager behaviour, useful for tests).
    uint64_t gc_trim_min_events = 256;
    /// B12: encoded root-shard body size at/above which trim is forced regardless of event count. This
    /// is the BACKSTOP against unbounded journal growth, not the working trimmer: the count gate
    /// (gc_trim_min_events = 256 events ≈ 21 KB) fires long before any reasonable size cap, and NO
    /// cap can cut events above the sealed fold cursor — the body is CURSOR-AGE-bound, not
    /// trim-bound (2026-07-03 night: a 96 KiB cap experiment could not hold a hot table's body;
    /// the real levers are root_shards and fold cadence). Keeping shard bodies under an object
    /// store's inline threshold (e.g. RustFS 128 KiB, relevant while rustfs#3231 leaks data dirs
    /// on big-object overwrite) is achieved by root_shards sizing, not by this cap.
    uint64_t gc_trim_body_soft_limit = 8ULL << 20;   /// 8 MiB (backstop)
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
    uint64_t manifest_soft_limit = 16ULL << 20;
    uint64_t manifest_hard_limit = 64ULL << 20;
    /// B164b: max backpressure delay (ms) applied to writer-originated mutations whose encoded root-shard
    /// body is between `manifest_soft_limit` and `manifest_hard_limit`. 0 disables delay (soft limit acts
    /// as a warning-only log). At most one delay per mutation call. Delay is linear: 0 at soft_limit →
    /// `manifest_max_delay_ms` near the hard limit.
    uint64_t manifest_max_delay_ms = 1000;
    bool background_watermark = false;       /// tests drive renewOnce explicitly; gates the merged heartbeat's background thread

    /// Mount-lease TTL (spec §mount-safety): how long a freshly-renewed mount lease is valid. The local
    /// write fence's monotonic deadline is `renew_time + this`, so a superseded/paused writer is fenced
    /// once `this` elapses with no successful renew. The background renewer runs every
    /// `mount_renew_period` (default ttl/3) so a healthy mount renews well before expiry.
    std::chrono::milliseconds mount_lease_ttl_ms{30000};
    std::chrono::milliseconds mount_renew_period{10000};   /// = ttl/3 by default
    bool read_only = false;                   /// observe-only open: skip the mutating capability probe; reads only

    /// Pillar B bounded-TTL decode cache: a staleness-tolerant caller (allow_stale=true) may reuse a
    /// decode validated < this many ms ago WITHOUT a HEAD. 0 disables the TTL (all callers force-fresh).
    /// Strict-freshness callers always pass allow_stale=false and always HEAD, regardless of this value.
    std::chrono::milliseconds shard_decode_cache_ttl_ms{200};

    /// The write-fence deadline clock (CLOCK_BOOTTIME milliseconds; see `MountFence`). Empty = the real
    /// boot clock (`Store::bootMs`); injected by tests to drive the fence deadline deterministically.
    std::function<uint64_t()> boot_ms_fn = {};
};

struct Resolved
{
    /// The namespace-qualified identity of the part manifest this ref names. The owning RootNamespace
    /// + the RootRef.manifest_ref form the ManifestId (the ref carries no namespace itself — that comes
    /// from the owning root context, spec §Object Identity And Ownership).
    ManifestId manifest_id;
    uint64_t manifest_size = 0;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
};

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
    /// Build drives the manifest publish CAS through the private mutateShard/shardOf: the gate logic
    /// (W-PUBLISH-GATE) is Build's responsibility (it owns deps/retireView access), but the CAS loop
    /// itself is the verified Store loop — reused, never duplicated.
    friend class Build;
    /// Gc drives the manifest fence CAS (R3) through the same private mutateShard loop — reused,
    /// never duplicated (the lease itself only needs the public accessors).
    friend class Gc;

public:
    static StorePtr open(BackendPtr backend, PoolConfig config);
    ~Store();

    /// ---- per-server watermark surface (spec 2026-06-16-ca-build-watermark) ----
    /// process_epoch: random nonzero per Store (process). GC checks epoch EQUALITY, never ordering.
    uint64_t epoch() const { return process_epoch; }
    /// The durable-monotone writer_epoch allocated at writable open (spec §writer-epoch-alloc). On a
    /// writable Store this is the value bridged into `process_epoch` (so the watermark + the manifest
    /// manifest ref carries it); on a read-only open the random `process_epoch` is unchanged and
    /// no durable epoch is allocated. Phase 2's epoch-aware sweep reads this value.
    uint64_t writerEpoch() const { return process_epoch; }
    /// The GC floor: the oldest in-flight build_seq, or next_build_seq when no build is active (so a
    /// quiescent server's watermark floor advances to the next-to-be-allocated seq). Locks builds_mutex.
    uint64_t minActive();
    /// The GC round the retired view is CURRENTLY INSTALLED at (spec 2026-07-06-decouple). Cheap,
    /// in-memory — the value the lease renewal advertises as `observed_gc_round`. Race-safe against
    /// the retired-view syncer via `RetireView`'s own internal shared_mutex; takes no Store lock.
    uint64_t observedGcRound() const;
    /// Test/assertion accessor for the next-to-allocate build_seq under the lock.
    uint64_t peekNextBuildSeq();
    /// Renew the merged heartbeat once (bump seq, refresh min_active from the live callback, stamp the
    /// last-INSTALLED GC round via `observedGcRound()`, stamp a fresh expires_at_ms). The build-watermark
    /// floor rides this beat after the ack-floor merge — there is no standalone watermark object. In
    /// production this is driven by the background renewer (background_watermark), which no longer
    /// depends on S3 (the retired-view syncer, Task 3, advances the installed round independently).
    /// Redefined (spec 2026-07-06-decouple) as the composed test/manual driver: `syncRetiredView()`
    /// THEN `mount_keeper->renewOnce()`, so a single call still makes `observed_gc_round` follow the
    /// freshly-published round — the contract the GC pipeline tests rely on. Use `renewLeaseOnlyForTest`
    /// to drive the isolated renewal path (renew without syncing).
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

    /// ---- ref lifecycle (CAS loops on the owning shard) ----
    void dropRef(const RootNamespace & ns, const String & ref_name);            /// refs−− + '-' journal, atomic
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RootRef &)> mutator);              /// mutable fields only; NO journal
    void dropNamespace(const RootNamespace & ns);                 /// tombstone every shard + delete verbatim files

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
    void removeMountpointObject(const String & key);

    /// Internal surface for Build (same TU family; not for the wiring):
    const PoolConfig & poolConfig() const { return config; }
    const PoolMeta & poolMeta() const { return meta; }
    const Layout & layout() const { return pool_layout; }
    Backend & backend() { return *pool_backend; }
    RetireView & retireView() { return retire_view; }

    /// The writer_epoch of the LIVE mount incarnation. Bumped by `tryRemountOnce` (self-remount
    /// after a GC fence-out) — a `Build` minted under an older epoch fails closed on its next step.
    uint64_t liveWriterEpoch() const { return live_writer_epoch.load(std::memory_order_acquire); }

    /// Self-remount after a GC fence-out (liveness counterpart of the fence-out safety rule): the
    /// OLD incarnation may never write again (the keeper never re-mints), but a FRESH incarnation —
    /// durable writer_epoch bump + mount reclaim + fresh retired view + re-armed write fence — is
    /// exactly what a server restart would create, so a live server may create it in place. Runs the
    /// same claim machinery as `Store::open` (S13); a synchronous `syncRetiredView()` call primes the
    /// view for the fresh incarnation before the new keeper's anchor (spec 2026-07-06-decouple — the
    /// keeper's own renewal reads the installed round via `observedGcRound()`, it does not sync), so
    /// open-ordering holds for the new incarnation too. Returns
    /// false (and changes nothing durable beyond the epoch bump) when the mount cannot be claimed
    /// (foreign owner / a genuinely live twin) — the caller retries. Safe to call concurrently
    /// (serialized internally); also the synchronous test seam.
    bool tryRemountOnce();

    /// Retired-view sync (spec 2026-07-06-cas-lease-view-sync-decouple; formerly the ack-floor
    /// "beat"): probe `gc/state`; when the published round advanced past the installed view, load and
    /// install the retired view under `RetireView`'s own internal mutex. There is NO install drain
    /// (spec 2026-07-09-cas-writer-gc-simplification D4): entry persistence + monotone installs mean a
    /// graduated entry (condemned at round C) is in EVERY view >= C+1, and this writer's own advertised
    /// round exceeded C before that graduation, so any later in-closure read sees the entry regardless
    /// of install timing; displacement is exact-token-safe. Returns the round the view is installed at.
    /// Any read failure leaves the view and the returned round UNCHANGED (fail-closed for the ack: never
    /// claim a view that was not actually loaded). Runs on the dedicated retired-view syncer thread
    /// (`retiredViewSyncLoop`) and, synchronously, at open/remount and in tests. It is NOT on the
    /// lease-renewal path — the renewal advertises the already-installed round via `observedGcRound()`.
    uint64_t syncRetiredView();

    /// Retired-view syncer (spec 2026-07-06-cas-lease-view-sync-decouple): a dedicated background
    /// thread that runs `syncRetiredView` on `mount_renew_period`, decoupled from the lease-renewal
    /// thread so slow S3 view work can never delay a lease renewal past its TTL. Gated on
    /// `background_watermark` like every background thread (production only; unit tests drive
    /// `syncRetiredView` explicitly via `renewWatermarkOnce`, or start/stop this thread directly).
    /// Started on writable open after the fence is armed; stopped+joined in the destructor before
    /// `retire_view`/`pool_backend` die.
    void startRetiredViewSync(std::chrono::milliseconds period);
    void stopRetiredViewSync();   /// idempotent

    /// P1 known-present blob-hash cache (design 2026-06-20, B168). A HINT only — correctness never
    /// depends on it: a hit just makes putBlob go HEAD-first, and a stale hit is caught by that HEAD.
    /// No-ops when disabled (dedup_cache_bytes == 0).
    bool dedupCacheContains(const UInt128 & blob_hash) const;
    void dedupCacheAdd(const UInt128 & blob_hash);
    /// Test seam: retained bytes of the manifest decode cache (0 when disabled).
    size_t manifestDecodeCacheBytesForTest() const { return manifest_cache ? manifest_cache->sizeInBytes() : 0; }
    /// The shard a ref name routes to: CityHash64(ref_name) % root_shards. Build uses it to address the
    /// publish/precommit CAS (the build-root ref name is the build_seq, B171); tests reconstruct the
    /// build-root shard with it.
    uint64_t shardOf(const String & ref_name) const;

    /// ---- B170 event audit (system.content_addressed_log) ----
    /// The wiring injects a sink (CasEvent -> SystemLog row) when the log is configured; null sink
    /// (unit tests, log disabled) makes emitEvent a no-op single branch. Build/Gc reach this via
    /// their owning Store. `reason`/`detail` on the event carry the decision's full rationale.
    void setEventSink(CasEventSink sink) { event_sink_ = std::move(sink); }
    void emitEvent(const CasEvent & e) const { if (event_sink_) event_sink_(e); }
    /// Cheap predicate so query-frequency hooks can skip constructing the CasEvent (+ its detail map)
    /// entirely when the log is disabled (sink null) — a true no-op on the production hot path.
    bool hasEventSink() const noexcept { return static_cast<bool>(event_sink_); }

    /// B164b: injectable backpressure delay hook for tests. In production the default
    /// (std::this_thread::sleep_for) is used. Tests replace this with a no-op counter hook.
    void setBackpressureDelayHook(std::function<void(std::chrono::milliseconds)> hook)
    {
        backpressure_delay_hook = std::move(hook);
    }

    /// Task 5: read the current GC round from `gc/state`. Returns 0 when `gc/state` is absent (pool
    /// never GC'd). Used by `precommitAdd` to self-floor a NEWBORN ref-shard to the current round so
    /// the `promote` gate forces a retire-view refresh before the shard's blobs can be committed.
    uint64_t currentGcRound() const;

    /// Test seam (spec 2026-07-06-decouple): drive the ISOLATED lease renewal — renewOnce WITHOUT a
    /// preceding view sync — so a test can prove the renewal path advertises only the installed round
    /// and never loads the view. Production renewal runs this via the keeper's background thread;
    /// `renewWatermarkOnce` is the composed (sync+renew) driver. Defined in CasStore.cpp beside
    /// `renewWatermarkOnce` (needs `Exception`/`ErrorCodes::LOGICAL_ERROR`).
    void renewLeaseOnlyForTest();

    /// B164b test seam: mutate root shard with explicit origin/kind for backpressure
    /// verification. Production code uses private `mutateShard` via Build/Gc friend
    /// classes. Exists only so the GC-bypass test can verify that `RootMutationOrigin::Gc`
    /// skips backpressure delay without exposing the full mutation API.
    /// Task 2: `birth_incarnation` (default `{}`) is forwarded to the private `mutateShard`
    /// so incarnation-stamp tests can drive the create-if-absent path directly.
    /// Task 5: `birth_floor` (default 0) is forwarded to the private `mutateShard` as a
    /// lazy provider so self-floor tests can drive the create-if-absent path with an
    /// explicit fence_round. Wraps the eager value in a lambda — the provider is only
    /// invoked inside the create-if-absent branch (no S3 call on the existing-shard path).
    void mutateShardForTest(const RootNamespace & ns, uint64_t shard,
                            std::function<void(RootShard &)> mutate,
                            RootMutationOrigin origin, RootMutationKind kind,
                            ShardIncarnation birth_incarnation = {},
                            uint64_t birth_floor = 0)
    {
        std::function<uint64_t()> provider = birth_floor ? std::function<uint64_t()>([birth_floor] { return birth_floor; }) : nullptr;
        mutateShard(ns, shard, MutationScope::wholeShard(), std::move(mutate), nullptr, origin, kind, birth_incarnation, std::move(provider));
    }

    /// Queue depth for the shard-mutation-queue tests: how many mutations are enqueued (the
    /// leader's own item stays counted until its batch is carved, which happens after the flush's
    /// first read — so a blocked-in-read leader plus one waiter reads as depth 2).
    size_t shardQueuePendingForTest(const RootNamespace & ns, uint64_t shard)
    {
        std::lock_guard<std::mutex> g(shard_queue_mutex);
        const auto it = shard_queues.find(std::make_pair(ns.string(), shard));
        return it == shard_queues.end() ? 0 : it->second->pending.size();
    }

    /// Scoped variant for the shard-mutation-queue tests (spec 2026-07-03): exposes the scope so
    /// batching/cut semantics are testable; returns the committed version.
    uint64_t mutateShardScopedForTest(const RootNamespace & ns, uint64_t shard, MutationScope scope,
                                      std::function<void(RootShard &)> mutate,
                                      RootMutationOrigin origin = RootMutationOrigin::Writer,
                                      RootMutationKind kind = RootMutationKind::Publish)
    {
        uint64_t v = 0;
        mutateShard(ns, shard, std::move(scope), std::move(mutate), &v, origin, kind, {}, nullptr);
        return v;
    }

private:

    Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_);

    CasEventSink event_sink_;   /// B170: null = disabled (emitEvent no-op)

    /// Allocate a strictly-increasing build_seq and add it to the active set (called by startBuild).
    uint64_t allocateBuildSeq();
    /// Remove a build_seq from the active set; idempotent (safe from publish/abandon/dtor).
    void retireBuildSeq(uint64_t seq);

    /// Read shard manifest (absent ⇒ empty RootShard with no token); used by mutateShard (writes,
    /// which need the token for the CAS) and as the uncached primitive under readShardDecoded.
    std::pair<RootShard, std::optional<Token>> readShard(const RootNamespace & ns, uint64_t shard);

    /// ---- plain-object CAS helpers (shared by namespace-file and mountpoint-object paths) ----
    /// head + putIfAbsent/putOverwrite loop (bounded by MAX_CAS_ATTEMPTS); throws ABORTED on live-lock.
    void casPutObject(const String & full_key, const String & bytes);
    /// Plain get → bytes; returns nullopt when absent.
    std::optional<String> casGetObject(const String & full_key);
    /// head + deleteExact loop; no-op when absent. Throws ABORTED on live-lock.
    void casRemoveObject(const String & full_key);

    /// Read-path shard read with a token-validated DECODE CACHE (B113): a `head` fetches the current
    /// token cheaply; on a token match the already-decoded, IMMUTABLE manifest is returned without a
    /// `get` or a re-decode (the dominant read-heavy cost — the whole shard manifest holds many refs).
    /// The cached object is `const` and shared, so it is never mutated; writers use `readShard`
    /// (always fresh) so a publish/drop never reads stale state. Correctness rests on the token
    /// uniquely identifying the incarnation's bytes (every write mints a new token), so a matching
    /// token guarantees identical content. Used by resolveRef/listRefs only.
    /// Single-flight coalescing (Pillar B, Task 2): concurrent callers for the same shard key share
    /// ONE head (+ at most one get); followers wait on the leader's shared_future.
    /// Pillar B TTL fast-path (Task 3): allow_stale=true callers skip the HEAD entirely when the
    /// cache entry was validated within shard_decode_cache_ttl_ms.
    std::shared_ptr<const RootShard> readShardDecoded(const RootNamespace & ns, uint64_t shard, bool allow_stale = false);
    /// The actual head+get+decode for one shard key (no coalescing). Called by coalescedReadShardDecoded.
    std::shared_ptr<const RootShard> loadShardDecoded(const String & key);
    /// Single-flight coalescing wrapper around loadShardDecoded.
    std::shared_ptr<const RootShard> coalescedReadShardDecoded(const String & key);

    /// Read-modify-CAS one shard manifest under the manifest size guard. `mutate` edits the in-memory
    /// RootShard (which carries the freshly-read shard_version); the helper bumps shard_version, encodes,
    /// applies the manifest size guard (soft ⇒ LOG_WARNING + backpressure delay for Writer, hard ⇒
    /// LIMIT_EXCEEDED), and casPut against the observed token (nullopt when the shard was absent —
    /// create-if-absent). On Conflict it re-reads and retries the WHOLE mutate, bounded (100) then ABORTED
    /// ("manifest CAS contention on {}"). Single-writer shards make a real storm impossible; the bound is
    /// a runaway brake. `mutate` runs on the FRESHLY READ root each attempt, so a journal append is never
    /// double-applied across retries.
    /// `out_committed_version` (optional) receives the shard_version the successful casPut committed —
    /// the GC fence (R3) records it as the durable per-shard fence position (the model's fencePos[s]).
    /// `origin` distinguishes Writer mutations (subject to backpressure) from Gc mutations (bypass).
    /// `kind` is diagnostic-only (logged/metrics) and does not affect behaviour.
    /// When the shard does not yet exist (token == nullopt on the first readShard call), the shard is
    /// created fresh: `root.incarnation` is set to `birth_incarnation` AND `root.fence_round` is set
    /// to the value returned by `birth_floor_provider` (Task 5 self-floor) BEFORE `mutate` runs. On
    /// subsequent mutations (token present), both are left untouched (immutable for the shard's life).
    /// Callers that never create a first object (drop, updateRefPayload, dropNamespace, GC fence) pass
    /// the defaults (`{}`, nullptr) — the stamps are no-ops since the shard already exists.
    /// Only `precommitAdd` passes a non-null `birth_floor_provider` (a lazy S3 GET of `gc/state`);
    /// the provider is invoked ONLY on the create-if-absent branch so the common existing-shard path
    /// incurs ZERO extra S3 round-trips. `fence_round = 0` (fresh pool, no GC) is a valid value and
    /// is assigned unconditionally (the `if (birth_floor > 0)` guard is a footgun — removed).
    void mutateShard(const RootNamespace & ns, uint64_t shard, MutationScope scope,
                     std::function<void(RootShard &)> mutate,
                     uint64_t * out_committed_version,
                     RootMutationOrigin origin, RootMutationKind kind,
                     ShardIncarnation birth_incarnation = {},
                     std::function<uint64_t()> birth_floor_provider = nullptr);

    /// ==== flat-combining shard-mutation queue (spec 2026-07-03-cas-shard-mutation-queue) ====
    /// One queued item = one BLOCKED mutateShard caller (bounded by writer-thread count by
    /// construction). Map entries exist only while work is in flight; ONE mutex guards the map and
    /// every item's completion fields; per-queue cv wakes that queue's waiters only.
    struct ShardMutationItem
    {
        MutationScope scope;
        std::function<void(RootShard &)> mutate;
        RootMutationOrigin origin = RootMutationOrigin::Writer;
        RootMutationKind kind = RootMutationKind::Publish;
        ShardIncarnation birth_incarnation;
        std::function<uint64_t()> birth_floor_provider;
        bool done = false;                       /// guarded by shard_queue_mutex
        std::exception_ptr error;                /// guarded by shard_queue_mutex
        uint64_t committed_version = 0;          /// written by the leader before done = true
    };
    struct ShardMutationQueue
    {
        std::deque<std::shared_ptr<ShardMutationItem>> pending;
        bool leader_active = false;
        uint64_t force_solo = 0;                 /// hard-limit degrade: next N carves are solo
        std::condition_variable cv;
    };
    static constexpr size_t kMaxShardBatch = 128;
    std::mutex shard_queue_mutex;
    std::map<std::pair<String, uint64_t>, std::shared_ptr<ShardMutationQueue>> shard_queues;

    /// Leader loop: flush batches until the leader's OWN item completes (fairness baton pass).
    void runShardQueueLeader(const RootNamespace & ns, uint64_t shard,
                             const std::shared_ptr<ShardMutationQueue> & q,
                             const std::shared_ptr<ShardMutationItem> & own);
    /// One carved batch through one CAS loop; NEVER throws (outcomes land in the items).
    void flushShardBatch(const RootNamespace & ns, uint64_t shard,
                         const std::shared_ptr<ShardMutationQueue> & q);

    BackendPtr pool_backend;
    PoolConfig config;
    PoolMeta meta;

    /// P1 known-present cache: a bytes-bounded LRU set of blob hashes confirmed present in the pool.
    /// Value is a 1-byte presence marker; DedupWeight charges a fixed per-entry byte estimate so the
    /// configured `dedup_cache_bytes` is an honest memory ceiling. nullptr ⇔ disabled.
    struct DedupPresent {};
    struct DedupWeight { size_t operator()(const DedupPresent &) const { return 64; } };
    using DedupCache = CacheBase<UInt128, DedupPresent, UInt128Hash, DedupWeight>;
    std::unique_ptr<DedupCache> dedup_cache;
    /// pool_layout MUST precede retire_view: the ctor init list builds retire_view from pool_layout.
    Layout pool_layout;
    RetireView retire_view;
    /// Per-server build watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random
    /// nonzero u64 minted once at open: GC checks it for EQUALITY (an object stamped with a different
    /// epoch is from a dead incarnation), never for ordering. next_build_seq is a strictly-increasing
    /// per-process counter (monotonicity is load-bearing — a seq is never reused or lowered);
    /// active_build_seqs holds the seqs of in-flight builds, so minActive yields the GC floor. After
    /// the ack-floor merge (spec 2026-07-02) the floor is published by the merged `mount_keeper`
    /// beat (there is no standalone watermark object anymore).
    uint64_t process_epoch = 0;
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;
    std::set<uint64_t> active_build_seqs;

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
    std::condition_variable remount_cv;
    std::mutex remount_cv_mutex;
    ThreadFromGlobalPool remount_thread;

    /// Retired-view syncer private state: `startRetiredViewSync`/`stopRetiredViewSync` (public, above,
    /// next to `syncRetiredView`) start/stop this thread; the loop body itself stays private.
    void retiredViewSyncLoop(std::chrono::milliseconds period);
    std::mutex retired_view_sync_mutex;
    std::condition_variable retired_view_sync_cv;
    bool retired_view_sync_stop = false;   /// guarded by retired_view_sync_mutex
    ThreadFromGlobalPool retired_view_sync_thread;

    /// Local write fence (spec §write-fence). Permissive by default (deadline = time_point::max,
    /// lost = false), so mayMutate() is true until Task 7 arms it with a real lease deadline and the
    /// renewer trips it. Gates the mutate chokepoint (mutateShard).
    MountFence mount_fence;

    /// B113 read-path decode cache: rootShardKey -> ShardDecodeCacheEntry. Guarded by its own mutex.
    /// A token mismatch (any write to the shard) forces a fresh get + decode; cache entries are
    /// otherwise reused across reads within and across queries. Bounded (wholesale clear on overflow,
    /// like the tree cache) so a long-lived server that touches many tables/backups/detached dirs
    /// cannot grow it without limit; `dropNamespace` also evicts a dropped namespace's shard entries
    /// explicitly (they would otherwise never be re-read — leak).
    /// validated_at supports the Pillar B TTL fast-path (Task 3): staleness-tolerant callers skip the
    /// HEAD when the entry was validated within shard_decode_cache_ttl_ms. Absence is NEVER TTL-cached
    /// — only present entries carry a validated_at stamp.
    struct ShardDecodeCacheEntry
    {
        Token token;
        std::shared_ptr<const RootShard> shard;
        std::chrono::steady_clock::time_point validated_at;
    };
    static constexpr size_t SHARD_DECODE_CACHE_MAX_ENTRIES = 16384;
    std::mutex shard_decode_cache_mutex;
    std::unordered_map<String, ShardDecodeCacheEntry> shard_decode_cache;

    /// Per-shard-key monotonic write counter, guarded by shard_decode_cache_mutex. Bumped on every
    /// committed manifest write (the mutateShard invalidation, alongside the cache erase). A reader
    /// captures it BEFORE its get() and re-checks before populating shard_decode_cache: if a write
    /// landed during the get(), the just-decoded bytes may already be superseded AND that write's
    /// invalidation erase has already run, so caching the decode would resurrect a stale entry that
    /// the TTL fast-path then serves — making a just-published ref look absent (read-your-writes
    /// coherence race, B157). On seq mismatch the reader returns its own point-in-time decode but
    /// does NOT cache it. Kept monotonic across the wholesale cache clear (never reset) so a captured
    /// value can never spuriously match a reset counter. Bounded by distinct (namespace, shard) pairs.
    std::unordered_map<String, uint64_t> shard_write_seq;

    /// Single-flight coalescing for readShardDecoded: concurrent resolves of the SAME shard key
    /// share ONE head (+ at most one get). The leader publishes its decode to the followers'
    /// shared_future. Zero added staleness — all coalesced callers get the leader's fresh result.
    std::mutex shard_inflight_mutex;
    std::unordered_map<String, std::shared_future<std::shared_ptr<const RootShard>>> shard_inflight;

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

    /// NOTE (M-C2): the manifest journal is never trimmed here — trimming needs folded_cursor
    /// (INV-JOURNAL-COVERAGE), which is GC state landing in M-C3; the manifest size guard
    /// (soft warn / hard throw, in the publish/drop CAS loop) bounds growth meanwhile.

    /// B164b: injectable delay hook for backpressure tests. In production, stores use the default
    /// (std::this_thread::sleep_for). Tests replace this with a no-op counter hook.
    std::function<void(std::chrono::milliseconds)> backpressure_delay_hook;
};

}
