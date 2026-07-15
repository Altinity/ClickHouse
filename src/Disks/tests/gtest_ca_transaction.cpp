#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>
#include <filesystem>

namespace ProfileEvents
{
extern const Event CasRefRepoint;
}

/// B151: CA publish-at-rename lock-scope tests.
/// Proves that a freshly-written part's FINAL manifest ref is published at the lock-free
/// tmp->final rename (moveDirectory), not deferred to commit() which runs under the data_parts
/// lock. This is the core of the B151 fix.

namespace
{

std::shared_ptr<DB::ContentAddressedMetadataStorage> openTxStorage()
{
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "test",
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

/// Match a manifest entry by its basename (the canonical `path` is the full part-relative path).
const DB::Cas::ManifestEntry * findByName(const std::vector<DB::Cas::ManifestEntry> & entries, const std::string & name)
{
    for (const auto & e : entries)
    {
        const auto slash = e.path.find_last_of('/');
        const std::string base = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
        if (base == name)
            return &e;
    }
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

    /// Resolve the published part to its manifest and inspect placements (the Store read API, as in
    /// gtest_cas_store.cpp: resolveRef -> readManifest).
    const auto ns = storage->liveNamespace("uuid-9");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const DB::Cas::PartManifest manifest = storage->store()->readManifest(resolved->manifest_id);
    const auto & entries = manifest.entries;

    const auto * checksums = findByName(entries, "checksums.txt");
    const auto * databin   = findByName(entries, "data.bin");
    ASSERT_TRUE(checksums && databin);
    EXPECT_EQ(checksums->placement, DB::Cas::EntryPlacement::Inline);
    EXPECT_EQ(checksums->inline_bytes, "the-checksums");
    EXPECT_EQ(databin->placement, DB::Cas::EntryPlacement::Blob);

    /// And the inlined file is still readable through the normal read path.
    EXPECT_EQ(storage->getFileSize("uui/uuid-9/all_1_1_0/checksums.txt"), 13u);
}

/// all-tree-part-files Task 4 (spec 2026-07-14-cas-all-tree-part-files-design.md §4): a standalone
/// write of ONE file onto an ALREADY-COMMITTED part must carry every other file of that part forward
/// (a repoint, Task 3), never replace the manifest with just the touched file.
TEST(CaTransactionRepoint, StandaloneWriteOnCommittedPartRepoints)
{
    auto storage = openTxStorage();

    /// 1. Write a part (checksums.txt inline + data.bin blob) through a normal transaction; commit.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-repoint/tmp_insert_all_1_1_0/checksums.txt", "old-checksums");
        writeFileTx(*tx, "uui/uuid-repoint/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        tx->moveDirectory("uui/uuid-repoint/tmp_insert_all_1_1_0", "uui/uuid-repoint/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("uui/uuid-repoint/all_1_1_0/checksums.txt"));
    ASSERT_TRUE(storage->existsFile("uui/uuid-repoint/all_1_1_0/data.bin"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load();

    /// 2. New transaction: standalone write of checksums.txt onto the ALREADY-COMMITTED part.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-repoint/all_1_1_0/checksums.txt", "new-checksums-longer");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The new content is served, the untouched file is carried forward unchanged, exactly one
    /// repoint fired, and an independent fsck reachability walk finds nothing dangling.
    EXPECT_EQ(storage->getFileSize("uui/uuid-repoint/all_1_1_0/checksums.txt"), 20u);
    EXPECT_EQ(storage->getFileSize("uui/uuid-repoint/all_1_1_0/data.bin"), 14u)
        << "carry-forward: the untouched file must survive a standalone write on the same part";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load(), repoints_before + 1);

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// Task 9 coverage gap (closed here, folded in from the T8 review): ONE uncommitted transaction that
/// BOTH writes a file and unlinks a DIFFERENT file of the SAME already-committed part must resolve to
/// exactly ONE repoint carrying the write, the removal, AND every untouched file forward together --
/// not two independent repoints, and not a lost update from one staged change clobbering the other.
/// `publishStaging`'s Task 4/8 merge already handles `st.entries` and `st.content_removed` together
/// (both conditions can be true on the same staging); this pins that the combined shape actually works
/// end to end through the real transaction, not just through each half in isolation.
TEST(CaTransactionRepoint, CombinedWriteAndUnlinkSameTxnRepointsOnce)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with three files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-combo-1/tmp_insert_all_1_1_0/checksums.txt", "old-checksums");
        writeFileTx(*tx, "uui/uuid-combo-1/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "uui/uuid-combo-1/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("uui/uuid-combo-1/tmp_insert_all_1_1_0", "uui/uuid-combo-1/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("uui/uuid-combo-1/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load();

    /// 2. ONE transaction: write checksums.txt (new bytes) AND unlink txn_version.txt (a DIFFERENT
    /// file of the same part) -- must resolve to exactly one repoint carrying both changes plus the
    /// untouched data.bin.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-combo-1/all_1_1_0/checksums.txt", "new-checksums-longer");
        tx->unlinkFile("uui/uuid-combo-1/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The written file is updated, the unlinked file is honestly gone, the untouched file survives
    /// (carry-forward), exactly ONE repoint fired (not two, not zero), and fsck finds nothing dangling.
    EXPECT_EQ(storage->getFileSize("uui/uuid-combo-1/all_1_1_0/checksums.txt"), 20u);
    EXPECT_FALSE(storage->existsFile("uui/uuid-combo-1/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-combo-1/all_1_1_0/data.bin"), 14u)
        << "carry-forward: the untouched file must survive a combined write+unlink on the same part";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load(), repoints_before + 1)
        << "one uncommitted transaction combining a write and an unlink must resolve to exactly one repoint";

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 6 (spec 2026-07-14-cas-all-tree-part-files-design.md §4): the mutable-
/// per-part-file branch is deleted from `writeFile` -- uuid.txt/metadata_version.txt/txn_version.txt
/// now flow down the ordinary content path, landing in the manifest like any other file.
TEST(CaTransactionAllTree, BuildTimeSidecarsLandInManifest)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-alltree-1/tmp_insert_all_1_1_0/uuid.txt", "part-uuid-bytes");
    writeFileTx(*tx, "uui/uuid-alltree-1/tmp_insert_all_1_1_0/metadata_version.txt", "3");
    writeFileTx(*tx, "uui/uuid-alltree-1/tmp_insert_all_1_1_0/txn_version.txt", "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
    writeFileTx(*tx, "uui/uuid-alltree-1/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
    tx->moveDirectory("uui/uuid-alltree-1/tmp_insert_all_1_1_0", "uui/uuid-alltree-1/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    const auto ns = storage->liveNamespace("uuid-alltree-1");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());

    const DB::Cas::PartManifest manifest = storage->store()->readManifest(resolved->manifest_id);
    const auto & entries = manifest.entries;
    const auto * uuid_entry = findByName(entries, "uuid.txt");
    const auto * meta_version_entry = findByName(entries, "metadata_version.txt");
    const auto * txn_version_entry = findByName(entries, "txn_version.txt");
    ASSERT_TRUE(uuid_entry && meta_version_entry && txn_version_entry)
        << "all three sidecar files must land in the manifest as ordinary tree entries";
    EXPECT_EQ(uuid_entry->placement, DB::Cas::EntryPlacement::Inline);
    EXPECT_EQ(meta_version_entry->placement, DB::Cas::EntryPlacement::Inline);
    EXPECT_EQ(txn_version_entry->placement, DB::Cas::EntryPlacement::Inline);
    EXPECT_EQ(meta_version_entry->inline_bytes, "3");

    /// And they are readable through the normal read path — Task 9 deleted the ForceFresh special
    /// case these reads used to go through; they now resolve purely via the manifest view like any
    /// other entry (existsFile / getFileSize / tryGetInManifestBytes).
    EXPECT_TRUE(storage->existsFile("uui/uuid-alltree-1/all_1_1_0/metadata_version.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-alltree-1/all_1_1_0/metadata_version.txt"), 1u);
}

/// A standalone one-shot write of txn_version.txt onto an ALREADY-COMMITTED part (the MVCC creation-
/// CSN fill-in / removal-TID rewrite shape) must repoint (Task 4), never orphan the rest of the part.
TEST(CaTransactionAllTree, CommittedTxnVersionStoreRepoints)
{
    auto storage = openTxStorage();

    /// 1. Commit a part WITHOUT txn_version.txt.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-alltree-2/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "uui/uuid-alltree-2/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        tx->moveDirectory("uui/uuid-alltree-2/tmp_insert_all_1_1_0", "uui/uuid-alltree-2/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_FALSE(storage->existsFile("uui/uuid-alltree-2/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load();

    /// 2. A single-op transaction writes ONLY txn_version.txt onto the already-committed part (mirrors
    /// the MVCC one-shot autocommit shape: no other file touched in this transaction).
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-alltree-2/all_1_1_0/txn_version.txt", "creation_tid: (2,2,00000000-0000-0000-0000-000000000000)");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. Exactly one repoint; the new file is served; the original files are intact (carry-forward).
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load(), repoints_before + 1);
    EXPECT_TRUE(storage->existsFile("uui/uuid-alltree-2/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-alltree-2/all_1_1_0/txn_version.txt"), 56u);
    EXPECT_EQ(storage->getFileSize("uui/uuid-alltree-2/all_1_1_0/checksums.txt"), 8u);
    EXPECT_EQ(storage->getFileSize("uui/uuid-alltree-2/all_1_1_0/data.bin"), 14u);

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 8 (B123 evolution, spec 2026-07-14-cas-all-tree-part-files-design.md §6):
/// a lone surgical unlink of ONE committed content file (not followed by a whole-part removal in the
/// same transaction — the ATTACH `removeVersionMetadata` shape) must actually delete the file via a
/// repoint-remove, closing the pre-Task-8 fail-open (unlinkFile of a committed content file used to be
/// an unconditional no-op).
TEST(CaTransactionRemove, SurgicalUnlinkRepoints)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with txn_version.txt among its files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-remove-1/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "uui/uuid-remove-1/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "uui/uuid-remove-1/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("uui/uuid-remove-1/tmp_insert_all_1_1_0", "uui/uuid-remove-1/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("uui/uuid-remove-1/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load();

    /// 2. A single-op transaction unlinks ONLY txn_version.txt on the already-committed part (mirrors
    /// ATTACH's removeVersionMetadata: no dir-drop in the same transaction).
    {
        auto tx = storage->createTransaction();
        tx->unlinkFile("uui/uuid-remove-1/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The file is honestly gone, the untouched files survive (carry-forward), exactly one repoint
    /// fired, and an independent fsck reachability walk finds nothing dangling.
    EXPECT_FALSE(storage->existsFile("uui/uuid-remove-1/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-remove-1/all_1_1_0/checksums.txt"), 8u);
    EXPECT_EQ(storage->getFileSize("uui/uuid-remove-1/all_1_1_0/data.bin"), 14u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load(), repoints_before + 1);

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 8 (B123 evolution, spec §6): the DOMINANT CA removal path — the MergeTree
/// fast-removal shape that unlinks every part file one by one and THEN calls removeDirectory — must
/// stay exactly one ref-drop and pay ZERO repoints. The per-file removal marks staged by the unlink
/// storm are superseded by the ref-drop, not individually repointed.
TEST(CaTransactionRemove, UnlinkStormThenDirDropIsOneRefDrop)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with three files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-remove-2/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "uui/uuid-remove-2/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "uui/uuid-remove-2/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("uui/uuid-remove-2/tmp_insert_all_1_1_0", "uui/uuid-remove-2/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-remove-2/all_1_1_0"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load();

    /// 2. The MergeTree fast-removal shape (IMergeTreeDataPart::remove, B123): unlink every file
    /// one-by-one, THEN removeDirectory the part — all in one transaction.
    {
        auto tx = storage->createTransaction();
        tx->unlinkFile("uui/uuid-remove-2/all_1_1_0/checksums.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("uui/uuid-remove-2/all_1_1_0/data.bin", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("uui/uuid-remove-2/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->removeDirectory("uui/uuid-remove-2/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The whole part is gone via the single ref-drop; the storm of marks never repointed anything.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-remove-2/all_1_1_0"));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefRepoint].load(), repoints_before)
        << "unlink-storm-then-dir-drop must supersede the marks, not repoint per file";

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}
