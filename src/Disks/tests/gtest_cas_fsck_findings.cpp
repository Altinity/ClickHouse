#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include "cas_test_helpers.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <vector>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
constexpr uint64_t kWriterEpoch = 7;

ManifestRef manifestRefOf(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_epoch = kWriterEpoch, .build_sequence = seq, .manifest_ordinal = static_cast<uint32_t>(inst)};
}

/// The `_ckpt` an ordinary live test life carries after its first committed record.
void writeCheckpoint(Backend & backend, const Layout & layout, const RootNamespace & ns, RefTxnId committed_through)
{
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const CasRefCatalog::Snapshot cut = CasRefCatalog::read(op, layout);
    const auto it = std::find_if(cut.catalog.entries.begin(), cut.catalog.entries.end(),
        [&](const CatalogEntry & entry) { return entry.ns == ns; });
    ASSERT_NE(it, cut.catalog.entries.end());
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(it->ns, it->incarnation);
    const String key = layout.refCkptKey(life);
    const String body = encodeRefCkptAtGeneration(RefCkpt{
        .life_epoch = committed_through.writer_epoch,
        .committed_through = committed_through,
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt}, 1);
    const auto current = op.head(key, Retry::once());
    const WriteResult put = current
        ? op.replace(key, body, current->etag, Retry::once())
        : op.create(key, body, Retry::once());
    ASSERT_TRUE(std::holds_alternative<Committed>(put));
}

/// Runs a hook once, just before the read of `key` that follows `skip` earlier reads of it.
class ReadHookBackend : public CountingBackend
{
public:
    void beforeRead(const String & key, size_t skip, std::function<void()> hook)
    {
        std::lock_guard lock(hook_mutex);
        hooks[key] = {skip, std::move(hook)};
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::function<void()> hook;
        {
            std::lock_guard lock(hook_mutex);
            if (const auto it = hooks.find(key); it != hooks.end())
            {
                if (it->second.first > 0)
                    --it->second.first;
                else
                {
                    hook = std::move(it->second.second);
                    hooks.erase(it);
                }
            }
        }
        if (hook)
            hook();
        return CountingBackend::read(key, access);
    }

private:
    std::mutex hook_mutex;
    std::map<String, std::pair<size_t, std::function<void()>>> hooks;
};

/// A two-shard pool whose one GC round folded a manifest with one blob per shard, so `gc/state` adopts a seal with
/// a source-edge run for each shard.
struct TwoShardFixture
{
    static constexpr uint64_t shards = 2;
    std::shared_ptr<ReadHookBackend> backend = std::make_shared<ReadHookBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    RootNamespace ns{"00/aa@cas@"};
    ManifestRef manifest = manifestRefOf(1, 0xA1);
    ManifestId manifest_id;
    std::vector<uint64_t> blob_of_shard = std::vector<uint64_t>(shards, 0);

    TwoShardFixture()
    {
        for (uint64_t b = 1, found = 0; found < shards; ++b)
            if (!blob_of_shard[blobShard(blobRefOf(b), shards)])
            {
                blob_of_shard[blobShard(blobRefOf(b), shards)] = b;
                ++found;
            }
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
        config.gc_shards = shards;
        store = Pool::open(backend, config);
        std::vector<ManifestEntry> entries;
        for (const uint64_t b : blob_of_shard)
        {
            writeBlobBody(*backend, store->layout(), numberedBlobHash(b));
            entries.push_back(blobEntryFor("f" + std::to_string(b), numberedBlobHash(b)));
        }
        manifest_id = writeManifestRaw(*backend, store->layout(), ns, manifest, entries);
        const uint64_t sequence = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, manifest);
        writeCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
        gc = std::make_unique<Gc>(store, hexToU128("00000000000000000000000000000001"));
        gc->runRegularRound();
    }

    /// The manifest is dropped and its body deleted, so each blob's edge is stale until a round folds it.
    void makeEdgesStale()
    {
        const uint64_t sequence = dropRefTransition(*backend, store->layout(), ns, "tbl", manifest);
        writeCheckpoint(*backend, store->layout(), ns, RefTxnId{1, sequence});
        deleteManifestBody(*backend, store->layout(), manifest_id);
    }

    String sealKey() const
    {
        OperationForTest op(*backend);
        const GcState state = decodeGcState((*op).read(store->layout().gcStateKey(), Retry::standard())->bytes);
        return store->layout().foldSealKey(state.snap_generation, state.snap_attempt);
    }

    CasFoldSeal readSeal() const
    {
        OperationForTest op(*backend);
        const GcState state = decodeGcState((*op).read(store->layout().gcStateKey(), Retry::standard())->bytes);
        const auto got = (*op).read(sealKey(), Retry::standard());
        return decodeFoldSeal(got->bytes, store->layout(), state.gc_shards, state.snap_generation);
    }

    void writeSeal(const CasFoldSeal & seal)
    {
        OperationForTest op(*backend);
        const String key = sealKey();
        const auto head = (*op).head(key, Retry::once());
        ASSERT_TRUE(head.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, encodeFoldSeal(seal), head->etag, Retry::once())));
    }

    /// `seal` without the run of `shard`, claiming one condemned and pending row there: its rows cannot match the claim.
    static CasFoldSeal withShardClaimingAbsentRows(CasFoldSeal seal, uint64_t shard)
    {
        std::erase_if(seal.blob_target_runs, [&](const RunRef & run) { return run.shard == shard; });
        seal.condemned_summary[shard] = CondemnedSummary{
            .condemned_total = 1, .pending_total = 1, .oldest_nonpending_condemn_round = std::numeric_limits<uint64_t>::max()};
        return seal;
    }
};

std::vector<String> corruptedRunKeys(const FsckReport & report)
{
    std::vector<String> keys;
    for (const FsckObject & object : report.objects)
        if (object.cls == FsckClass::CorruptedRun)
            keys.push_back(object.key);
    return keys;
}
}

/// Two shards refused for rows their seal claims but no run holds are two findings, each named by its own key.
TEST(CASFsckFindings, TwoShardsRefusedWithoutARunAreTwoDistinctFindings)
{
    TwoShardFixture f;
    f.writeSeal(TwoShardFixture::withShardClaimingAbsentRows(
        TwoShardFixture::withShardClaimingAbsentRows(f.readSeal(), 0), 1));

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    EXPECT_EQ(rep.corrupted_runs, 2u) << formatFsckSummary(rep);
    const std::vector<String> keys = corruptedRunKeys(rep);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(std::set<String>(keys.begin(), keys.end()).size(), 2u) << "two shards, one key: " << keys[0] << " / " << keys[1];
}

/// Shard 0 is refused by the first read and shard 1 by the confirmation read: both are findings.
TEST(CASFsckFindings, RefusalsOfDifferentShardsInTwoReadsAreBothKept)
{
    TwoShardFixture f;
    f.makeEdgesStale();
    const CasFoldSeal original = f.readSeal();
    f.writeSeal(TwoShardFixture::withShardClaimingAbsentRows(original, 0));
    bool second_read_seen = false;
    f.backend->beforeRead(f.sealKey(), /*skip=*/1, [&]
    {
        second_read_seen = true;
        f.writeSeal(TwoShardFixture::withShardClaimingAbsentRows(original, 1));
    });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(second_read_seen) << "positive control: the scan read the seal a second time";
    EXPECT_EQ(rep.corrupted_runs, 2u) << formatFsckSummary(rep);
    const std::vector<String> keys = corruptedRunKeys(rep);
    EXPECT_EQ(std::set<String>(keys.begin(), keys.end()).size(), 2u);
}

/// The same shard refused by both reads is one finding.
TEST(CASFsckFindings, ARefusalSeenByBothReadsForTheSameShardIsOneFinding)
{
    TwoShardFixture f;
    f.makeEdgesStale();
    f.writeSeal(TwoShardFixture::withShardClaimingAbsentRows(f.readSeal(), 1));
    bool second_read_seen = false;
    f.backend->beforeRead(f.sealKey(), /*skip=*/1, [&] { second_read_seen = true; });

    const FsckReport rep = runFsck(*f.store, /*detail=*/true);

    ASSERT_TRUE(second_read_seen) << "positive control: the scan read the seal a second time";
    EXPECT_EQ(rep.corrupted_runs, 1u) << formatFsckSummary(rep);
    EXPECT_EQ(corruptedRunKeys(rep).size(), 1u);
}
