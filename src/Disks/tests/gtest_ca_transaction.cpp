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

const DB::Cas::TreeEntry * findByName(const std::vector<DB::Cas::TreeEntry> & entries, const std::string & name)
{
    for (const auto & e : entries)
        if (e.name == name)
            return &e;
    return nullptr;
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

/// B151 rollback safety: a ref published at the lock-free rename must be DROPPED if the transaction
/// is abandoned (destructed without commit) — e.g. a ZK multi failure rolls back after renameParts().
TEST(CaTransactionLockScope, RenamePublishedRefDroppedOnAbandon)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_3_3_0/data.bin", "abandoned");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_3_3_0", "uui/uuid-1/all_3_3_0");
        EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));   /// published at the rename
        /// tx goes out of scope here WITHOUT commit() — abandon/rollback.
    }
    /// The early-published ref must have been dropped by the destructor.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));
}

/// A COMMITTED transaction must NOT drop its rename-published ref.
TEST(CaTransactionLockScope, RenamePublishedRefSurvivesCommit)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_4_4_0/data.bin", "kept");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_4_4_0", "uui/uuid-1/all_4_4_0");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_4_4_0"));
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

/// Plan 2d: a small eager metadata file (checksums.txt) is staged INLINE — it rides the single tree
/// object (one-GET part open) — while per-column data (data.bin) stays a standalone Blob (preserving
/// column-read selectivity). The inlined file is still readable through the normal read path.
TEST(CaTransactionInlining, EagerFileInlinedDataBinBlobbed)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-9/tmp_insert_all_1_1_0/checksums.txt", "the-checksums");
    writeFileTx(*tx, "uui/uuid-9/tmp_insert_all_1_1_0/data.bin", std::string(50000, 'D'));
    tx->moveDirectory("uui/uuid-9/tmp_insert_all_1_1_0", "uui/uuid-9/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    /// Resolve the published part to its tree and inspect placements (the Store read API, as in
    /// gtest_cas_store.cpp: resolveRef -> readTree).
    const auto ns = storage->liveNamespace("uuid-9");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const auto entries = storage->store()->readTree(resolved->tree_id);

    const auto * checksums = findByName(entries, "checksums.txt");
    const auto * databin   = findByName(entries, "data.bin");
    ASSERT_TRUE(checksums && databin);
    EXPECT_EQ(checksums->placement, DB::Cas::Placement::Inline);
    EXPECT_EQ(checksums->inline_bytes, "the-checksums");
    EXPECT_EQ(databin->placement, DB::Cas::Placement::Blob);

    /// And the inlined file is still readable through the normal read path.
    EXPECT_EQ(storage->getFileSize("uui/uuid-9/all_1_1_0/checksums.txt"), 13u);
}
