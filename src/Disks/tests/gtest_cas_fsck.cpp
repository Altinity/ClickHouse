#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <algorithm>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{
StorePtr openPool(std::shared_ptr<Backend> backend)
{
    PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(7);
    return Store::open(backend, cfg);
}

/// Publish a one-blob part `ref` carrying `payload` (shared content => shared blob).
void publishPart(Store & store, const String & payload, const String & ref)
{
    auto build = store.startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    TreeEntry e; e.name = "data.bin"; e.placement = Placement::Blob;
    e.file_hash = u128Of(payload); e.file_size = payload.size();
    auto tree = build->putTree({e});
    build->publish(RootNamespace{"uui/uuid-1"}, ref, tree, {});
}
}

TEST(CasFsck, CleanPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "shared-A", "all_1_1_0");
    publishPart(*store, "shared-A", "all_2_2_0");   /// identical content => same blob (dedup)

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_TRUE(report.clean());
    EXPECT_EQ(report.unreachable, 0u);
    EXPECT_EQ(report.distinct_blobs, 1u);
    EXPECT_EQ(report.total_blob_refs, 2u);
}

TEST(CasFsck, MissingReachableBlobIsDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "lonely", "all_1_1_0");

    const String bkey = store->layout().blobKey(idOf("lonely"));
    const auto tok = backend->head(bkey).token;
    ASSERT_EQ(backend->deleteExact(bkey, tok).kind, DeleteOutcome::Kind::Deleted);

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 1u);
    EXPECT_FALSE(report.clean());
    auto it = std::find_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.cls == FsckClass::Dangling; });
    ASSERT_NE(it, report.objects.end());
    EXPECT_EQ(it->key, bkey);
    EXPECT_FALSE(it->reachable_from.empty());
}

TEST(CasFsck, DroppedButUnreclaimedBlobIsUnreachable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "ghost", "all_1_1_0");
    store->dropRef(RootNamespace{"uui/uuid-1"}, "all_1_1_0");

    auto report = runFsck(*store, /*detail=*/false);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_GE(report.unreachable, 1u);
}
