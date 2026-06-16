#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>

namespace DB::Cas
{

struct PoolConfig
{
    String pool_prefix;
    UInt128 server_id{};                      /// provenance + heartbeats
    uint64_t root_shards = 8;                 /// creation-time only; pool is authoritative on reopen
    uint64_t blob_header_len = 256;           /// creation-time only; ditto
    uint64_t manifest_soft_limit = 16ULL << 20;
    uint64_t manifest_hard_limit = 64ULL << 20;
    std::chrono::milliseconds heartbeat_period{5000};
    bool background_heartbeats = false;       /// tests drive renewOnce explicitly
    bool read_only = false;                   /// observe-only open: skip the mutating capability probe; reads only

    /// Pillar B bounded-TTL decode cache: a staleness-tolerant caller (allow_stale=true) may reuse a
    /// decode validated < this many ms ago WITHOUT a HEAD. 0 disables the TTL (all callers force-fresh).
    /// Strict-freshness callers always pass allow_stale=false and always HEAD, regardless of this value.
    std::chrono::milliseconds shard_decode_cache_ttl_ms{200};
};

struct Resolved
{
    TreeId tree_id;
    uint64_t tree_size = 0;
    std::map<String, String> mutable_files;
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
    ProvenanceOp op = ProvenanceOp::Other;
};

class Build;
using BuildPtr = std::shared_ptr<Build>;
class Gc;
class Store;
using StorePtr = std::shared_ptr<Store>;

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
    /// The GC floor: the oldest in-flight build_seq, or next_build_seq when no build is active (so a
    /// quiescent server's watermark floor advances to the next-to-be-allocated seq). Locks builds_mutex.
    uint64_t minActive();
    /// Test/assertion accessor for the next-to-allocate build_seq under the lock.
    uint64_t peekNextBuildSeq();
    /// Renew the per-server watermark once (bump seq, refresh min_active from the live active set).
    /// In production this is driven by the background renewer (background_heartbeats); tests with the
    /// renewer disabled drive it explicitly to make a finished build's floor advance durable.
    void renewWatermarkOnce();

    /// ---- write side ----
    BuildPtr startBuild(BuildInfo info);                          /// W-HEARTBEAT durable before return

    /// ---- read side (spec §6) ----
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false);
    std::vector<TreeEntry> readTree(const TreeId & id);           /// validates envelope, kind, key↔hash
    BlobLocation locate(const TreeEntry & entry) const;           /// Blob/PackSlice placements only
    std::map<String, Resolved> listRefs(const RootNamespace & ns);
    /// Registered namespaces with the given prefix (one registry GET; opaque strings, sorted).
    /// Dropped namespaces linger registered until full GC (M-F) — visible-but-empty, never wrong.
    std::vector<String> listNamespaces(const String & prefix);

    /// ---- ref lifecycle (CAS loops on the owning shard) ----
    void dropRef(const RootNamespace & ns, const String & ref_name);            /// refs−− + '-' journal, atomic
    void updateRefPayload(const RootNamespace & ns, const String & ref_name,
                          std::function<void(RefPayload &)> mutator);           /// mutable fields only; NO journal
    void dropNamespace(const RootNamespace & ns);                 /// tombstone every shard + delete verbatim files

    /// ---- verbatim namespace files (format_version.txt, ...) — plain keys, never content-addressed ----
    void putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes);
    std::optional<String> getNamespaceFile(const RootNamespace & ns, const String & name);
    std::vector<String> listNamespaceFiles(const RootNamespace & ns);
    /// Exact-token delete of one verbatim file (no-op when absent). Verbatim files are never
    /// content-addressed, so a mid-life delete (a pruned mutation entry, a stale tmp) must reclaim
    /// the object NOW - the reachability GC never scans them.
    void removeNamespaceFile(const RootNamespace & ns, const String & name);

    /// Internal surface for Build (same TU family; not for the wiring):
    const PoolConfig & poolConfig() const { return config; }
    const PoolMeta & poolMeta() const { return meta; }
    const Layout & layout() const { return pool_layout; }
    Backend & backend() { return *pool_backend; }
    RetireView & retireView() { return retire_view; }

    /// W-REGISTER (spec §5, decision 2026-06-12): CAS-append `ns` to roots/_registry if not yet
    /// present, BEFORE the namespace's first manifest is created — this orders namespace creation
    /// against the GC fence. Returns the registry's fence_round observed/committed in the
    /// registering attempt: the GATE FLOOR for a first publish into the namespace (a floor of 0
    /// when the namespace was already registered — the per-shard fence carries the ordering then,
    /// because R3 fences ALL shards of every registered namespace each round, minting fence-only
    /// manifests for absent ones). Registrations are monotone, so a hit in the in-memory cache
    /// short-circuits without I/O.
    uint64_t ensureRegistered(const RootNamespace & ns);

private:
    Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_);

    /// Allocate a strictly-increasing build_seq and add it to the active set (called by startBuild).
    uint64_t allocateBuildSeq();
    /// Remove a build_seq from the active set; idempotent (safe from publish/abandon/dtor).
    void retireBuildSeq(uint64_t seq);

    uint64_t shardOf(const String & ref_name) const;             /// CityHash64(ref_name) % root_shards
    /// Read shard manifest (absent ⇒ empty RootShard with no token); used by mutateShard (writes,
    /// which need the token for the CAS) and as the uncached primitive under readShardDecoded.
    std::pair<RootShard, std::optional<Token>> readShard(const RootNamespace & ns, uint64_t shard);

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
    /// applies the manifest size guard (soft ⇒ LOG_WARNING, hard ⇒ LIMIT_EXCEEDED), and casPut against the
    /// observed token (nullopt when the shard was absent — create-if-absent). On Conflict it re-reads and
    /// retries the WHOLE mutate, bounded (100) then ABORTED ("manifest CAS contention on {}"). Single-writer
    /// shards make a real storm impossible; the bound is a runaway brake. `mutate` runs on the FRESHLY READ
    /// root each attempt, so a journal append is never double-applied across retries.
    /// `out_committed_version` (optional) receives the shard_version the successful casPut committed —
    /// the GC fence (R3) records it as the durable per-shard fence position (the model's fencePos[s]).
    void mutateShard(const RootNamespace & ns, uint64_t shard, std::function<void(RootShard &)> mutate,
                     uint64_t * out_committed_version = nullptr);

    BackendPtr pool_backend;
    PoolConfig config;
    PoolMeta meta;
    /// pool_layout MUST precede retire_view: the ctor init list builds retire_view from pool_layout.
    Layout pool_layout;
    RetireView retire_view;

    /// Namespaces this Store has confirmed registered (monotone — registrations are never undone
    /// in M-C3; full GC removes a namespace with its final manifests in M-F).
    std::mutex registered_mutex;
    std::set<String> registered_cache;

    /// Per-server build watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random
    /// nonzero u64 minted once at open: GC checks it for EQUALITY (an object stamped with a different
    /// epoch is from a dead incarnation), never for ordering. next_build_seq is a strictly-increasing
    /// per-process counter (monotonicity is load-bearing — a seq is never reused or lowered);
    /// active_build_seqs holds the seqs of in-flight builds, so minActive yields the GC floor.
    /// watermark is the Store-owned per-server renewer (anchored before any object PUT in open).
    uint64_t process_epoch = 0;
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;
    std::set<uint64_t> active_build_seqs;
    std::unique_ptr<WatermarkKeeper> watermark;

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

    /// B113 (part 2) tree decode cache: tree_id_hex -> decoded immutable tree. Trees are
    /// content-addressed (the key IS the content hash), so entries never go stale — no invalidation.
    /// `route` resolves per FILE op, so the same part's tree was decoded O(files) times per read;
    /// caching makes it O(1). Bounded (cleared wholesale on overflow) to cap memory on a server that
    /// reads very many distinct parts.
    static constexpr size_t TREE_CACHE_MAX_ENTRIES = 16384;
    std::mutex tree_cache_mutex;
    std::unordered_map<String, std::shared_ptr<const std::vector<TreeEntry>>> tree_cache;

    /// NOTE (M-C2): the manifest journal is never trimmed here — trimming needs folded_cursor
    /// (INV-JOURNAL-COVERAGE), which is GC state landing in M-C3; the manifest size guard
    /// (soft warn / hard throw, in the publish/drop CAS loop) bounds growth meanwhile.
};

}
