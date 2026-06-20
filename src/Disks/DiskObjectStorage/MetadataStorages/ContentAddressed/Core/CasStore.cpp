#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Common/thread_local_rng.h>
#include <city.h>
#include <algorithm>
#include <unordered_set>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LIMIT_EXCEEDED;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

/// Runaway brake on CAS/conditional-write retry loops. These keys are single-owner (one writer per
/// namespace by construction), so a contention storm is impossible — the bound only catches a
/// pathological live-lock and never a legitimate steady state.
static constexpr size_t MAX_CAS_ATTEMPTS = 100;

Store::Store(BackendPtr backend_, PoolConfig config_, PoolMeta meta_)
    : pool_backend(std::move(backend_))
    , config(std::move(config_))
    , meta(std::move(meta_))
    , pool_layout(config.pool_prefix)
    , retire_view(pool_backend, pool_layout)
{
    if (config.dedup_cache_bytes > 0)
        dedup_cache = std::make_unique<DedupCache>(
            "LRU", CurrentMetrics::end(), CurrentMetrics::end(),
            config.dedup_cache_bytes, DedupCache::NO_MAX_COUNT, DedupCache::DEFAULT_SIZE_RATIO);
}

bool Store::dedupCacheContains(const UInt128 & blob_hash) const
{
    return dedup_cache && dedup_cache->contains(blob_hash);
}

void Store::dedupCacheAdd(const UInt128 & blob_hash)
{
    if (dedup_cache)
        dedup_cache->set(blob_hash, std::make_shared<DedupPresent>());
}

StorePtr Store::open(BackendPtr backend, PoolConfig config)
{
    /// B168 P0: wrap the pool backend once, transparently, so EVERY CA S3 op — probe, pool-meta,
    /// writer, GC, watermark — flows through the per-namespace/op ProfileEvents chokepoint. The
    /// decorator only delegates and counts; it changes no behavior (read-only opens stay write-free).
    backend = std::make_shared<InstrumentedBackend>(std::move(backend));

    /// FAIL-CLOSED (design §6): the capability probe throws NOT_IMPLEMENTED on any failed check, and
    /// PoolMeta::createOrValidate is pool-authoritative — the config constants apply only at creation.
    Layout layout(config.pool_prefix);
    /// The probe writes and deletes throwaway keys to verify conditional-op enforcement. A read-only
    /// open must never mutate the pool it inspects; fsck only reads, so skip it. (Pool meta below is
    /// read-only when the pool already exists; a missing pool meta on a read-only backend fails closed.)
    if (!config.read_only)
    {
        /// B135: give each mount a PER-MOUNT UNIQUE probe key prefix so two servers mounting the SAME
        /// shared pool concurrently never collide on the (formerly fixed) `<pool>/_probe/token` /
        /// `<pool>/_probe/cas` keys. Without this, the loser of the `putIfAbsent` race aborts startup
        /// with PreconditionFailed (and the winner's cleanup delete can cascade into the loser). With a
        /// fresh random 128-bit id per `Store::open`, each mounter validates conditional-op support
        /// independently. A crashed mount leaves harmless `_probe/<rand>/...` debris under the `_probe/`
        /// namespace only (never the content planes) — acceptable.
        const UInt128 probe_uid = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
        runCapabilityProbe(*backend, config.pool_prefix + "/_probe/" + u128ToHex(probe_uid));
    }
    PoolMeta meta = PoolMeta::createOrValidate(*backend, layout, config.root_shards, config.blob_header_len);

    /// Private ctor: make_shared cannot reach it.
    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));

    /// Prime the writer-side retire view once (rare by construction; see RetireView).
    store->retire_view.refresh();

    /// Per-server watermark (spec 2026-06-16-ca-build-watermark). process_epoch is a random NONZERO
    /// value minted once per Store: GC checks it for equality only (a different epoch == a dead
    /// incarnation). It rides through the watermark JSON codec, which parses integers as Int64 and
    /// caps at 2^53 (the JSON-number interop bound — see requireU64Var / the min_active comment), so
    /// the epoch MUST stay in that range: a full-u64 draw would round-trip-fail to decode roughly
    /// half the time. Mask to 52 bits (collision-safe for an equality-only token) and avoid the 0
    /// sentinel (UINT64_MAX is the retired sentinel, also distinct from a live epoch).
    constexpr uint64_t EPOCH_MASK = (1ULL << 52) - 1;
    store->process_epoch = (thread_local_rng() ^ (static_cast<uint64_t>(thread_local_rng()) << 32)) & EPOCH_MASK;
    if (store->process_epoch == 0)
        store->process_epoch = 1;

    /// W-ANCHOR: the per-server watermark must be durable BEFORE any object PUT. A read-only open
    /// must never mutate the pool (the probe is skipped above for the same reason), so the watermark
    /// — which writes the roots/<server-hex>/_watermark slot — is only constructed and anchored on a writable open.
    if (!store->config.read_only)
    {
        Store * raw = store.get();
        store->watermark = std::make_unique<WatermarkKeeper>(
            store->pool_backend, store->pool_layout, store->config.server_id, store->process_epoch,
            [raw] { return raw->minActive(); });
        store->watermark->start();
        if (store->config.background_heartbeats)
            store->watermark->startBackground(store->config.heartbeat_period);
    }

    return store;
}

Store::~Store()
{
    /// Stop the background renewal cleanly. We deliberately do NOT call farewell here: there is no
    /// clean-shutdown hook plumbed through to Store dtor, and farewell would retire the epoch even on
    /// a transient drop. The crash/shutdown path (a frozen seq) is handled by GC's frozen-seq
    /// liveness detection (spec 2026-06-16-ca-build-watermark), so stopBackground alone is correct.
    if (watermark)
        watermark->stopBackground();
}

uint64_t Store::shardOf(const String & ref_name) const
{
    /// root_shards >= 1 is a PoolMeta invariant, so the modulus is always well-defined.
    return CityHash_v1_0_2::CityHash64(ref_name.data(), ref_name.size()) % meta.root_shards;
}

void Store::casPutObject(const String & full_key, const String & bytes)
{
    /// head + putIfAbsent/putOverwrite loop. Single-owner keys make a contention storm impossible;
    /// the bound is a runaway brake.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        HeadResult head = pool_backend->head(full_key);
        if (!head.exists)
        {
            if (pool_backend->putIfAbsent(full_key, bytes) == PutOutcome::Done)
                return;
        }
        else
        {
            if (pool_backend->putOverwrite(full_key, bytes, head.token) == PutOutcome::Done)
                return;
        }
        /// PreconditionFailed ⇒ the observed state changed under us; re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}'", full_key);
}

std::optional<String> Store::casGetObject(const String & full_key)
{
    std::optional<GetResult> result = pool_backend->get(full_key);
    if (!result)
        return std::nullopt;
    return result->bytes;
}

void Store::casRemoveObject(const String & full_key)
{
    /// head + deleteExact loop; no-op when absent. Single-owner keys; the bound is a runaway brake.
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        const HeadResult head = pool_backend->head(full_key);
        if (!head.exists)
            return;
        const DeleteOutcome outcome = pool_backend->deleteExact(full_key, head.token);
        if (outcome.kind == DeleteOutcome::Kind::Deleted || outcome.kind == DeleteOutcome::Kind::NotFound)
            return;
        /// TokenMismatch: a concurrent rewrite — re-head and retry.
    }
    throw Exception(ErrorCodes::ABORTED, "object CAS contention on '{}' (runaway live-lock brake)", full_key);
}

void Store::putNamespaceFile(const RootNamespace & ns, const String & name, const String & bytes)
{
    casPutObject(pool_layout.namespaceFileKey(ns, name), bytes);
}

std::optional<String> Store::getNamespaceFile(const RootNamespace & ns, const String & name)
{
    return casGetObject(pool_layout.namespaceFileKey(ns, name));
}

std::vector<String> Store::listNamespaceFiles(const RootNamespace & ns)
{
    const String prefix = pool_layout.namespaceFilesPrefix(ns);
    std::vector<String> names;
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            /// Strip the prefix to yield the bare flat file name.
            if (listed.key.size() >= prefix.size() && listed.key.compare(0, prefix.size(), prefix) == 0)
                names.push_back(listed.key.substr(prefix.size()));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    /// The InMemoryBackend lists sorted, but sort explicitly to stay backend-agnostic.
    std::sort(names.begin(), names.end());
    return names;
}

uint64_t Store::minActive()
{
    std::lock_guard lk(builds_mutex);
    return active_build_seqs.empty() ? next_build_seq : *active_build_seqs.begin();
}

uint64_t Store::peekNextBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    return next_build_seq;
}

void Store::renewWatermarkOnce()
{
    /// A read-only open never anchored a watermark; there is nothing to renew (fail closed rather
    /// than fabricate one).
    if (!watermark)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS watermark: renewWatermarkOnce on a read-only Store");
    watermark->renewOnce();
}

uint64_t Store::allocateBuildSeq()
{
    std::lock_guard lk(builds_mutex);
    const uint64_t s = next_build_seq++;
    active_build_seqs.insert(s);
    return s;
}

void Store::retireBuildSeq(uint64_t seq)
{
    std::lock_guard lk(builds_mutex);
    active_build_seqs.erase(seq);
}

BuildPtr Store::startBuild(BuildInfo info)
{
    /// Mint a globally-unique build id from two thread_local_rng draws (random u128).
    const UInt64 hi = thread_local_rng();
    const UInt64 lo = thread_local_rng();
    const UInt128 build_id = (static_cast<UInt128>(hi) << 64) | lo;

    /// Strictly-increasing per-process build_seq carried by the Build (spec 2026-06-16). The Build is
    /// added to the active set here and retired on publish/abandon/dtor, so minActive — the GC floor
    /// the Store-owned watermark renews — tracks in-flight builds.
    const uint64_t seq = allocateBuildSeq();

    /// W-HEARTBEAT: the heartbeat must be durable BEFORE the build returns (and thus before any object
    /// PUT). start does the durable-first putIfAbsent; background renewal is opt-in (tests drive
    /// renewOnce explicitly). The per-build heartbeat is retained alongside the new per-server
    /// watermark (it still gates full-GC debris); collapsing the two is a separate future cleanup.
    auto keeper = std::make_unique<HeartbeatKeeper>(pool_backend, pool_layout, build_id, config.server_id);
    keeper->start();
    if (config.background_heartbeats)
        keeper->startBackground(config.heartbeat_period);

    return std::make_shared<Build>(shared_from_this(), std::move(keeper), build_id, seq, process_epoch, std::move(info));
}

std::shared_ptr<const RootShard> Store::readShardDecoded(const RootNamespace & ns, uint64_t shard, bool allow_stale)
{
    const String key = pool_layout.rootShardKey(ns, shard);

    /// Pillar B TTL fast-path: a staleness-tolerant caller may reuse a recently-validated decode
    /// WITHOUT a HEAD. Only for PRESENT entries (absence is never TTL-cached — a just-created ref
    /// must be observable by force-fresh callers; staleness-tolerant callers re-validate on miss).
    if (allow_stale && config.shard_decode_cache_ttl_ms.count() > 0)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end())
        {
            const auto age = std::chrono::steady_clock::now() - it->second.validated_at;
            if (age < config.shard_decode_cache_ttl_ms)
                return it->second.shard;
        }
    }

    return coalescedReadShardDecoded(key);
}

std::shared_ptr<const RootShard> Store::coalescedReadShardDecoded(const String & key)
{
    std::shared_ptr<std::promise<std::shared_ptr<const RootShard>>> promise;
    std::shared_future<std::shared_ptr<const RootShard>> future;
    {
        std::lock_guard lock(shard_inflight_mutex);
        auto it = shard_inflight.find(key);
        if (it != shard_inflight.end())
        {
            future = it->second;   /// follower: wait on the in-flight leader
        }
        else
        {
            promise = std::make_shared<std::promise<std::shared_ptr<const RootShard>>>();
            future = promise->get_future().share();
            shard_inflight.emplace(key, future);
        }
    }

    if (!promise)
        return future.get();   /// follower: returns the leader's result (or rethrows the leader's exception)

    /// Leader: do the real work, publish to followers whether it succeeds or throws.
    try
    {
        auto result = loadShardDecoded(key);
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_value(result);
        return result;
    }
    catch (...)
    {
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_exception(std::current_exception());
        throw;
    }
}

std::shared_ptr<const RootShard> Store::loadShardDecoded(const String & key)
{
    /// Empty-manifest sentinel for the absent case — shared so callers can treat absent and present
    /// uniformly (no refs). Never mutated.
    static const std::shared_ptr<const RootShard> empty_shard = std::make_shared<const RootShard>();

    /// A `head` gets the current token without transferring/decoding the manifest body.
    const HeadResult h = pool_backend->head(key);
    if (!h.exists)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }

    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end() && it->second.token == h.token)
        {
            /// Token match: the cached decode is still valid. Stamp validated_at so TTL-tolerant
            /// callers can skip the next HEAD within the configured window.
            it->second.validated_at = std::chrono::steady_clock::now();
            return it->second.shard;
        }
    }

    /// Miss (or the shard was written since we last decoded it): fetch + decode once, then cache by
    /// the token the bytes actually came from (NOT the head token — a write could have landed in
    /// between; we cache what we decoded, which is self-consistent).
    ///
    /// Capture the per-key write counter BEFORE the get(): a committed write that bumps it while our
    /// get() is in flight means the bytes we are about to fetch may predate that write — and the
    /// write's invalidation erase has already run — so caching the decode now would poison the TTL
    /// fast-path with a stale entry the erase can no longer remove (read-your-writes coherence, B157).
    uint64_t seq_before_get;
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        seq_before_get = shard_write_seq[key];
    }

    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
    {
        /// Raced a deletion between head and get — treat as absent.
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }
    auto decoded = std::make_shared<const RootShard>(decodeRootShard(object->bytes));
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        /// Skip caching if a write committed during our get() (B157): the decode may be superseded
        /// and its invalidation erase has already run, so populating now would resurrect a stale
        /// entry. We still RETURN the decode to our own caller — a point-in-time view, acceptable
        /// for allow_stale; a force-fresh caller's own writes happen-before its head() so this only
        /// ever under-caches, never serves a caller its own stale write.
        if (shard_write_seq[key] == seq_before_get)
        {
            /// Bound memory on a long-lived server: a wholesale clear is fine — entries re-populate
            /// on demand (a stale entry would be re-validated by the head token check anyway).
            if (shard_decode_cache.size() >= SHARD_DECODE_CACHE_MAX_ENTRIES)
                shard_decode_cache.clear();
            shard_decode_cache[key] = ShardDecodeCacheEntry{
                .token = object->token,
                .shard = decoded,
                .validated_at = std::chrono::steady_clock::now()};
        }
    }
    return decoded;
}

std::optional<Resolved> Store::resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale)
{
    /// Read side (spec §6): no GC awareness, no tokens. The ref is pure manifest state — resolving it
    /// only reads the owning shard manifest; whether the named tree object is still present is checked
    /// later by readTree (INV-NO-DANGLE surfaces there).
    const auto root = readShardDecoded(ns, shardOf(ref_name), allow_stale);
    auto it = root->refs.find(ref_name);
    if (it == root->refs.end())
        return std::nullopt;

    const RefPayload & payload = it->second;
    /// B170: a ref resolved to its tree (the read-path entry point). object_hash is the tree the
    /// ref names; pairs with a later readTree ReadMissing/DanglingAccess if that tree is gone.
    if (hasEventSink())
    {
        CasEvent _ev0;
        _ev0.type = CasEventType::RefResolve;
        _ev0.namespace_ = ns.string();
        _ev0.ref_name = ref_name;
        _ev0.object_kind = CasEventObjectKind::Tree;
        _ev0.object_hash = u128ToHex(payload.tree_id);
        _ev0.outcome = "resolved";
        _ev0.reason = "read-side resolve of a ref to its tree";
        emitEvent(_ev0);
    }
    return Resolved{
        .tree_id = TreeId(u128ToHex(payload.tree_id)),
        .tree_size = payload.tree_size,
        .mutable_files = payload.mutable_files,
    };
}

std::vector<TreeEntry> Store::readTree(const TreeId & id)
{
    /// B113: trees are content-addressed (immutable), so a cached decode is always valid — no token,
    /// no invalidation. The read path resolves `route` per file, hitting the same tree repeatedly.
    {
        std::lock_guard lock(tree_cache_mutex);
        auto it = tree_cache.find(id.string());
        if (it != tree_cache.end())
            return *it->second;
    }

    /// A live ref naming a missing tree object is a storage invariant violation (INV-NO-DANGLE): surface
    /// it, never substitute an empty tree or any default. Readers have no condemnation awareness — a
    /// present-but-condemned object reads fine here.
    std::optional<GetResult> object = pool_backend->get(pool_layout.treeKey(id));
    if (!object)
    {
        /// B170: a live ref named a tree whose object is gone — INV-NO-DANGLE surfaced on the read
        /// path (the dangling-access anomaly). Record before failing closed.
        if (hasEventSink())
        {
            CasEvent _ev1;
            _ev1.type = CasEventType::ReadMissing;
            _ev1.object_kind = CasEventObjectKind::Tree;
            _ev1.object_hash = id.string();
            _ev1.outcome = "missing";
            _ev1.reason = "live ref names tree but its object is missing (INV-NO-DANGLE)";
            _ev1.detail = {{"code", "FILE_DOESNT_EXIST"}, {"site", "readTree"}};
            emitEvent(_ev1);
        }
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "live ref names tree {} but its object is missing — INV-NO-DANGLE", id.string());
    }

    /// Validate the envelope (magic / kind=Tree / header_hash / size arithmetic), then the key↔hash
    /// binding: a tree stored at a key other than hex(logical_hash) is corruption.
    const EnvelopeHeader header = decodeEnvelopeHeader(object->bytes, object->bytes.size(), ObjectKind::Tree);
    if (u128ToHex(header.logical_hash) != id.string())
    {
        /// B170: the tree object decoded but its content hash does not match its key — corruption.
        if (hasEventSink())
        {
            CasEvent _ev2;
            _ev2.type = CasEventType::CorruptDecode;
            _ev2.object_kind = CasEventObjectKind::Tree;
            _ev2.object_hash = id.string();
            _ev2.outcome = "corrupt";
            _ev2.reason = "tree key/hash mismatch: object carries a different logical_hash";
            _ev2.detail = {{"code", "CORRUPTED_DATA"}, {"site", "readTree"},
                       {"carried_hash", u128ToHex(header.logical_hash)}};
            emitEvent(_ev2);
        }
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS tree key/hash mismatch: object at tree key {} carries logical_hash {}",
            id.string(), u128ToHex(header.logical_hash));
    }

    auto decoded = std::make_shared<const std::vector<TreeEntry>>(
        decodeTree(std::string_view(object->bytes).substr(payloadOffset(header))));
    {
        std::lock_guard lock(tree_cache_mutex);
        /// Bound memory: a wholesale clear on overflow is fine — the entries are pure decode caches
        /// that re-populate on demand (immutable content, no correctness dependence on residency).
        if (tree_cache.size() >= TREE_CACHE_MAX_ENTRIES)
            tree_cache.clear();
        tree_cache[id.string()] = decoded;
    }
    return *decoded;
}

BlobLocation Store::locate(const TreeEntry & entry) const
{
    /// A ranged read into the content object: the payload starts at a constant offset for blobs
    /// (the pool's fixed blob_header_len — no per-object header read), and at the slice offset for
    /// pack slices. Inline/Subtree carry no standalone object location.
    switch (entry.placement)
    {
        case Placement::Blob:
            return BlobLocation{
                .key = pool_layout.blobKey(BlobId(u128ToHex(entry.file_hash))),
                .offset = meta.blob_header_len,
                .length = entry.file_size,
            };
        case Placement::PackSlice:
            /// Encoded and validated from day one; produced by nobody until packing lands in M-F.
            return BlobLocation{
                .key = pool_layout.packKey(PackId(u128ToHex(entry.pack_hash))),
                .offset = entry.pack_offset,
                .length = entry.pack_length,
            };
        case Placement::Inline:
        case Placement::Subtree:
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "entry placement {} has no blob location", static_cast<int>(entry.placement));
    }
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "entry placement {} has no blob location", static_cast<int>(entry.placement));
}

std::map<String, Resolved> Store::listRefs(const RootNamespace & ns)
{
    /// Refs are sharded by name across all root shards; the full ref set is the union over every shard.
    /// Listing tolerates a point-in-time snapshot: pass allow_stale=true to benefit from the TTL
    /// fast-path and skip a HEAD per shard when a recent decode is already cached.
    std::map<String, Resolved> result;
    for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
    {
        const auto root = readShardDecoded(ns, shard, /*allow_stale=*/true);
        for (const auto & [ref_name, payload] : root->refs)
        {
            result.emplace(ref_name, Resolved{
                .tree_id = TreeId(u128ToHex(payload.tree_id)),
                .tree_size = payload.tree_size,
                .mutable_files = payload.mutable_files,
            });
        }
    }
    return result;
}

void Store::mutateShard(const RootNamespace & ns, uint64_t shard, std::function<void(RootShard &)> mutate,
                        uint64_t * out_committed_version)
{
    const String key = pool_layout.rootShardKey(ns, shard);
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        /// Re-read inside the loop so `mutate` always edits the FRESH manifest: on a Conflict retry the
        /// previous attempt's edits are discarded and re-applied to the winner's state, so a journal
        /// append is never double-appended.
        auto [root, token] = readShard(ns, shard);
        mutate(root);
        ++root.shard_version;
        String body = encodeRootShard(root);

        /// Manifest size guard (spec §4): soft ⇒ warn (still commit), hard ⇒ refuse the write.
        if (body.size() >= config.manifest_hard_limit)
            throw Exception(ErrorCodes::LIMIT_EXCEEDED,
                "manifest {} size {} reached hard limit {}", key, body.size(), config.manifest_hard_limit);
        if (body.size() >= config.manifest_soft_limit)
            LOG_WARNING(getLogger("CasStore"),
                "manifest {} size {} crossed soft limit {}", key, body.size(), config.manifest_soft_limit);

        if (pool_backend->casPut(key, body, token) == CasOutcome::Committed)
        {
            /// Read-your-writes (Pillar B): invalidate this shard's decode cache so a same-Store
            /// allow_stale read (bounded-TTL fast-path) cannot serve the pre-write decode. A LOCAL
            /// write is reflected immediately; cross-node staleness stays bounded by the TTL.
            /// Bumping shard_write_seq under the SAME lock fences any in-flight reader (B157): a
            /// reader whose get() straddled this casPut sees the seq change before it populates and
            /// declines to cache its now-superseded decode (which the erase below could not remove).
            {
                std::lock_guard cache_lock(shard_decode_cache_mutex);
                ++shard_write_seq[key];
                shard_decode_cache.erase(key);
            }
            if (out_committed_version)
                *out_committed_version = root.shard_version;
            return;
        }
        /// Conflict ⇒ someone committed under us; re-read and re-apply the whole mutate.
    }
    throw Exception(ErrorCodes::ABORTED, "manifest CAS contention on {}", key);
}

void Store::dropRef(const RootNamespace & ns, const String & ref_name)
{
    /// Drop = the same CAS shape as publish (spec §5 step 6): remove refs[name], append {-, name, T}.
    UInt128 dropped_tree{};
    uint64_t at_version = 0;
    mutateShard(ns, shardOf(ref_name), [&](RootShard & root)
    {
        auto it = root.refs.find(ref_name);
        if (it == root.refs.end())
            /// Fail-closed (no silent no-op): the throw propagates out of mutateShard and aborts the drop.
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "dropRef: no such ref {} in namespace {}", ref_name, ns.string());

        const UInt128 tree_id = it->second.tree_id;
        dropped_tree = tree_id;
        at_version = root.shard_version + 1;
        root.refs.erase(it);
        /// The at_version is the NEW shard_version this attempt commits — the helper bumps AFTER mutate,
        /// so here the post-commit version is root.shard_version + 1.
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Remove, .ref_name = ref_name, .tree_id = tree_id,
            .at_version = root.shard_version + 1});
    });
    /// B170: the ref was dropped (a '-' journal record GC will fold as a root Remove). object_hash is
    /// the tree the ref named, so a part's "publish -> drop" life is reconstructable from the rows.
    if (hasEventSink())
    {
        CasEvent _ev3;
        _ev3.type = CasEventType::RefDrop;
        _ev3.namespace_ = ns.string();
        _ev3.ref_name = ref_name;
        _ev3.object_kind = CasEventObjectKind::Tree;
        _ev3.object_hash = u128ToHex(dropped_tree);
        _ev3.at_version = at_version;
        _ev3.outcome = "ok";
        _ev3.reason = "dropRef: removed the ref and appended a Remove record";
        emitEvent(_ev3);
    }
}

void Store::updateRefPayload(const RootNamespace & ns, const String & ref_name,
                             std::function<void(RefPayload &)> mutator)
{
    /// Mutable-fields-only update (design §3): no reachability change ⇒ NO journal record.
    mutateShard(ns, shardOf(ref_name), [&](RootShard & root)
    {
        auto it = root.refs.find(ref_name);
        if (it == root.refs.end())
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "updateRefPayload: no such ref {} in namespace {}", ref_name, ns.string());

        const UInt128 old_tree_id = it->second.tree_id;
        const uint64_t old_tree_size = it->second.tree_size;

        RefPayload payload = it->second;
        mutator(payload);

        /// A reachability change is not allowed on this path: it would need a journal record (use
        /// publish/drop instead). Throwing here aborts before casPut — the manifest stays untouched.
        if (payload.tree_id != old_tree_id || payload.tree_size != old_tree_size)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "updateRefPayload must not change tree_id/tree_size; use publish/drop");

        it->second = std::move(payload);
    });
}

void Store::removeNamespaceFile(const RootNamespace & ns, const String & name)
{
    casRemoveObject(pool_layout.namespaceFileKey(ns, name));
}

void Store::putMountpointObject(const String & key, const String & bytes)
{
    casPutObject(pool_layout.mountpointObjectKey(key), bytes);
}

std::optional<String> Store::getMountpointObject(const String & key)
{
    return casGetObject(pool_layout.mountpointObjectKey(key));
}

void Store::removeMountpointObject(const String & key)
{
    casRemoveObject(pool_layout.mountpointObjectKey(key));
}

void Store::dropNamespace(const RootNamespace & ns)
{
    /// Tombstone every TOUCHED shard, then delete the verbatim files. GC removes the manifest OBJECTS
    /// themselves later (M-C3); here we only clear refs + journal the removals (spec §4: untouched
    /// shards stay absent — we never mint a manifest just to hold a tombstone).
    for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
    {
        const auto [root, token] = readShard(ns, shard);
        if (!token)
            continue;                       /// absent shard: stays absent, no manifest minted
        if (root.refs.empty())
            /// OPTIMIZATION, not a correctness guard: skip an empty snapshot to avoid a no-op CAS.
            /// Single-writer-per-namespace makes this snapshot safe to trust; even if it were racy,
            /// mutateShard re-reads the manifest inside its loop, so correctness never rests here.
            continue;

        mutateShard(ns, shard, [](RootShard & shard_root)
        {
            /// Append one Remove per former ref (iterate before clearing), then clear all refs.
            for (const auto & [ref_name, payload] : shard_root.refs)
                shard_root.journal.push_back(JournalRecord{
                    .op = JournalRecord::Op::Remove, .ref_name = ref_name, .tree_id = payload.tree_id,
                    .at_version = shard_root.shard_version + 1});
            shard_root.refs.clear();
        });
    }

    /// Delete the verbatim files: list → head for the token → deleteExact (single-owner, bounded retry).
    const String prefix = pool_layout.namespaceFilesPrefix(ns);
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(prefix, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
            {
                HeadResult head = pool_backend->head(listed.key);
                if (!head.exists)
                    break;                  /// NotFound ⇒ already gone (idempotent)
                DeleteOutcome outcome = pool_backend->deleteExact(listed.key, head.token);
                if (outcome.kind == DeleteOutcome::Kind::Deleted
                    || outcome.kind == DeleteOutcome::Kind::NotFound)
                    break;
                /// TokenMismatch ⇒ the object changed under us; re-head and retry.
                if (attempt + 1 == MAX_CAS_ATTEMPTS)
                    throw Exception(ErrorCodes::ABORTED,
                        "verbatim file delete contention on {}", listed.key);
            }
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }

    /// Evict this namespace's shard entries from the read-path decode cache: the namespace is gone,
    /// so they would never be re-read (and thus never revalidated/replaced) — a slow leak otherwise.
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        for (uint64_t shard = 0; shard < meta.root_shards; ++shard)
            shard_decode_cache.erase(pool_layout.rootShardKey(ns, shard));
    }
}

std::pair<RootShard, std::optional<Token>> Store::readShard(const RootNamespace & ns, uint64_t shard)
{
    /// An absent shard manifest means the shard holds no refs yet — a fresh, empty manifest with no
    /// token (nothing to CAS against). This is normal state, NOT a fallback masking an error.
    std::optional<GetResult> object = pool_backend->get(pool_layout.rootShardKey(ns, shard));
    if (!object)
        return {RootShard{}, std::nullopt};
    return {decodeRootShard(object->bytes), object->token};
}

uint64_t Store::ensureRegistered(const RootNamespace & ns)
{
    pool_layout.rootShardKey(ns, 0);   /// validate the namespace string early (checkNamespace)

    {
        std::lock_guard lock(registered_mutex);
        if (registered_cache.contains(ns.string()))
            /// Already registered: no gate floor needed — R3 fences ALL shards of every registered
            /// namespace each round (minting fence-only manifests for absent ones), so the per-shard
            /// fence carries the publish ordering from here on.
            return 0;
    }

    const String key = pool_layout.rootsRegistryKey();
    for (size_t attempt = 0; attempt < MAX_CAS_ATTEMPTS; ++attempt)
    {
        const std::optional<GetResult> got = pool_backend->get(key);
        RootsRegistry registry;
        if (got)
            registry = decodeRootsRegistry(got->bytes);

        if (registry.namespaces.contains(ns.string()))
        {
            /// Another writer (or our own crashed attempt) registered it. Return the observed
            /// fence_round anyway — for a FIRST publish of THIS Store the floor is cheap and
            /// strictly conservative (the round-R fence may not have minted this namespace's
            /// shards yet if registration landed after R3 began).
            std::lock_guard lock(registered_mutex);
            registered_cache.insert(ns.string());
            return registry.fence_round;
        }

        registry.namespaces.insert(ns.string());
        ++registry.registry_version;
        const CasOutcome outcome = got
            ? pool_backend->casPut(key, encodeRootsRegistry(registry), got->token)
            : pool_backend->casPut(key, encodeRootsRegistry(registry), std::nullopt);
        if (outcome == CasOutcome::Committed)
        {
            std::lock_guard lock(registered_mutex);
            registered_cache.insert(ns.string());
            /// W-REGISTER gate floor: the fence_round READ in the committed attempt. An append that
            /// lands BELOW a round's registry fence is folded into that round's discovery; an
            /// append AFTER it observed fence_round >= round here — the caller's publish gate must
            /// refresh its retire view to at least this round before committing (spec §5).
            return registry.fence_round;
        }
        /// Conflict: a racing registration or the GC fence moved the registry — re-read and retry.
    }
    throw Exception(ErrorCodes::ABORTED,
        "CAS namespace registration contention on {} (runaway live-lock brake)", key);
}

std::vector<String> Store::listNamespaces(const String & prefix)
{
    /// One registry GET — the authoritative namespace universe (W-REGISTER: every published
    /// namespace is here; never LIST). Absent registry = fresh pool = no namespaces. The wiring
    /// uses this for directory-style enumeration of opaque namespace strings (e.g. the FREEZE
    /// shadow tree); registrations of dropped namespaces linger until full GC (M-F) — callers
    /// resolve refs per namespace, so a lingering empty namespace is visible-but-empty, not wrong.
    std::vector<String> result;
    if (const std::optional<GetResult> got = pool_backend->get(pool_layout.rootsRegistryKey()))
    {
        const RootsRegistry registry = decodeRootsRegistry(got->bytes);
        for (const String & ns : registry.namespaces)
            if (ns.starts_with(prefix))
                result.push_back(ns);
    }
    return result;
}

std::vector<String> Store::listMirroredChildren(const String & prefix)
{
    /// Loose LIST of the mirrored subtree under `roots/<prefix>` (design §5.3). Returns the
    /// distinct next-path-segment names. NOT authoritative — callers must re-check `listRefs`
    /// per candidate before surfacing it. GC continues to use the compact registry.
    const String full = pool_layout.rootsPrefix() + prefix;   /// e.g. <pool>/roots/shadow/
    std::unordered_set<String> children;
    String cursor;
    while (true)
    {
        ListPage page = pool_backend->list(full, cursor, /*limit*/ 1000);
        for (const ListedKey & listed : page.keys)
        {
            const String & key = listed.key;
            if (!key.starts_with(full))
                continue;
            const std::string_view rest(key.data() + full.size(), key.size() - full.size());
            const size_t slash = rest.find('/');
            const std::string_view seg = slash == std::string_view::npos ? rest : rest.substr(0, slash);
            if (!seg.empty())
                children.emplace(seg);
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return {children.begin(), children.end()};
}

}
