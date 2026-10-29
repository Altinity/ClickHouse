#include <gtest/gtest.h>

#include <Disks/DiskLocal.h>
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/Replication/ClusterConfiguration.h>
#include <Disks/DiskObjectStorage/Replication/ObjectStorageRouter.h>
#include <Disks/SingleDiskVolume.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Storages/MergeTree/DataPartStorageOnDiskFull.h>
#include <Storages/MergeTree/DataPartStorageOnDiskPacked.h>

#include <Poco/Util/XMLConfiguration.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unistd.h>

using namespace DB;

namespace
{
    /// A DiskLocal-backed parent part storage. `DiskLocal::createTransaction` yields a real
    /// transaction object, which is all `beginTransaction` needs to hand a NON-NULL transaction to a
    /// borrowed projection sub-part (the `has_shared_transaction == true` case).
    struct ParentStorageFixture
    {
        std::filesystem::path base_path;
        DiskPtr disk;
        VolumePtr volume;
        MutableDataPartStoragePtr parent;

        ParentStorageFixture()
        {
            const auto unique = std::to_string(::getpid()) + "_"
                + std::to_string(reinterpret_cast<uintptr_t>(this));
            base_path = std::filesystem::temp_directory_path() / ("proj_txn_gtest_" + unique);
            std::filesystem::create_directories(base_path / "all_1_1_0");
            disk = std::make_shared<DiskLocal>("test_disk_" + unique, base_path.string());
            volume = std::make_shared<SingleDiskVolume>("test_volume", disk);
            parent = std::make_shared<DataPartStorageOnDiskFull>(volume, /*root_path=*/"", "all_1_1_0");
        }

        ~ParentStorageFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(base_path, ec);
        }
    };

    struct CASPackedStorageFixture
    {
        std::filesystem::path scratch_path;
        ObjectStoragePtr object_storage;
        std::shared_ptr<ContentAddressedMetadataStorage> metadata_storage;
        std::shared_ptr<DiskObjectStorage> disk;
        VolumePtr volume;
        const String root_path = "a11/a11a11a1-1111-4111-8111-111111111111";
        const String part_dir = "all_1_1_0";

        CASPackedStorageFixture()
        {
            Cas::tests::ensureBlobUploadPoolForTest();
            const auto unique = std::to_string(::getpid()) + "_"
                + std::to_string(reinterpret_cast<uintptr_t>(this));
            scratch_path = std::filesystem::temp_directory_path() / ("packed_cas_gtest_" + unique);
            object_storage = Cas::tests::makeLocalObjectStorageForTest();
            auto settings = Cas::tests::makeSettingsForTest("packed", scratch_path);
            metadata_storage = std::make_shared<ContentAddressedMetadataStorage>(
                object_storage, "pool", "srv1", "", nullptr, settings);
            metadata_storage->startup();

            std::unordered_map<Location, LocationInfo> cluster_registry = {{"main", {true, true, ""}}};
            std::unordered_map<Location, ObjectStoragePtr> object_storage_registry = {{"main", object_storage}};
            auto cluster = std::make_shared<ClusterConfiguration>("packed_cas", std::move(cluster_registry));
            auto object_storages = std::make_shared<ObjectStorageRouter>(std::move(object_storage_registry));
            Poco::AutoPtr<Poco::Util::XMLConfiguration> config(new Poco::Util::XMLConfiguration());
            disk = std::make_shared<DiskObjectStorage>(
                "packed_cas", std::move(cluster), metadata_storage, std::move(object_storages), nullptr, *config, "");
            volume = std::make_shared<SingleDiskVolume>("packed_cas", disk);
        }
    };
}

/// A projection sub-part that BORROWS the parent's whole-part transaction (the CA-disk shape:
/// getProjection(..., use_parent_transaction = true)) must let begin/commit be NO-OPS — it rides the
/// parent's single commit. Before the encapsulation this threw "Uncommitted shared transaction already
/// exists" / "Cannot commit shared transaction", forcing every caller to branch on isContentAddressed().
TEST(ProjectionBorrowedTransaction, BorrowedStorageBeginCommitAreNoOps)
{
    ParentStorageFixture fx;

    /// Parent opens the whole-part transaction (as MergeTask/writer do for a CA part).
    fx.parent->beginTransaction();
    ASSERT_TRUE(fx.parent->hasActiveTransaction());

    /// Borrowed projection sub-part: shares the parent transaction (has_shared_transaction == true).
    auto proj = fx.parent->getProjection("p.proj", /*use_parent_transaction=*/true);
    EXPECT_TRUE(proj->hasActiveTransaction());

    /// The encapsulated rule: begin/commit on the borrowed storage are silent no-ops (they must NOT
    /// open a second transaction, nor commit the parent's).
    EXPECT_NO_THROW(proj->beginTransaction());
    EXPECT_NO_THROW(proj->commitTransaction());

    /// The parent's transaction is untouched by the projection's no-ops and still commits cleanly.
    EXPECT_TRUE(fx.parent->hasActiveTransaction());
    EXPECT_NO_THROW(fx.parent->commitTransaction());
    EXPECT_FALSE(fx.parent->hasActiveTransaction());
}

/// The non-CA temp-projection shape (use_parent_transaction = false) is unchanged: the sub-part OWNS
/// its transaction, so begin creates it and commit commits it (has_shared_transaction == false, so the
/// no-op path never triggers).
TEST(ProjectionBorrowedTransaction, OwnedProjectionStorageStillBeginsAndCommits)
{
    ParentStorageFixture fx;

    auto proj = fx.parent->getProjection("q.proj", /*use_parent_transaction=*/false);
    EXPECT_FALSE(proj->hasActiveTransaction());

    EXPECT_NO_THROW(proj->beginTransaction());
    EXPECT_TRUE(proj->hasActiveTransaction());
    EXPECT_NO_THROW(proj->commitTransaction());
    EXPECT_FALSE(proj->hasActiveTransaction());
}

TEST(ProjectionBorrowedTransaction, BorrowedPackedStorageFinalizesWithoutCommittingParent)
{
    ParentStorageFixture fx;
    auto parent = std::make_shared<DataPartStorageOnDiskPacked>(fx.volume, "", "all_1_1_0", ReadSettings{}, false);

    parent->beginTransaction();
    ASSERT_TRUE(parent->hasActiveTransaction());

    auto parent_out = parent->writeFile("parent.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("parent payload", *parent_out);
    parent_out->finalize();

    auto proj = parent->getProjectionNoInitialize("p.proj", true);
    ASSERT_TRUE(proj->hasActiveTransaction());
    EXPECT_NO_THROW(proj->beginTransaction());
    proj->createDirectories();

    auto out = proj->writeFile("value.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("packed payload", *out);
    out->finalize();

    EXPECT_NO_THROW(proj->precommitTransaction());
    EXPECT_NO_THROW(proj->commitTransaction());
    EXPECT_TRUE(parent->hasActiveTransaction());

    auto in = proj->readFile("value.bin", ReadSettings{}, std::nullopt);
    String value;
    readStringUntilEOF(value, *in);
    EXPECT_EQ(value, "packed payload");

    EXPECT_NO_THROW(parent->commitTransaction());
    EXPECT_FALSE(parent->hasActiveTransaction());
}

TEST(ProjectionBorrowedTransaction, BorrowedPackedStorageCommitFinalizesWriter)
{
    ParentStorageFixture fx;
    auto parent = std::make_shared<DataPartStorageOnDiskPacked>(fx.volume, "", "all_1_1_0", ReadSettings{}, false);

    parent->beginTransaction();
    auto parent_out = parent->writeFile("parent.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("parent payload", *parent_out);
    parent_out->finalize();

    auto proj = parent->getProjectionNoInitialize("p.proj", true);
    proj->beginTransaction();
    proj->createDirectories();

    auto out = proj->writeFile("value.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("packed payload", *out);
    out->finalize();

    EXPECT_NO_THROW(proj->commitTransaction());
    EXPECT_TRUE(parent->hasActiveTransaction());

    auto in = proj->readFile("value.bin", ReadSettings{}, std::nullopt);
    String value;
    readStringUntilEOF(value, *in);
    EXPECT_EQ(value, "packed payload");

    EXPECT_NO_THROW(parent->commitTransaction());
    EXPECT_FALSE(parent->hasActiveTransaction());
}

TEST(ProjectionBorrowedTransaction, OwnedPackedStorageStillCommitsAndResets)
{
    ParentStorageFixture fx;
    auto storage = std::make_shared<DataPartStorageOnDiskPacked>(fx.volume, "", "all_1_1_0", ReadSettings{}, false);

    storage->beginTransaction();
    auto out = storage->writeFile("value.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("packed payload", *out);
    out->finalize();

    EXPECT_NO_THROW(storage->commitTransaction());
    EXPECT_FALSE(storage->hasActiveTransaction());

    auto in = storage->readFile("value.bin", ReadSettings{}, std::nullopt);
    String value;
    readStringUntilEOF(value, *in);
    EXPECT_EQ(value, "packed payload");
}

TEST(ProjectionBorrowedTransaction, CASPackedStorageReadsStagedReplacement)
{
    CASPackedStorageFixture fx;

    {
        auto parent = std::make_shared<DataPartStorageOnDiskPacked>(
            fx.volume, fx.root_path, fx.part_dir, ReadSettings{}, false);
        parent->beginTransaction();
        parent->createDirectories();

        auto parent_out = parent->writeFile("parent.bin", 4096, WriteMode::Rewrite, WriteSettings{});
        writeString("parent payload", *parent_out);
        parent_out->finalize();

        auto proj = parent->getProjectionNoInitialize("p.proj", true);
        proj->beginTransaction();
        proj->createDirectories();
        auto out = proj->writeFile("value.bin", 4096, WriteMode::Rewrite, WriteSettings{});
        writeString("old payload", *out);
        out->finalize();
        proj->commitTransaction();
        parent->commitTransaction();
    }

    auto parent = std::make_shared<DataPartStorageOnDiskPacked>(
        fx.volume, fx.root_path, fx.part_dir, ReadSettings{}, true);
    parent->beginTransaction();
    auto proj = parent->getProjection("p.proj", true);
    ASSERT_FALSE(proj->getRemotePaths("value.bin").empty());

    auto out = proj->writeFile("value.bin", 4096, WriteMode::Rewrite, WriteSettings{});
    writeString("new payload", *out);
    out->finalize();
    proj->commitTransaction();

    EXPECT_TRUE(parent->hasActiveTransaction());
    EXPECT_TRUE(proj->getRemotePaths("value.bin").empty());

    auto in = proj->readFile("value.bin", ReadSettings{}, std::nullopt);
    String value;
    readStringUntilEOF(value, *in);
    EXPECT_EQ(value, "new payload");

    parent->checkpointTransaction();
    EXPECT_TRUE(parent->hasActiveTransaction());

    auto before_commit_parent = std::make_shared<DataPartStorageOnDiskPacked>(
        fx.volume, fx.root_path, fx.part_dir, ReadSettings{}, true);
    auto before_commit_proj = before_commit_parent->getProjection("p.proj");
    auto before_commit_in = before_commit_proj->readFile("value.bin", ReadSettings{}, std::nullopt);
    value.clear();
    readStringUntilEOF(value, *before_commit_in);
    EXPECT_EQ(value, "old payload");

    parent->commitTransaction();
    EXPECT_FALSE(parent->hasActiveTransaction());

    auto committed_parent = std::make_shared<DataPartStorageOnDiskPacked>(
        fx.volume, fx.root_path, fx.part_dir, ReadSettings{}, true);
    auto committed_proj = committed_parent->getProjection("p.proj");
    auto committed_in = committed_proj->readFile("value.bin", ReadSettings{}, std::nullopt);
    value.clear();
    readStringUntilEOF(value, *committed_in);
    EXPECT_EQ(value, "new payload");
}
