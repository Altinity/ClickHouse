#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/ProfileEvents.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

namespace ProfileEvents
{
extern const Event CASGCCondemnMarkerUnconfirmedCarry;
extern const Event CASGCMetaWriteAnomaly;
extern const Event CASGCReadAheadWasted;
}

/// Fixtures for the condemn-marker gate: real GC rounds over a raw ref log, so a test decides which edges each
/// round folds; real writers where adoption is the subject; backends that act at one chosen request.
namespace DB::Cas::tests
{

inline const RootNamespace kGateNs{"00/aa@cas@"};
inline const UInt128 kGateLeaderA = hexToU128("000000000000000000000000000000a1");
inline const UInt128 kGateLeaderB = hexToU128("000000000000000000000000000000b2");

inline BlobRef gateRef(const UInt128 & hash)
{
    return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hash)};
}

/// A pool that folds every round, so a round's number is the number of rounds run.
template <typename BackendT>
PoolPtr openGatePool(std::shared_ptr<BackendT> backend, uint64_t io_concurrency = 1, uint64_t graduation_budget = 0,
                     std::function<void(const BlobRef &)> apply_hook = {}, uint64_t gc_shards = 1)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test",
                      .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = io_concurrency};
    config.gc_shards = gc_shards;
    config.gc_round_graduation_budget = graduation_budget;
    config.gc_redelete_apply_hook_for_test = std::move(apply_hook);
    return Pool::open(std::move(backend), std::move(config));
}

/// Publishes `ref_name` in `kGateNs` owning every blob of `hashes`, writing each missing body first.
/// `build_sequence` names the manifest, so it must differ between publications.
inline ManifestRef publishBlobs(Backend & backend, const Layout & layout, const String & ref_name,
                                uint64_t build_sequence, const std::vector<UInt128> & hashes)
{
    std::vector<ManifestEntry> entries;
    for (size_t i = 0; i < hashes.size(); ++i)
    {
        bool present = false;
        {
            OperationForTest op(backend);
            present = (*op).head(layout.blobKey(gateRef(hashes[i])), Retry::standard()).has_value();
        }
        if (!present)
            writeBlobBody(backend, layout, hashes[i]);
        entries.push_back(blobEntryFor("f" + std::to_string(i), hashes[i]));
    }
    const ManifestRef manifest{.writer_epoch = 1, .build_sequence = build_sequence, .manifest_ordinal = 1};
    writeManifestRaw(backend, layout, kGateNs, manifest, entries);
    publishCommittedTransition(backend, layout, kGateNs, ref_name, std::nullopt, manifest);
    return manifest;
}

inline void dropBlobs(Backend & backend, const Layout & layout, const String & ref_name, const ManifestRef & manifest)
{
    dropRefTransition(backend, layout, kGateNs, ref_name, manifest);
}

inline uint64_t committedGcRound(Backend & backend, const Layout & layout)
{
    OperationForTest op(backend);
    const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
    return got ? decodeGcState(got->bytes).round : 0;
}

/// The retired entry of `ref` in the adopted seal, looked up in the blob's own GC shard.
inline std::optional<RetiredEntry> retiredEntryFor(Backend & backend, const Layout & layout, const BlobRef & ref,
                                                   uint64_t gc_shards = 1)
{
    for (const RetiredEntry & entry : currentRetiredSet(backend, layout, blobShard(ref, gc_shards)))
        if (entry.ref == ref)
            return entry;
    return std::nullopt;
}

inline std::optional<LoadedMeta> markerOf(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    return loadMeta(*op, layout, ref);
}

inline BlobMeta condemnedAt(uint64_t round, uint64_t size = 1)
{
    return BlobMeta{.state = MetaState::Condemned, .condemn_round = round, .size = size};
}

inline BlobMeta cleanMarker(uint64_t size = 1)
{
    return BlobMeta{.state = MetaState::Clean, .condemn_round = 0, .size = size};
}

/// Replaces the marker of `ref` with `meta`, or creates it when absent.
inline void setMarker(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta)
{
    OperationForTest op(backend);
    const auto current = loadMeta(*op, layout, ref);
    const WriteResult written = current ? casMeta(*op, layout, ref, current->etag, meta)
                                        : putMetaIfAbsent(*op, layout, ref, meta);
    ASSERT_TRUE(std::holds_alternative<Committed>(written)) << layout.blobMetaKey(ref);
}

inline void removeMarker(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    const auto current = loadMeta(*op, layout, ref);
    ASSERT_TRUE(current.has_value()) << layout.blobMetaKey(ref);
    ASSERT_EQ(deleteMetaExact(*op, layout, ref, current->etag), Removal::Removed);
}

inline bool blobPresent(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    return (*op).head(layout.blobKey(ref), Retry::standard()).has_value();
}

/// Moves the GC lease to `owner` as a steal would, without the observation window.
inline void handGcLeaseTo(Backend & backend, const Layout & layout, const UInt128 & owner)
{
    OperationForTest op(backend);
    const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
    ASSERT_TRUE(got.has_value());
    GcState state = decodeGcState(got->bytes);
    state.lease.owner = owner;
    ++state.lease.seq;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(layout.gcStateKey(), encodeGcState(state), got->etag, Retry::once())));
}

inline RoundReport runGateRound(const PoolPtr & store, Gc & gc, UniversePolicy policy = UniversePolicy::Authoritative)
{
    const RoundReport report = gc.runRegularRound({}, /*allow_steal*/ true, policy);
    store->renewWatermarkOnce();
    return report;
}

/// Runs rounds until `done` holds after one of them; fails the test after `max_rounds`.
inline void runRoundsUntil(const PoolPtr & store, Gc & gc, const std::function<bool()> & done, size_t max_rounds = 12)
{
    for (size_t i = 0; i < max_rounds; ++i)
    {
        runGateRound(store, gc);
        if (done())
            return;
    }
    FAIL() << "the condition did not hold within " << max_rounds << " rounds";
}

/// Publishes `hashes`, folds their edges, drops the ref and runs rounds until every blob is condemned. All of
/// them are condemned in one round, written to `condemn_round`.
inline void condemnBlobs(const PoolPtr & store, Gc & gc, Backend & backend, const String & ref_name,
                         uint64_t build_sequence, const std::vector<UInt128> & hashes, uint64_t & condemn_round)
{
    const Layout & layout = store->layout();
    const uint64_t shards = store->poolConfig().gc_shards;
    const ManifestRef manifest = publishBlobs(backend, layout, ref_name, build_sequence, hashes);
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    dropBlobs(backend, layout, ref_name, manifest);
    store->renewWatermarkOnce();
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc, [&]
    {
        for (const UInt128 & hash : hashes)
            if (!retiredEntryFor(backend, layout, gateRef(hash), shards))
                return false;
        return true;
    }));
    condemn_round = retiredEntryFor(backend, layout, gateRef(hashes.front()), shards)->condemn_round;
}

/// Writes one part holding `payload` through the writer. `Observed` means the writer adopted the stored
/// incarnation; `Published` means it uploaded a fresh one.
inline BlobMaterializationAction writeOnePart(const PoolPtr & store, const String & ns, const String & ref_name,
                                              const String & payload)
{
    const RootNamespace root{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref_name;
    auto build = store->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(root, ref_name, id);
    const BlobUploadResult uploaded = build->uploadBlobDetached(BlobUploadRequest{
        .ref = entry.ref, .source = BlobSource::fromString(payload), .declared_size = payload.size()});
    build->mergeBlobUploadResults(std::span<const BlobUploadResult>(&uploaded, 1));
    build->promote(root, ref_name, build->buildId(), id);
    return uploaded.diagnostics.action;
}

/// A bounded spin that fails the test instead of hanging.
inline void awaitCondition(const std::function<bool()> & condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition())
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the awaited condition never held";
        std::this_thread::yield();
    }
}

inline uint64_t unconfirmedCarries()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCCondemnMarkerUnconfirmedCarry].load();
}

inline uint64_t metaWriteAnomalies()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCMetaWriteAnomaly].load();
}

inline uint64_t readAheadWasted()
{
    return ProfileEvents::global_counters[ProfileEvents::CASGCReadAheadWasted].load();
}

/// Runs a hook once, on the first HEAD of one key after `armOnHead`, before that HEAD is served and with no
/// backend lock held. A writer that commits there lands after the round's cut and before the redelete
/// observes the blob.
class HeadHookBackend : public CountingBackend
{
public:
    void armOnHead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(hook_mutex);
        hooked_key = key;
        on_head = std::move(hook);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        std::function<void()> fire;
        {
            std::lock_guard lock(hook_mutex);
            if (on_head && key == hooked_key)
                fire = std::exchange(on_head, {});
        }
        if (fire)
            fire();
        return CountingBackend::head(key, access);
    }

private:
    std::mutex hook_mutex;
    String hooked_key;
    std::function<void()> on_head;
};

/// Parks the first redelete apply of `target` until released. The parked leader has removed the body and has
/// not yet scheduled its marker delete: the window a successor's rounds run in.
struct ApplyParking
{
    std::atomic<bool> armed{false};
    BlobRef target{};
    ManualBarrier barrier;
};

/// `target` is set before `armed`, and the hook reads `armed` first, so the two never race.
inline std::function<void(const BlobRef &)> parkingHook(const std::shared_ptr<ApplyParking> & parking)
{
    return [parking](const BlobRef & ref)
    {
        if (parking->armed.load() && ref == parking->target && parking->armed.exchange(false))
            parking->barrier.arriveAndWait();
    };
}


/// Marker etags derived from the bytes, as S3 derives them: equal bytes carry equal etags, so a replaced
/// marker's etag comes back when the same bytes are written again. Every other key keeps minted values.
class ContentEtagBackend : public HeadHookBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = HeadHookBackend::read(key, access);
        if (raw && isMarker(key))
            raw->value = contentValue(raw->bytes);
        return raw;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = HeadHookBackend::head(key, access);
        if (meta && isMarker(key))
            if (const auto raw = InMemoryBackend::read(key, access))
                meta->value = contentValue(raw->bytes);
        return meta;
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        RawListPage page = HeadHookBackend::list(prefix, cursor, limit, access);
        for (RawListedKey & listed : page.keys)
            if (listed.value && isMarker(listed.key))
                if (const auto raw = InMemoryBackend::read(listed.key, access))
                    listed.value = contentValue(raw->bytes);
        return page;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!isMarker(key))
            return HeadHookBackend::remove(key, expected_value, access);
        std::lock_guard lock(marker_mutex);
        const auto current = InMemoryBackend::read(key, access);
        if (!current)
            return RawRemoval::Gone;
        if (contentValue(current->bytes) != expected_value)
            return RawRemoval::Mismatch;
        return HeadHookBackend::remove(key, current->value, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (!isMarker(key))
            return HeadHookBackend::write(key, bytes, expected_value, access);
        std::lock_guard lock(marker_mutex);
        std::optional<String> minted_expected;
        if (expected_value)
        {
            const auto current = InMemoryBackend::read(key, access);
            if (!current || contentValue(current->bytes) != *expected_value)
                return std::unexpected(RawConflict{});
            minted_expected = current->value;
        }
        const auto written = HeadHookBackend::write(key, bytes, minted_expected, access);
        if (!written)
            return written;
        return contentValue(bytes);
    }

private:
    static bool isMarker(const String & key) { return key.ends_with(".meta"); }
    static String contentValue(const String & bytes) { return "content-" + hexOf(bytes); }

    std::mutex marker_mutex;
};

}
