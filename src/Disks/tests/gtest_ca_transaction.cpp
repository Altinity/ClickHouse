#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <filesystem>

namespace ProfileEvents
{
extern const Event CasRefRepoint;
}

/// [TXN-ONE-PIPELINE] CA publish-at-commit lock-scope tests.
/// Proves that a freshly-written part's FINAL manifest ref is published only by commit(); the
/// tmp->final rename (moveDirectory) is a pure re-key of the transaction-private overlay and
/// publishes nothing. This inverts the former B151 publish-at-rename behavior.

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

/// [TXN-ONE-PIPELINE] A freshly-written part is published by commit(), NOT at the tmp->final rename.
/// moveDirectory only re-keys the transaction overlay; the durable ref appears at commit().
TEST(CaTransactionLockScope, PublishHappensAtCommitNotRename)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_1_1_0/data.bin", "content-A");

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/tmp_insert_all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    tx->moveDirectory("uui/uuid-1/tmp_insert_all_1_1_0", "uui/uuid-1/all_1_1_0");

    /// Re-key only: the final ref is NOT durable yet.
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));

    tx->commit(DB::NoCommitOptions{});

    /// Published by commit().
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-1/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 9u);
}

/// [TXN-ONE-PIPELINE] An abandoned transaction (destructed without commit) never published, so the
/// final ref is simply absent — no early-published ref to drop.
TEST(CaTransactionLockScope, AbandonedPartLeavesNoRef)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-1/tmp_insert_all_3_3_0/data.bin", "abandoned");
        tx->moveDirectory("uui/uuid-1/tmp_insert_all_3_3_0", "uui/uuid-1/all_3_3_0");
        EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));   /// not published at the rename
        /// tx goes out of scope WITHOUT commit().
    }
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-1/all_3_3_0"));
}

/// [TXN-ONE-PIPELINE] commit() publishes the re-keyed part.
TEST(CaTransactionLockScope, RefPublishedByCommit)
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

/// [TXN-ONE-PIPELINE] B183 migration gate: a scratch ref durably published at the part's own (tmp)
/// BUILD path by a nested sub-storage must be dropped on the staged-source tmp->final finalize, and
/// commit() must publish the AUTHORITATIVE staged manifest (not the scratch content). This mirrors
/// `createTemporaryTextIndexStorage`, which publishes scratch under the new_data_part's STILL-TMP
/// relative path (`MergeTask.cpp` uses `getDataPartStorage().getRelativePath()`) — i.e. the SOURCE of
/// the tmp->final rename, which is exactly what `moveDirectory`'s `dropRefIfPresent(src->refKey())`
/// drops. (The plan's destination-path scenario would not reproduce this: `publishStaging`'s
/// repoint-merge would carry the scratch file forward.)
TEST(CaTransactionLockScope, StagedFinalizeDropsForeignScratchRef)
{
    auto storage = openTxStorage();

    /// A SEPARATE transaction (the nested text-index sub-storage) durably publishes a committed ref at
    /// the tmp BUILD path holding only a scratch file under `text_index_tmp/`.
    {
        auto scratch_tx = storage->createTransaction();
        writeFileTx(*scratch_tx, "uui/uuid-7/tmp_merge_all_1_1_0/text_index_tmp/scratch.bin", "scratch");
        scratch_tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-7/tmp_merge_all_1_1_0"));

    /// The real part build: stage the authoritative data.bin under the SAME tmp path, then finalize
    /// tmp->final. The staged-source finalize drops the foreign scratch ref at the tmp path.
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-7/tmp_merge_all_1_1_0/data.bin", std::string(50000, 'D'));
    tx->moveDirectory("uui/uuid-7/tmp_merge_all_1_1_0", "uui/uuid-7/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    /// The published manifest is the authoritative one (has data.bin), not the scratch ref.
    const auto ns = storage->liveNamespace("uuid-7");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const auto manifest = storage->store()->readManifest(resolved->manifest_id);
    EXPECT_TRUE(findByName(manifest.entries, "data.bin"));
    EXPECT_FALSE(findByName(manifest.entries, "scratch.bin"));

    /// The foreign scratch ref at the tmp build path is gone (dropped, not carried forward).
    EXPECT_FALSE(storage->existsDirectory("uui/uuid-7/tmp_merge_all_1_1_0"));
}

/// [TXN-ONE-PIPELINE] After a tmp->final re-key, a read THROUGH the open transaction resolves the
/// staged content under the FINAL path (read-your-writes), before commit(); the inner-directory
/// overlay is likewise re-keyed and answers under the final path. The staged file lives under an
/// inner projection dir because the directory overlay tracks INNER dirs only — the part dir itself
/// answers `hasInFlightDirectory`=false by contract (removeIfNeeded clean early-return; see
/// `CaWiringInFlight`), so asserting the bare part dir would contradict that invariant.
TEST(CaTransactionLockScope, ReadYourWritesAfterReKey)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "uui/uuid-3/tmp_insert_all_1_1_0/p.proj/checksums.txt", "the-checksums");
    tx->moveDirectory("uui/uuid-3/tmp_insert_all_1_1_0", "uui/uuid-3/all_1_1_0");

    /// The overlay answers the final path before commit (read-your-writes).
    auto buf = ca_tx.tryReadFileInFlight("uui/uuid-3/all_1_1_0/p.proj/checksums.txt", DB::ReadSettings{}, std::nullopt);
    ASSERT_NE(buf, nullptr);
    std::string got;
    DB::readStringUntilEOF(got, *buf);
    EXPECT_EQ(got, "the-checksums");
    /// The inner-directory overlay is re-keyed too and resolves under the final path.
    EXPECT_TRUE(ca_tx.hasInFlightDirectory("uui/uuid-3/all_1_1_0/p.proj"));
}

/// [TXN-ONE-PIPELINE] Program order in the overlay: create -> delete -> create leaves the file PRESENT
/// (no delayed delete fires after the later create); delete of a staged file makes it absent to reads.
TEST(CaTransactionLockScope, OverlayProgramOrder)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "uui/uuid-5/tmp_insert_all_1_1_0/a.txt", "v1");
    ca_tx.unlinkFile("uui/uuid-5/tmp_insert_all_1_1_0/a.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
    EXPECT_EQ(ca_tx.tryReadFileInFlight("uui/uuid-5/tmp_insert_all_1_1_0/a.txt", DB::ReadSettings{}, std::nullopt), nullptr);

    writeFileTx(*tx, "uui/uuid-5/tmp_insert_all_1_1_0/a.txt", "v2");
    tx->moveDirectory("uui/uuid-5/tmp_insert_all_1_1_0", "uui/uuid-5/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    ASSERT_TRUE(storage->existsFile("uui/uuid-5/all_1_1_0/a.txt"));
    EXPECT_EQ(storage->getFileSize("uui/uuid-5/all_1_1_0/a.txt"), 2u);
}

/// [02941 root-cause] A carried-forward projection sidecar (createHardLink from a COMMITTED source part
/// into a mutated tmp part) must be readable through the transaction's in-flight read path BOTH at the
/// tmp build path (loadProjections runs here during MutateTask finalize) AND after the tmp->final re-key.
/// This is the exact sequence MATERIALIZE PROJECTION drives on a part that already has the projection.
/// If the in-flight read returns empty, the mutated part's in-memory projection sub-part loads with 0
/// marks (the 02941 "Empty marks file: 0, must be: 144" corruption on a same-session projection SELECT).
TEST(CaTransactionLockScope, InFlightReadCarriedForwardProjectionSidecar)
{
    auto storage = openTxStorage();

    /// 1. Commit a source part with a small INLINE projection sidecar (marks-like) + a blob.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-proj/tmp_insert_all_1_1_0/data.bin", "the-main-data-bytes");
        writeFileTx(*tx, "uui/uuid-proj/tmp_insert_all_1_1_0/aaaa.proj/data.cmrk4", "PROJMARKS9");
        tx->moveDirectory("uui/uuid-proj/tmp_insert_all_1_1_0", "uui/uuid-proj/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("uui/uuid-proj/all_1_1_0/aaaa.proj/data.cmrk4"));

    /// 2. Mutation: build a new tmp part + carry the projection sidecar forward via createHardLink.
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    writeFileTx(*tx, "uui/uuid-proj/tmp_mut_all_1_1_0_2/data.bin", "mutated-main-data");
    ca_tx.createHardLink("uui/uuid-proj/all_1_1_0/aaaa.proj/data.cmrk4",
                         "uui/uuid-proj/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4");

    /// 2a. loadProjections timing: read the carried sidecar in-flight at the TMP build path (pre-re-key).
    {
        auto buf = ca_tx.tryReadFileInFlight("uui/uuid-proj/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4", DB::ReadSettings{}, std::nullopt);
        ASSERT_NE(buf, nullptr) << "carried-forward projection sidecar not readable in-flight at the tmp path";
        std::string got; DB::readStringUntilEOF(got, *buf);
        EXPECT_EQ(got, "PROJMARKS9");
        EXPECT_EQ(ca_tx.tryGetInFlightFileSize("uui/uuid-proj/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4"),
                  std::optional<uint64_t>(10));
    }

    /// 2b. After the tmp->final re-key (Phase 1), the sidecar must still resolve at the final path.
    tx->moveDirectory("uui/uuid-proj/tmp_mut_all_1_1_0_2", "uui/uuid-proj/all_1_1_0_2");
    {
        auto buf = ca_tx.tryReadFileInFlight("uui/uuid-proj/all_1_1_0_2/aaaa.proj/data.cmrk4", DB::ReadSettings{}, std::nullopt);
        ASSERT_NE(buf, nullptr) << "carried-forward projection sidecar not readable in-flight at the final path after re-key";
        std::string got; DB::readStringUntilEOF(got, *buf);
        EXPECT_EQ(got, "PROJMARKS9");
    }

    /// 3. And after commit it is durable + correct.
    tx->commit(DB::NoCommitOptions{});
    EXPECT_EQ(storage->getFileSize("uui/uuid-proj/all_1_1_0_2/aaaa.proj/data.cmrk4"), 10u);
}

/// [TXN-ONE-PIPELINE] Audit 5: on a commit, only refs this commit CREATED are eligible for rollback;
/// a repoint of an already-existing ref is NEVER dropped as compensation. `publishStaging` returns
/// false for the repoint path (a committed ref exists), so the repoint is never recorded in
/// `created_refs` and the pre-existing part survives with its content carried forward.
TEST(CaTransactionLockScope, CommitRollbackSparesPreexistingRef)
{
    auto storage = openTxStorage();

    /// Pre-existing committed part.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-8/tmp_insert_all_1_1_0/data.bin", "orig");
        tx->moveDirectory("uui/uuid-8/tmp_insert_all_1_1_0", "uui/uuid-8/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("uui/uuid-8/all_1_1_0"));

    /// A standalone write on the committed part repoints the EXISTING ref. Even if a later part in the
    /// same commit were to fail, the existing ref must survive: the repoint returns false from
    /// `publishStaging`, so it never enters `created_refs` and is never dropped on the error path.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "uui/uuid-8/all_1_1_0/metadata_version.txt", "1");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("uui/uuid-8/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("uui/uuid-8/all_1_1_0/data.bin"));   /// original content carried forward
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

    /// Resolve the published part to its manifest and inspect placements (the Pool read API, as in
    /// gtest_cas_pool.cpp: resolveRef -> readManifest).
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

/// A create-then-remove of a new part in one transaction must discard both the manifest entries
/// and the in-flight build, so commit() leaves no ref and no live precommit behind.
TEST(CaTransactionRemove, CreateThenDirDropDoesNotPublish)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-remove-3/all_1_1_0/data.bin", "created-then-removed");
    tx->removeDirectory("uui/uuid-remove-3/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_FALSE(storage->existsDirectory("uui/uuid-remove-3/all_1_1_0"));
}
