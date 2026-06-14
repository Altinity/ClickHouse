#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/tests/cas_test_helpers.h>
#include <filesystem>

/// B151: CA publish-at-rename lock-scope tests.
/// Proves that a freshly-written part's FINAL manifest ref is published at the lock-free
/// tmp->final rename (moveDirectory), not deferred to commit() which runs under the data_parts
/// lock. This is the core of the B151 fix.

namespace
{

std::shared_ptr<DB::ContentAddressedMetadataStorage> openTxStorage()
{
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1",
        std::filesystem::temp_directory_path() / "ca_tx_lockscope_scratch", nullptr);
    storage->startup();
    return storage;
}

void writeFileTx(DB::IMetadataTransaction & tx, const std::string & path, const std::string & bytes)
{
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(tx);
    auto buf = ca_tx.writeFile(path, 65536, DB::WriteMode::Rewrite, {});
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

}

/// B151: a freshly-written part is PUBLISHED at the (lock-free) tmp->final rename, so the final ref
/// resolves AFTER moveDirectory and BEFORE commit().
TEST(CaTransactionLockScope, PublishHappensAtRenameNotCommit)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_1_1_0/data.bin", "content-A");

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    tx->moveDirectory("uui/uuid-1/tmp_insert_all_1_1_0", "uui/uuid-1/all_1_1_0");

    /// Published at the rename — before commit().
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));

    tx->commit(DB::NoCommitOptions{});
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);
}

/// A committed-ref rename (no staged source) must NOT spuriously publish — it goes via republishRef.
TEST(CaTransactionLockScope, CommittedRefMoveDoesNotSpuriouslyPublish)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_2_2_0/data.bin", "payload");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_2_2_0", "uui/uuid-1/all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory("uui/uuid-1/all_2_2_0", "uui/uuid-1/delete_tmp_all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/delete_tmp_all_2_2_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_2_2_0"));
}
