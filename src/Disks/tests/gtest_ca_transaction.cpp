#include <gtest/gtest.h>
#include <base/defines.h>
#include <base/scope_guard.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <mutex>

namespace ProfileEvents
{
extern const Event CASRefRepoint;
}

namespace DB::ContentAddressedSetting
{
extern const ContentAddressedSettingsBool gc_enabled;
extern const ContentAddressedSettingsUInt64 manifest_decode_cache_bytes;
}

namespace DB::ErrorCodes
{
extern const int NOT_IMPLEMENTED;
extern const int INVALID_STATE;
extern const int CAS_OWNERSHIP_CONFLICT;
extern const int NO_REPLICA_HAS_PART;
extern const int LOGICAL_ERROR;
extern const int CORRUPTED_DATA;
}

/// [TXN-ONE-PIPELINE] CA publish-at-commit lock-scope tests.
/// Proves that a freshly-written part's FINAL manifest ref is published only by commit(); the
/// tmp->final rename (moveDirectory) is a pure re-key of the transaction-private overlay and
/// publishes nothing. This inverts the former B151 publish-at-rename behavior.

namespace
{

/// Constructs the storage but deliberately does NOT call `startup()` -- used by tests that need to
/// control when/how startup runs (e.g. injecting a late fault before the atomic publish step).
std::shared_ptr<DB::ContentAddressedMetadataStorage> makeUnstartedTxStorage()
{
    auto settings = DB::Cas::tests::makeSettingsForTest(
        "test", std::filesystem::temp_directory_path() / "ca_tx_lockscope_scratch");
    return std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
}

std::shared_ptr<DB::ContentAddressedMetadataStorage> openTxStorage()
{
    auto storage = makeUnstartedTxStorage();
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

/// Regression for STID 0883 on the CAS write path: an extreme `max_compress_block_size` (the exact
/// 2^63-1 the `04070_no_crash_extreme_compress_block_size` stateless test sets) flows into
/// `writeFile`'s `buf_size` and, unclamped, reaches `Memory::alloc` -- where the allocator's
/// `checkSize` (>= 0x8000000000000000) fires a `LOGICAL_ERROR` and aborts the server. The ordinary
/// MergeTree writers clamp compress-block sizes to 256 MiB; the CAS write buffer must do the same at
/// its own allocation site. Building the buffer with the extreme size must NOT throw/abort, and the
/// resulting buffer must be clamped -- never allocated at the extreme size.
TEST(CASContentWriteBuffer, ExtremeBufferSizeIsClampedNotPassedToAllocator)
{
    const auto scratch = std::filesystem::temp_directory_path() / "ca_extreme_bufsize_scratch";

    constexpr size_t extreme = 0x7FFFFFFFFFFFFFFFULL;   /// 2^63 - 1; unclamped this crashes the allocator
    constexpr size_t max_clamped = 256ULL * 1024 * 1024;

    std::unique_ptr<DB::Cas::CaContentWriteBuffer> buf;
    ASSERT_NO_THROW(
        buf = std::make_unique<DB::Cas::CaContentWriteBuffer>(
            scratch.string(),
            DB::Cas::BlobHashAlgo::CityHash128,
            /*buf_size=*/extreme,
            /*use_adaptive_buffer_size=*/false,
            /*adaptive_buffer_initial_size=*/extreme,
            [](const std::string &, size_t, const std::string &) {}));
    ASSERT_TRUE(buf);
    EXPECT_LE(buf->internalBuffer().size(), max_clamped)
        << "the CAS write buffer must be clamped, never allocated at the extreme compress-block size";

    buf.reset();
    std::filesystem::remove_all(scratch);
}

/// [TXN-ONE-PIPELINE] A freshly-written part is published by commit(), NOT at the tmp->final rename.
/// moveDirectory only re-keys the transaction overlay; the durable ref appears at commit().
TEST(CASTransactionLockScope, PublishHappensAtCommitNotRename)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_1_1_0/data.bin", "content-A");

    EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_1_1_0"));
    EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0"));

    tx->moveDirectory("a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_1_1_0", "a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0");

    /// Re-key only: the final ref is NOT durable yet.
    EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0"));

    tx->commit(DB::NoCommitOptions{});

    /// Published by commit().
    EXPECT_TRUE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0/data.bin"));
    EXPECT_EQ(storage->getFileSize("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0/data.bin"), 9u);
}

TEST(CASTransactionOps, TruncateFileIsNotSupported)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { ca_tx.truncateFile("a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0/data.bin", 0); });
}

/// [TXN-ONE-PIPELINE] An abandoned transaction (destructed without commit) never published, so the
/// final ref is simply absent — no early-published ref to drop.
TEST(CASTransactionLockScope, AbandonedPartLeavesNoRef)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_3_3_0/data.bin", "abandoned");
        tx->moveDirectory("a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_3_3_0", "a11/a11a11a1-1111-4111-8111-111111111111/all_3_3_0");
        EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_3_3_0"));   /// not published at the rename
        /// tx goes out of scope WITHOUT commit().
    }
    EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_3_3_0"));
}

/// [TXN-ONE-PIPELINE] commit() publishes the re-keyed part.
TEST(CASTransactionLockScope, RefPublishedByCommit)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_4_4_0/data.bin", "kept");
        tx->moveDirectory("a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_4_4_0", "a11/a11a11a1-1111-4111-8111-111111111111/all_4_4_0");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_4_4_0"));
}

/// A committed-ref rename (no staged source) must NOT spuriously publish — it goes via republishRef.
TEST(CASTransactionLockScope, CommittedRefMoveDoesNotSpuriouslyPublish)
{
    auto storage = openTxStorage();
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_2_2_0/data.bin", "payload");
        tx->moveDirectory("a11/a11a11a1-1111-4111-8111-111111111111/tmp_insert_all_2_2_0", "a11/a11a11a1-1111-4111-8111-111111111111/all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_2_2_0"));
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_2_2_0", "a11/a11a11a1-1111-4111-8111-111111111111/delete_tmp_all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/delete_tmp_all_2_2_0"));
    EXPECT_FALSE(storage->existsDirectory("a11/a11a11a1-1111-4111-8111-111111111111/all_2_2_0"));
}

namespace
{

const std::string scratch_table = "a77/a77a77a7-7777-4777-8777-777777777777/";
const std::string scratch_tmp_part = scratch_table + "tmp_merge_all_1_1_0";

/// The nested text-index sub-storage (`createTemporaryTextIndexStorage`): a SEPARATE transaction that
/// durably publishes a ref at the part's tmp name holding only `text_index_tmp/` files.
void commitNestedScratch(DB::ContentAddressedMetadataStorage & storage)
{
    auto scratch_tx = storage.createTransaction();
    writeFileTx(*scratch_tx, scratch_tmp_part + "/text_index_tmp/scratch.bin", "scratch");
    scratch_tx->commit(DB::NoCommitOptions{});
}

/// MergeTree's order after `finalize`: commit the part at its tmp name, then rename it in place.
void commitAndRenamePart(DB::ContentAddressedMetadataStorage & storage, DB::IMetadataTransaction & tx)
{
    tx.commit(DB::NoCommitOptions{});
    auto rename_tx = storage.createTransaction();
    rename_tx->moveDirectory(scratch_tmp_part, scratch_table + "all_1_1_0");
    rename_tx->commit(DB::NoCommitOptions{});
}

void expectAuthoritativeManifestOnly(DB::ContentAddressedMetadataStorage & storage)
{
    const auto ns = storage.liveNamespace("a77a77a7-7777-4777-8777-777777777777");
    const auto resolved = storage.store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const auto manifest = storage.store()->readManifest(resolved->manifest_id);
    EXPECT_TRUE(findByName(manifest.entries, "data.bin"));
    EXPECT_FALSE(findByName(manifest.entries, "scratch.bin")) << "the nested scratch must not be merged into the part";
    EXPECT_FALSE(storage.existsDirectory(scratch_tmp_part));
}

}

/// A merge commits the nested scratch ref, then writes the part's files (`MergeTask` text-index
/// stage): the part transaction stages over the committed scratch and must publish its own manifest.
TEST(CASTransactionLockScope, StagedFinalizeDropsForeignScratchRef)
{
    auto storage = openTxStorage();
    commitNestedScratch(*storage);
    ASSERT_TRUE(storage->existsDirectory(scratch_tmp_part));

    auto tx = storage->createTransaction();
    writeFileTx(*tx, scratch_tmp_part + "/data.bin", std::string(50000, 'D'));
    writeFileTx(*tx, scratch_tmp_part + "/checksums.txt", "checksums");
    commitAndRenamePart(*storage, *tx);

    expectAuthoritativeManifestOnly(*storage);
}

/// A mutation stages the part (hardlinks, written columns) before the nested scratch commits: the
/// scratch appears under a ref the part transaction staged as new.
TEST(CASTransactionLockScope, ScratchCommittedWhilePartIsStagedIsDropped)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, scratch_tmp_part + "/data.bin", std::string(50000, 'D'));
    commitNestedScratch(*storage);
    writeFileTx(*tx, scratch_tmp_part + "/checksums.txt", "checksums");
    commitAndRenamePart(*storage, *tx);

    expectAuthoritativeManifestOnly(*storage);
}

/// The scratch rule covers only a manifest with no top-level file: a part another writer publishes at
/// the staged name still conflicts.
TEST(CASTransactionLockScope, PartPublishedWhileStagedStillConflicts)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, scratch_tmp_part + "/data.bin", std::string(50000, 'D'));
    {
        auto other = storage->createTransaction();
        writeFileTx(*other, scratch_tmp_part + "/checksums.txt", "other");
        other->commit(DB::NoCommitOptions{});
    }
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CAS_OWNERSHIP_CONFLICT, [&] { tx->commit(DB::NoCommitOptions{}); });
    EXPECT_TRUE(storage->existsFile(scratch_tmp_part + "/checksums.txt"));
}

/// The staged-source rename applies the same rule as the commit: a scratch at the staged name is dropped,
/// a part another writer published there after staging began is a conflict, never a silent drop.
TEST(CASTransactionLockScope, RenameOfStagedPartOverACommittedPartConflicts)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, scratch_tmp_part + "/data.bin", std::string(50000, 'D'));
    {
        auto other = storage->createTransaction();
        writeFileTx(*other, scratch_tmp_part + "/checksums.txt", "other");
        other->commit(DB::NoCommitOptions{});
    }
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CAS_OWNERSHIP_CONFLICT,
        [&] { tx->moveDirectory(scratch_tmp_part, scratch_table + "all_1_1_0"); });
    EXPECT_TRUE(storage->existsFile(scratch_tmp_part + "/checksums.txt"));
    EXPECT_FALSE(storage->existsDirectory(scratch_table + "all_1_1_0"));
}

/// A second scratch commit onto a scratch ref stages no top-level file, so it merges as an ordinary
/// write onto a committed ref.
TEST(CASTransactionLockScope, ScratchOntoScratchMerges)
{
    auto storage = openTxStorage();
    commitNestedScratch(*storage);
    auto tx = storage->createTransaction();
    writeFileTx(*tx, scratch_tmp_part + "/text_index_tmp/scratch2.bin", "scratch2");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->existsFile(scratch_tmp_part + "/text_index_tmp/scratch.bin"));
    EXPECT_TRUE(storage->existsFile(scratch_tmp_part + "/text_index_tmp/scratch2.bin"));
}

/// [TXN-ONE-PIPELINE] After a tmp->final re-key, a read THROUGH the open transaction resolves the
/// staged content under the FINAL path (read-your-writes), before commit(); the inner-directory
/// overlay is likewise re-keyed and answers under the final path. The staged file lives under an
/// inner projection dir because the directory overlay tracks INNER dirs only — the part dir itself
/// answers `hasInFlightDirectory`=false by contract (removeIfNeeded clean early-return; see
/// `CASWiringInFlight`), so asserting the bare part dir would contradict that invariant.
TEST(CASTransactionLockScope, ReadYourWritesAfterReKey)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "a33/a33a33a3-3333-4333-8333-333333333333/tmp_insert_all_1_1_0/p.proj/checksums.txt", "the-checksums");
    tx->moveDirectory("a33/a33a33a3-3333-4333-8333-333333333333/tmp_insert_all_1_1_0", "a33/a33a33a3-3333-4333-8333-333333333333/all_1_1_0");

    /// The overlay answers the final path before commit (read-your-writes).
    auto buf = ca_tx.tryReadFileInFlight("a33/a33a33a3-3333-4333-8333-333333333333/all_1_1_0/p.proj/checksums.txt", DB::ReadSettings{}, std::nullopt);
    ASSERT_NE(buf, nullptr);
    std::string got;
    DB::readStringUntilEOF(got, *buf);
    EXPECT_EQ(got, "the-checksums");
    /// The inner-directory overlay is re-keyed too and resolves under the final path.
    EXPECT_TRUE(ca_tx.hasInFlightDirectory("a33/a33a33a3-3333-4333-8333-333333333333/all_1_1_0/p.proj"));
}

/// [TXN-ONE-PIPELINE] Program order in the overlay: create -> delete -> create leaves the file PRESENT
/// (no delayed delete fires after the later create); delete of a staged file makes it absent to reads.
TEST(CASTransactionLockScope, OverlayProgramOrder)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);

    writeFileTx(*tx, "a55/a55a55a5-5555-4555-8555-555555555555/tmp_insert_all_1_1_0/a.txt", "v1");
    ca_tx.unlinkFile("a55/a55a55a5-5555-4555-8555-555555555555/tmp_insert_all_1_1_0/a.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
    EXPECT_EQ(ca_tx.tryReadFileInFlight("a55/a55a55a5-5555-4555-8555-555555555555/tmp_insert_all_1_1_0/a.txt", DB::ReadSettings{}, std::nullopt), nullptr);

    writeFileTx(*tx, "a55/a55a55a5-5555-4555-8555-555555555555/tmp_insert_all_1_1_0/a.txt", "v2");
    tx->moveDirectory("a55/a55a55a5-5555-4555-8555-555555555555/tmp_insert_all_1_1_0", "a55/a55a55a5-5555-4555-8555-555555555555/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    ASSERT_TRUE(storage->existsFile("a55/a55a55a5-5555-4555-8555-555555555555/all_1_1_0/a.txt"));
    EXPECT_EQ(storage->getFileSize("a55/a55a55a5-5555-4555-8555-555555555555/all_1_1_0/a.txt"), 2u);
}

/// [02941 root-cause] A carried-forward projection sidecar (createHardLink from a COMMITTED source part
/// into a mutated tmp part) must be readable through the transaction's in-flight read path BOTH at the
/// tmp build path (loadProjections runs here during MutateTask finalize) AND after the tmp->final re-key.
/// This is the exact sequence MATERIALIZE PROJECTION drives on a part that already has the projection.
/// If the in-flight read returns empty, the mutated part's in-memory projection sub-part loads with 0
/// marks (the 02941 "Empty marks file: 0, must be: 144" corruption on a same-session projection SELECT).
TEST(CASTransactionLockScope, InFlightReadCarriedForwardProjectionSidecar)
{
    auto storage = openTxStorage();

    /// 1. Commit a source part with a small INLINE projection sidecar (marks-like) + a blob.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b01/b01b01b0-0101-4101-8101-010101010101/tmp_insert_all_1_1_0/data.bin", "the-main-data-bytes");
        writeFileTx(*tx, "b01/b01b01b0-0101-4101-8101-010101010101/tmp_insert_all_1_1_0/aaaa.proj/data.cmrk4", "PROJMARKS9");
        tx->moveDirectory("b01/b01b01b0-0101-4101-8101-010101010101/tmp_insert_all_1_1_0", "b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0/aaaa.proj/data.cmrk4"));

    /// 2. Mutation: build a new tmp part + carry the projection sidecar forward via createHardLink.
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    writeFileTx(*tx, "b01/b01b01b0-0101-4101-8101-010101010101/tmp_mut_all_1_1_0_2/data.bin", "mutated-main-data");
    ca_tx.createHardLink("b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0/aaaa.proj/data.cmrk4",
                         "b01/b01b01b0-0101-4101-8101-010101010101/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4");

    /// 2a. loadProjections timing: read the carried sidecar in-flight at the TMP build path (pre-re-key).
    {
        auto buf = ca_tx.tryReadFileInFlight("b01/b01b01b0-0101-4101-8101-010101010101/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4", DB::ReadSettings{}, std::nullopt);
        ASSERT_NE(buf, nullptr) << "carried-forward projection sidecar not readable in-flight at the tmp path";
        std::string got; DB::readStringUntilEOF(got, *buf);
        EXPECT_EQ(got, "PROJMARKS9");
        EXPECT_EQ(ca_tx.tryGetInFlightFileSize("b01/b01b01b0-0101-4101-8101-010101010101/tmp_mut_all_1_1_0_2/aaaa.proj/data.cmrk4"),
                  std::optional<uint64_t>(10));
    }

    /// 2b. After the tmp->final re-key (Phase 1), the sidecar must still resolve at the final path.
    tx->moveDirectory("b01/b01b01b0-0101-4101-8101-010101010101/tmp_mut_all_1_1_0_2", "b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0_2");
    {
        auto buf = ca_tx.tryReadFileInFlight("b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0_2/aaaa.proj/data.cmrk4", DB::ReadSettings{}, std::nullopt);
        ASSERT_NE(buf, nullptr) << "carried-forward projection sidecar not readable in-flight at the final path after re-key";
        std::string got; DB::readStringUntilEOF(got, *buf);
        EXPECT_EQ(got, "PROJMARKS9");
    }

    /// 3. And after commit it is durable + correct.
    tx->commit(DB::NoCommitOptions{});
    EXPECT_EQ(storage->getFileSize("b01/b01b01b0-0101-4101-8101-010101010101/all_1_1_0_2/aaaa.proj/data.cmrk4"), 10u);
}

/// [TXN-ONE-PIPELINE] Audit 5: on a commit, only refs this commit CREATED are eligible for rollback;
/// a repoint of an already-existing ref is NEVER dropped as compensation. `publishStaging` writes a
/// `CommitOutcome` with `created=false` for the repoint path (a committed ref exists), so `commit`'s
/// rollback loop skips this slot and the pre-existing part survives with its content carried forward.
TEST(CASTransactionLockScope, CommitRollbackSparesPreexistingRef)
{
    auto storage = openTxStorage();

    /// Pre-existing committed part.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "a88/a88a88a8-8888-4888-8888-888888888888/tmp_insert_all_1_1_0/data.bin", "orig");
        tx->moveDirectory("a88/a88a88a8-8888-4888-8888-888888888888/tmp_insert_all_1_1_0", "a88/a88a88a8-8888-4888-8888-888888888888/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("a88/a88a88a8-8888-4888-8888-888888888888/all_1_1_0"));

    /// A standalone write on the committed part repoints the EXISTING ref. Even if a later part in the
    /// same commit were to fail, the existing ref must survive: `publishStaging` writes `created=false`
    /// for this slot, so `commit`'s rollback loop skips it and it is never dropped on the error path.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "a88/a88a88a8-8888-4888-8888-888888888888/all_1_1_0/metadata_version.txt", "1");
        tx->commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(storage->existsDirectory("a88/a88a88a8-8888-4888-8888-888888888888/all_1_1_0"));
    EXPECT_TRUE(storage->existsFile("a88/a88a88a8-8888-4888-8888-888888888888/all_1_1_0/data.bin"));   /// original content carried forward
}

/// Plan 2d: a small eager metadata file (checksums.txt) is staged INLINE — it rides the single tree
/// object (one-GET part open) — while per-column data (data.bin) stays a standalone Blob (preserving
/// column-read selectivity). The inlined file is still readable through the normal read path.
TEST(CASTransactionInlining, EagerFileInlinedDataBinBlobbed)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "a99/a99a99a9-9999-4999-8999-999999999999/tmp_insert_all_1_1_0/checksums.txt", "the-checksums");
    writeFileTx(*tx, "a99/a99a99a9-9999-4999-8999-999999999999/tmp_insert_all_1_1_0/data.bin", std::string(50000, 'D'));
    tx->moveDirectory("a99/a99a99a9-9999-4999-8999-999999999999/tmp_insert_all_1_1_0", "a99/a99a99a9-9999-4999-8999-999999999999/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    /// Resolve the published part to its manifest and inspect placements (the Pool read API, as in
    /// gtest_cas_pool.cpp: resolveRef -> readManifest).
    const auto ns = storage->liveNamespace("a99a99a9-9999-4999-8999-999999999999");
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
    EXPECT_EQ(storage->getFileSize("a99/a99a99a9-9999-4999-8999-999999999999/all_1_1_0/checksums.txt"), 13u);
}

TEST(CASTransactionPackedManifest, ExactArchivesAreBlobs)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    const std::string tmp_part = "c01/c01c01c0-0101-4101-8101-010101010101/tmp_insert_all_1_1_0";
    const std::string final_part = "c01/c01c01c0-0101-4101-8101-010101010101/all_1_1_0";

    writeFileTx(*tx, tmp_part + "/data.packed", "base");
    writeFileTx(*tx, tmp_part + "/p1.proj/data.packed", "projection-1");
    writeFileTx(*tx, tmp_part + "/p2.proj/data.packed", "projection-2");
    writeFileTx(*tx, tmp_part + "/checksums.txt", "base-checksums");
    writeFileTx(*tx, tmp_part + "/p1.proj/checksums.txt", "projection-1-checksums");
    writeFileTx(*tx, tmp_part + "/p2.proj/checksums.txt", "projection-2-checksums");
    tx->moveDirectory(tmp_part, final_part);
    tx->commit(DB::NoCommitOptions{});

    const auto ns = storage->liveNamespace("c01c01c0-0101-4101-8101-010101010101");
    const auto resolved = storage->store()->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(resolved.has_value());
    const auto manifest = storage->store()->readManifest(resolved->manifest_id);

    std::vector<std::string> blob_paths;
    for (const auto & entry : manifest.entries)
        if (entry.placement == DB::Cas::EntryPlacement::Blob)
            blob_paths.push_back(entry.path);
    std::sort(blob_paths.begin(), blob_paths.end());

    const std::vector<std::string> expected{
        "data.packed",
        "p1.proj/data.packed",
        "p2.proj/data.packed",
    };
    EXPECT_EQ(blob_paths, expected);
}

/// all-tree-part-files Task 4: a standalone
/// write of ONE file onto an ALREADY-COMMITTED part must carry every other file of that part forward
/// (a repoint, Task 3), never replace the manifest with just the touched file.
TEST(CASTransactionRepoint, StandaloneWriteOnCommittedPartRepoints)
{
    auto storage = openTxStorage();

    /// 1. Write a part (checksums.txt inline + data.bin blob) through a normal transaction; commit.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b02/b02b02b0-0202-4202-8202-020202020202/tmp_insert_all_1_1_0/checksums.txt", "old-checksums");
        writeFileTx(*tx, "b02/b02b02b0-0202-4202-8202-020202020202/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        tx->moveDirectory("b02/b02b02b0-0202-4202-8202-020202020202/tmp_insert_all_1_1_0", "b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0/checksums.txt"));
    ASSERT_TRUE(storage->existsFile("b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0/data.bin"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CASRefRepoint];

    /// 2. New transaction: standalone write of checksums.txt onto the ALREADY-COMMITTED part.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0/checksums.txt", "new-checksums-longer");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The new content is served, the untouched file is carried forward unchanged, exactly one
    /// repoint fired, and an independent fsck reachability walk finds nothing dangling.
    EXPECT_EQ(storage->getFileSize("b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0/checksums.txt"), 20u);
    EXPECT_EQ(storage->getFileSize("b02/b02b02b0-0202-4202-8202-020202020202/all_1_1_0/data.bin"), 14u)
        << "carry-forward: the untouched file must survive a standalone write on the same part";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRepoint], repoints_before + 1);

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
TEST(CASTransactionRepoint, CombinedWriteAndUnlinkSameTxnRepointsOnce)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with three files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b03/b03b03b0-0303-4303-8303-030303030303/tmp_insert_all_1_1_0/checksums.txt", "old-checksums");
        writeFileTx(*tx, "b03/b03b03b0-0303-4303-8303-030303030303/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "b03/b03b03b0-0303-4303-8303-030303030303/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("b03/b03b03b0-0303-4303-8303-030303030303/tmp_insert_all_1_1_0", "b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CASRefRepoint];

    /// 2. ONE transaction: write checksums.txt (new bytes) AND unlink txn_version.txt (a DIFFERENT
    /// file of the same part) -- must resolve to exactly one repoint carrying both changes plus the
    /// untouched data.bin.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/checksums.txt", "new-checksums-longer");
        tx->unlinkFile("b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The written file is updated, the unlinked file is honestly gone, the untouched file survives
    /// (carry-forward), exactly ONE repoint fired (not two, not zero), and fsck finds nothing dangling.
    EXPECT_EQ(storage->getFileSize("b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/checksums.txt"), 20u);
    EXPECT_FALSE(storage->existsFile("b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("b03/b03b03b0-0303-4303-8303-030303030303/all_1_1_0/data.bin"), 14u)
        << "carry-forward: the untouched file must survive a combined write+unlink on the same part";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRepoint], repoints_before + 1)
        << "one uncommitted transaction combining a write and an unlink must resolve to exactly one repoint";

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 6: the mutable-
/// per-part-file branch is deleted from `writeFile` -- uuid.txt/metadata_version.txt/txn_version.txt
/// now flow down the ordinary content path, landing in the manifest like any other file.
TEST(CASTransactionAllTree, BuildTimeSidecarsLandInManifest)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "b04/b04b04b0-0404-4404-8404-040404040404/tmp_insert_all_1_1_0/uuid.txt", "part-uuid-bytes");
    writeFileTx(*tx, "b04/b04b04b0-0404-4404-8404-040404040404/tmp_insert_all_1_1_0/metadata_version.txt", "3");
    writeFileTx(*tx, "b04/b04b04b0-0404-4404-8404-040404040404/tmp_insert_all_1_1_0/txn_version.txt", "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
    writeFileTx(*tx, "b04/b04b04b0-0404-4404-8404-040404040404/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
    tx->moveDirectory("b04/b04b04b0-0404-4404-8404-040404040404/tmp_insert_all_1_1_0", "b04/b04b04b0-0404-4404-8404-040404040404/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    const auto ns = storage->liveNamespace("b04b04b0-0404-4404-8404-040404040404");
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
    EXPECT_TRUE(storage->existsFile("b04/b04b04b0-0404-4404-8404-040404040404/all_1_1_0/metadata_version.txt"));
    EXPECT_EQ(storage->getFileSize("b04/b04b04b0-0404-4404-8404-040404040404/all_1_1_0/metadata_version.txt"), 1u);
}

/// A standalone one-shot write of txn_version.txt onto an ALREADY-COMMITTED part (the MVCC creation-
/// CSN fill-in / removal-TID rewrite shape) must repoint (Task 4), never orphan the rest of the part.
TEST(CASTransactionAllTree, CommittedTxnVersionStoreRepoints)
{
    auto storage = openTxStorage();

    /// 1. Commit a part WITHOUT txn_version.txt.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b05/b05b05b0-0505-4505-8505-050505050505/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "b05/b05b05b0-0505-4505-8505-050505050505/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        tx->moveDirectory("b05/b05b05b0-0505-4505-8505-050505050505/tmp_insert_all_1_1_0", "b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_FALSE(storage->existsFile("b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CASRefRepoint];

    /// 2. A single-op transaction writes ONLY txn_version.txt onto the already-committed part (mirrors
    /// the MVCC one-shot autocommit shape: no other file touched in this transaction).
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/txn_version.txt", "creation_tid: (2,2,00000000-0000-0000-0000-000000000000)");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. Exactly one repoint; the new file is served; the original files are intact (carry-forward).
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRepoint], repoints_before + 1);
    EXPECT_TRUE(storage->existsFile("b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/txn_version.txt"), 56u);
    EXPECT_EQ(storage->getFileSize("b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/checksums.txt"), 8u);
    EXPECT_EQ(storage->getFileSize("b05/b05b05b0-0505-4505-8505-050505050505/all_1_1_0/data.bin"), 14u);

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 8 (B123 evolution):
/// a lone surgical unlink of ONE committed content file (not followed by a whole-part removal in the
/// same transaction — the ATTACH `removeVersionMetadata` shape) must actually delete the file via a
/// repoint-remove, closing the pre-Task-8 fail-open (unlinkFile of a committed content file used to be
/// an unconditional no-op).
TEST(CASTransactionRemove, SurgicalUnlinkRepoints)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with txn_version.txt among its files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b06/b06b06b0-0606-4606-8606-060606060606/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "b06/b06b06b0-0606-4606-8606-060606060606/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "b06/b06b06b0-0606-4606-8606-060606060606/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("b06/b06b06b0-0606-4606-8606-060606060606/tmp_insert_all_1_1_0", "b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsFile("b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0/txn_version.txt"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CASRefRepoint];

    /// 2. A single-op transaction unlinks ONLY txn_version.txt on the already-committed part (mirrors
    /// ATTACH's removeVersionMetadata: no dir-drop in the same transaction).
    {
        auto tx = storage->createTransaction();
        tx->unlinkFile("b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The file is honestly gone, the untouched files survive (carry-forward), exactly one repoint
    /// fired, and an independent fsck reachability walk finds nothing dangling.
    EXPECT_FALSE(storage->existsFile("b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0/txn_version.txt"));
    EXPECT_EQ(storage->getFileSize("b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0/checksums.txt"), 8u);
    EXPECT_EQ(storage->getFileSize("b06/b06b06b0-0606-4606-8606-060606060606/all_1_1_0/data.bin"), 14u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRepoint], repoints_before + 1);

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// all-tree-part-files Task 8 (B123 evolution, spec §6): the DOMINANT CA removal path — the MergeTree
/// fast-removal shape that unlinks every part file one by one and THEN calls removeDirectory — must
/// stay exactly one ref-drop and pay ZERO repoints. The per-file removal marks staged by the unlink
/// storm are superseded by the ref-drop, not individually repointed.
TEST(CASTransactionRemove, UnlinkStormThenDirDropIsOneRefDrop)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with three files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b07/b07b07b0-0707-4707-8707-070707070707/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "b07/b07b07b0-0707-4707-8707-070707070707/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "b07/b07b07b0-0707-4707-8707-070707070707/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("b07/b07b07b0-0707-4707-8707-070707070707/tmp_insert_all_1_1_0", "b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0"));

    const uint64_t repoints_before = ProfileEvents::global_counters[ProfileEvents::CASRefRepoint];

    /// 2. The MergeTree fast-removal shape (IMergeTreeDataPart::remove, B123): unlink every file
    /// one-by-one, THEN removeDirectory the part — all in one transaction.
    {
        auto tx = storage->createTransaction();
        tx->unlinkFile("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0/checksums.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0/data.bin", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->removeDirectory("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 3. The whole part is gone via the single ref-drop; the storm of marks never repointed anything.
    EXPECT_FALSE(storage->existsDirectory("b07/b07b07b0-0707-4707-8707-070707070707/all_1_1_0"));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefRepoint], repoints_before)
        << "unlink-storm-then-dir-drop must supersede the marks, not repoint per file";

    const auto rep = DB::Cas::runFsck(*storage->store(), /*detail*/false);
    EXPECT_EQ(rep.dangling, 0u);
}

/// Memoizing the per-file `ForceFresh` read per (transaction, ref) saves a ref resolve and a view
/// rebuild on every file of the burst after the first.
TEST(CASTransactionRemove, UnlinkStormMemoizesOneForceFreshResolve)
{
    auto storage = openTxStorage();

    /// 1. Commit a part with three files.
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, "b09/b09b09b0-0909-4909-8909-090909090909/tmp_insert_all_1_1_0/checksums.txt", "cs-bytes");
        writeFileTx(*tx, "b09/b09b09b0-0909-4909-8909-090909090909/tmp_insert_all_1_1_0/data.bin", "the-data-bytes");
        writeFileTx(*tx, "b09/b09b09b0-0909-4909-8909-090909090909/tmp_insert_all_1_1_0/txn_version.txt",
            "creation_tid: (1,1,00000000-0000-0000-0000-000000000000)");
        tx->moveDirectory("b09/b09b09b0-0909-4909-8909-090909090909/tmp_insert_all_1_1_0", "b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_TRUE(storage->existsDirectory("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0"));

    /// A warm view-cache hit deliberately emits no `RefResolve`, so this counts exactly the calls that
    /// did real resolve work on the part.
    size_t resolves = 0;
    storage->store()->setEventSink([&](DB::Cas::CasEvent e)
    {
        if (e.type == DB::Cas::CasEventType::RefResolve && e.ref_name == "all_1_1_0")
            ++resolves;
    });

    size_t resolves_after_unlinks = 0;
    /// 2. The MergeTree fast-removal shape: unlink every file one-by-one, THEN removeDirectory — all
    /// in ONE transaction (mirrors UnlinkStormThenDirDropIsOneRefDrop above).
    {
        auto tx = storage->createTransaction();
        tx->unlinkFile("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0/checksums.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0/data.bin", /*if_exists=*/false, /*should_remove_objects=*/true);
        tx->unlinkFile("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0/txn_version.txt", /*if_exists=*/false, /*should_remove_objects=*/true);
        resolves_after_unlinks = resolves;
        tx->removeDirectory("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0");
        tx->commit(DB::NoCommitOptions{});
    }

    storage->store()->setEventSink(nullptr);

    /// 3. The whole part is gone, and the three-file burst resolved it exactly once: the first unlink's
    /// ForceFresh read, with the other two served from the retained view it left behind. Without the
    /// memo each unlink would resolve again.
    EXPECT_FALSE(storage->existsDirectory("b09/b09b09b0-0909-4909-8909-090909090909/all_1_1_0"));
    EXPECT_EQ(resolves_after_unlinks, 1u)
        << "an unlink storm over one ref must resolve it once for the whole burst, not once per file";
}

/// A create-then-remove of a new part in one transaction must discard both the manifest entries
/// and the in-flight build, so commit() leaves no ref and no live precommit behind.
TEST(CASTransactionRemove, CreateThenDirDropDoesNotPublish)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "b08/b08b08b0-0808-4808-8808-080808080808/all_1_1_0/data.bin", "created-then-removed");
    tx->removeDirectory("b08/b08b08b0-0808-4808-8808-080808080808/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_FALSE(storage->existsDirectory("b08/b08b08b0-0808-4808-8808-080808080808/all_1_1_0"));
}

/// [Task 3] `startup()` publishes `cas_store`/`part_access`/`gc_scheduler` (and sets
/// `pool_uuid`) atomically as its LAST action. Everything
/// before that point -- opening the pool, building the part-folder facade, the capability probe,
/// starting the GC scheduler -- happens into locals first, so a throw anywhere along the way (here
/// simulated via `startup_fault_injection_for_test`, injected right before the publish step) must
/// leave nothing published: `store()` still refuses (null pool -- the Constructing lifecycle,
/// `INVALID_STATE` "not started") even though `Pool::open` and everything else already succeeded.
/// Clearing the hook and retrying `startup()` must then succeed cleanly.
TEST(CASTransactionLifecycle, StartupFailureLatePublishesNothing)
{
    auto storage = makeUnstartedTxStorage();

    storage->startup_fault_injection_for_test = [] { throw std::runtime_error("injected late-startup failure"); };
    EXPECT_ANY_THROW(storage->startup());
    /// Nothing was published by the failed attempt: store() must still refuse (null pool, not started).
    EXPECT_ANY_THROW(storage->store());

    storage->startup_fault_injection_for_test = {};
    EXPECT_NO_THROW(storage->startup());
    EXPECT_NO_THROW(storage->store());
}

/// [Task 4] The storage-lifecycle gate: `store()` (and every other caller of `poolAccess()`) must
/// refuse with `INVALID_STATE` -- an operational condition, not a programming invariant -- whenever
/// no pool is published (the null-pool ShutDown lifecycle after `shutdown`). This is a deliberate
/// behavior change from the previous `LOGICAL_ERROR "accessed before startup"`, which would abort a
/// debug/sanitizer build on a mis-sequenced access instead of surfacing a catchable, operator-actionable
/// error.
TEST(CASTransactionWiring, OperationsRefuseWithoutPublishedPool)
{
    auto storage = openTxStorage();
    storage->shutdown();   /// terminal path resets cas_store to null (the ShutDown storage lifecycle)

    try
    {
        storage->store();
        FAIL() << "store() must refuse once the disk has no published pool";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::INVALID_STATE) << e.message();
    }
}

/// (rev.8, Task 15) The Dormant/UNMOUNT lifecycle rollback flips the transitional benign-absent probe
/// behavior to fail-loud on a NULL pool. A storage with no published pool (here: after `shutdown()`, the
/// null-pool ShutDown storage lifecycle) refuses the ENTIRE surface -- including the read-only
/// existence/enumeration probes that the old (now-deleted) `DormantDiskAnswersExistenceProbesAsAbsent`
/// asserted answered benign-absent. This is spec §1's null-pool fail-loud contract: every op class,
/// `Probe` included, throws `INVALID_STATE` ("not started"); a genuinely `Vanished` POOL is the only
/// state that answers truth-absent, and a null pool is not that. (Behavior change documented in the
/// Task 15 report: generic all-disk existence sweeps during server shutdown now see a throw here, not a
/// benign absent; the shutdown window is the deliberate cost of never lying about a not-started disk.)
TEST(CASLifecycle, ShutdownDiskProbesFailLoud)
{
    auto storage = openTxStorage();
    storage->shutdown();   /// null pool -- the ShutDown storage lifecycle

    const std::string file = "a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0/data.bin";
    const std::string part_dir = "a11/a11a11a1-1111-4111-8111-111111111111/all_1_1_0";

    /// Every read-only probe now THROWS (not started), where the transitional Dormant path answered benign.
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { storage->existsDirectory("store"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { storage->existsFile(file); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { storage->existsFileOrDirectory(part_dir); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { storage->isDirectoryEmpty("store"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { (void)storage->listDirectory("store"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { (void)storage->iterateDirectory("store"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { (void)storage->getStorageObjectsIfExist(file); });

    /// The content/size surface stays fail-close too (unchanged from the transitional behavior).
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { (void)storage->getFileSize(file); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&] { (void)storage->getStorageObjects(file); });
}

/// ---- No-bytes copies confirm their source after the destination precommit ----

namespace
{

/// Commits one part `table + part` holding `files` (path -> bytes).
void commitPartTx(DB::ContentAddressedMetadataStorage & storage, const std::string & table, const std::string & part,
                  const std::vector<std::pair<std::string, std::string>> & files)
{
    auto tx = storage.createTransaction();
    for (const auto & [name, bytes] : files)
        writeFileTx(*tx, table + "tmp_insert_" + part + "/" + name, bytes);
    tx->moveDirectory(table + "tmp_insert_" + part, table + part);
    tx->commit(DB::NoCommitOptions{});
}

void dropPartTx(DB::ContentAddressedMetadataStorage & storage, const std::string & part_dir)
{
    auto tx = storage.createTransaction();
    tx->removeRecursive(part_dir, {});
    tx->commit(DB::NoCommitOptions{});
}

/// Counts the pool's confirmations (`Pool::confirmPinnedRef` for a copy); each runs the hook once, whatever it answers.
struct ConfirmCounter
{
    DB::Cas::PoolPtr store;
    size_t calls = 0;
    std::function<void()> on_call;

    explicit ConfirmCounter(DB::Cas::PoolPtr store_) : store(std::move(store_))
    {
        store->setConfirmAfterOwnershipCaptureHookForTest([this]
        {
            ++calls;
            if (on_call)
                on_call();
        });
    }
    ~ConfirmCounter() { store->setConfirmAfterOwnershipCaptureHookForTest(nullptr); }
};

const std::string kLivenessTable = "a12/a12a12a1-1212-4121-8121-121212121212";
const std::string kLivenessUuid = "a12a12a1-1212-4121-8121-121212121212";

}

/// `createHardLink` reads the committed source at call time and the destination precommits only at commit.
/// A source dropped in between must not let the copy commit.
TEST(CASTransactionSourceLiveness, HardLinkOfASourceDroppedBeforeCommitIsRefused)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_71_71_0", {{"data.bin", "source-liveness-hardlink"}});
    ASSERT_TRUE(storage->existsFile(table + "all_71_71_0/data.bin"));

    auto copy_tx = storage->createTransaction();
    copy_tx->createHardLink(table + "all_71_71_0/data.bin", table + "tmp_clone_all_72_72_0/data.bin");
    dropPartTx(*storage, table + "all_71_71_0");
    ASSERT_FALSE(storage->existsDirectory(table + "all_71_71_0")) << "the source was not dropped";

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { copy_tx->commit(DB::NoCommitOptions{}); });
    EXPECT_FALSE(storage->existsDirectory(table + "tmp_clone_all_72_72_0")) << "the copy of a dropped source was promoted";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    for (const auto & [ref, manifest] : storage->store()->livePrecommitsForTest(ns))
        EXPECT_NE(ref, "tmp_clone_all_72_72_0") << "the refused copy left its precommit";
}

/// The same window with the source's body reclaimed by GC before the copy commits: the copy is refused and
/// nothing names the missing body.
TEST(CASTransactionSourceLiveness, HardLinkWhoseSourceBodyWasReclaimedIsNotPromoted)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    const DB::Cas::PoolPtr store = storage->store();
    commitPartTx(*storage, table, "all_73_73_0", {{"data.bin", "source-liveness-reclaimed-before-the-copy"}});
    const auto view = storage->partAccess()->getView({ns, "all_73_73_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr);
    ASSERT_NE(view->findFile("data.bin"), nullptr);
    const std::string body_key = store->layout().blobKey(view->findFile("data.bin")->ref);
    const auto body_present = [&]
    {
        auto op = store->mountRequests().admit();
        return op.head(body_key, DB::Cas::Retry::standard()).has_value();
    };
    ASSERT_TRUE(body_present());

    auto copy_tx = storage->createTransaction();
    copy_tx->createHardLink(table + "all_73_73_0/data.bin", table + "tmp_clone_all_74_74_0/data.bin");
    dropPartTx(*storage, table + "all_73_73_0");
    DB::Cas::Gc gc(store, DB::Cas::hexToU128("000000000000000000000000000000c1"));
    for (size_t round = 0; round < 60 && body_present(); ++round)
    {
        gc.runRegularRound();
        store->renewWatermarkOnce();
    }
    ASSERT_FALSE(body_present()) << "GC did not reclaim the dropped source's body";

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { copy_tx->commit(DB::NoCommitOptions{}); });
    EXPECT_FALSE(storage->existsDirectory(table + "tmp_clone_all_74_74_0"));
    const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
}

/// A mutation-style copy of many entries from one source confirms that source once, after the destination's
/// precommit is durable and before its promote. An entry the destination writes itself needs no confirmation.
TEST(CASTransactionSourceLiveness, ManyEntriesFromOneSourceConfirmItOnceBetweenPrecommitAndPromote)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    std::vector<std::pair<std::string, std::string>> files;
    for (size_t i = 0; i < 10; ++i)
        files.emplace_back("c" + std::to_string(i) + ".bin", "column-" + std::to_string(i) + "-of-the-source");
    commitPartTx(*storage, table, "all_75_75_0", files);

    ConfirmCounter counter(storage->store());
    bool precommit_live_at_confirm = false;
    bool committed_at_confirm = true;
    counter.on_call = [&]
    {
        for (const auto & [ref, manifest] : storage->store()->livePrecommitsForTest(ns))
            precommit_live_at_confirm |= ref == "tmp_mut_all_75_75_0_2";
        committed_at_confirm = storage->store()->resolveRef(ns, "tmp_mut_all_75_75_0_2").has_value();
    };

    auto tx = storage->createTransaction();
    for (const auto & [name, bytes] : files)
        tx->createHardLink(table + "all_75_75_0/" + name, table + "tmp_mut_all_75_75_0_2/" + name);
    writeFileTx(*tx, table + "tmp_mut_all_75_75_0_2/new.bin", "a column the mutation wrote");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_EQ(counter.calls, 1u) << "one source, one confirmation";
    EXPECT_TRUE(precommit_live_at_confirm) << "the confirmation ran before the destination precommit was durable";
    EXPECT_FALSE(committed_at_confirm) << "the confirmation ran after the destination promote";
    EXPECT_TRUE(storage->existsFile(table + "tmp_mut_all_75_75_0_2/c9.bin"));
}

/// An entry staged earlier in the same transaction is copied and renamed; the copy confirms the committed part
/// the entry came from, not the staging path it was copied from.
TEST(CASTransactionSourceLiveness, StagedCopyConfirmsTheOriginalSource)
{
    for (const bool drop_source : {false, true})
    {
        SCOPED_TRACE(drop_source ? "source dropped" : "source live");
        auto storage = openTxStorage();
        const std::string table = kLivenessTable + "/";
        commitPartTx(*storage, table, "all_76_76_0", {{"data.bin", "the original source of a staged copy"}});
        ConfirmCounter counter(storage->store());

        auto tx = storage->createTransaction();
        tx->createHardLink(table + "all_76_76_0/data.bin", table + "tmp_a_all_77_77_0/data.bin");
        tx->createHardLink(table + "tmp_a_all_77_77_0/data.bin", table + "tmp_b_all_78_78_0/data.bin");
        tx->moveDirectory(table + "tmp_b_all_78_78_0", table + "all_78_78_0");
        tx->removeDirectory(table + "tmp_a_all_77_77_0");
        if (drop_source)
            dropPartTx(*storage, table + "all_76_76_0");

        if (drop_source)
        {
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { tx->commit(DB::NoCommitOptions{}); });
            EXPECT_FALSE(storage->existsDirectory(table + "all_78_78_0"));
        }
        else
        {
            tx->commit(DB::NoCommitOptions{});
            EXPECT_TRUE(storage->existsFile(table + "all_78_78_0/data.bin"));
        }
        EXPECT_EQ(counter.calls, 1u) << "the staged copy confirmed its original source once";
    }
}

/// A copy whose source the same transaction also writes is refused: `commit` publishes `all_*` before `tmp_*`, so
/// the source no longer commits the manifest the copy read when the copy confirms it.
TEST(CASTransactionSourceLiveness, CopyOfASourceTheSameTransactionWritesIsRefused)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_82_82_0", {{"data.bin", "a source the copying transaction also writes"}});

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_82_82_0/data.bin", table + "tmp_clone_all_83_83_0/data.bin");
    writeFileTx(*tx, table + "all_82_82_0/txn_version.txt", "written by the copying transaction");

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { tx->commit(DB::NoCommitOptions{}); });
    EXPECT_FALSE(storage->existsDirectory(table + "tmp_clone_all_83_83_0")) << "the copy was promoted";
    for (const auto & [ref, manifest] : storage->store()->livePrecommitsForTest(ns))
        EXPECT_NE(ref, "tmp_clone_all_83_83_0") << "the refused copy left its precommit";
}

/// A file hardlinked inside its committed part keeps a valid source through the transaction's own rename of that
/// part: the commit confirms the renamed part.
TEST(CASTransactionSourceLiveness, ASelfHardLinkSurvivesTheRenameOfItsCommittedPart)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_90_90_0", {{"data.bin", "a part that hardlinks a file of its own"}});

    ConfirmCounter counter(storage->store());
    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_90_90_0/data.bin", table + "all_90_90_0/copy.bin");
    tx->moveDirectory(table + "all_90_90_0", table + "all_90_90_1");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_EQ(counter.calls, 1u);
    EXPECT_FALSE(storage->existsDirectory(table + "all_90_90_0"));
    EXPECT_TRUE(storage->existsFile(table + "all_90_90_1/data.bin"));
    EXPECT_TRUE(storage->existsFile(table + "all_90_90_1/copy.bin"));
    const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
}

/// Other parts borrowing a committed part keep a valid source through the transaction's own rename of it, within
/// the table and into another table, and one confirmation per destination part covers its borrows.
TEST(CASTransactionSourceLiveness, EveryBorrowerOfARenamedSourceConfirmsTheRenamedPart)
{
    for (const bool other_table : {false, true})
    {
        SCOPED_TRACE(other_table ? "renamed into another table" : "renamed within the table");
        auto storage = openTxStorage();
        const std::string table = kLivenessTable + "/";
        const std::string renamed = other_table ? "b12/b12b12b1-1212-4121-8121-121212121212/all_91_91_0"
                                                : table + "detached/all_91_91_0";
        commitPartTx(*storage, table, "all_91_91_0", {{"data.bin", "a source renamed by the copying transaction"}});

        ConfirmCounter counter(storage->store());
        auto tx = storage->createTransaction();
        tx->createHardLink(table + "all_91_91_0/data.bin", table + "tmp_clone_all_92_92_0/data.bin");
        tx->createHardLink(table + "all_91_91_0/data.bin", table + "tmp_clone_all_93_93_0/data.bin");
        tx->moveDirectory(table + "all_91_91_0", renamed);
        tx->commit(DB::NoCommitOptions{});

        EXPECT_EQ(counter.calls, other_table ? 3u : 2u) << "the rename into another table confirms its own source too";
        EXPECT_TRUE(storage->existsFile(table + "tmp_clone_all_92_92_0/data.bin"));
        EXPECT_TRUE(storage->existsFile(table + "tmp_clone_all_93_93_0/data.bin"));
        EXPECT_TRUE(storage->existsFile(renamed + "/data.bin"));
        const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
        EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
    }
}

/// The transaction's rename follows a borrow only when it moves the manifest the borrow read: a source another
/// writer rewrote before that rename is still refused.
TEST(CASTransactionSourceLiveness, ARenameOfASourceRewrittenAfterTheBorrowDoesNotRescueTheBorrow)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_94_94_0", {{"data.bin", "a source rewritten before the rename"}});

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_94_94_0/data.bin", table + "tmp_clone_all_95_95_0/data.bin");
    {
        auto writer = storage->createTransaction();
        writeFileTx(*writer, table + "all_94_94_0/txn_version.txt", "another writer's rewrite");
        writer->commit(DB::NoCommitOptions{});
    }
    tx->moveDirectory(table + "all_94_94_0", table + "detached/all_94_94_0");

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { tx->commit(DB::NoCommitOptions{}); });
    EXPECT_FALSE(storage->existsDirectory(table + "tmp_clone_all_95_95_0"));
    EXPECT_TRUE(storage->existsFile(table + "detached/all_94_94_0/txn_version.txt")) << "positive control: the rename moved the rewrite";
}

namespace
{

/// Stages a projection under `<part>/p_1.tmp_proj` and re-keys it to `<part>/p.proj`, with or without a `p.proj`
/// already staged in the same transaction.
void rekeyProjection(DB::ContentAddressedMetadataStorage & storage, bool destination_staged)
{
    const std::string part = kLivenessTable + "/tmp_insert_all_84_84_0/";
    auto tx = storage.createTransaction();
    if (destination_staged)
        writeFileTx(*tx, part + "p.proj/columns.txt", "a projection directory staged earlier");
    writeFileTx(*tx, part + "p_1.tmp_proj/checksums.txt", "the projection being renamed");
    tx->moveDirectory(part + "p_1.tmp_proj", part + "p.proj");
}

}

#ifndef DEBUG_OR_SANITIZER_BUILD
/// Re-keying `.tmp_proj` entry by entry onto a staged `.proj` would merge two projection directories, so it fails.
TEST(CASTransactionOps, ProjectionRekeyOntoAStagedProjectionThrows)
{
    auto storage = openTxStorage();
    EXPECT_NO_THROW(rekeyProjection(*storage, /*destination_staged=*/false)) << "positive control: a free destination";
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { rekeyProjection(*storage, /*destination_staged=*/true); });
}
#else
/// The storage is built inside the child, so the parent of this death suite constructs none.
TEST(CASTransactionOpsDeathTest, ProjectionRekeyOntoAStagedProjectionAborts)
{
    EXPECT_DEATH({
        auto storage = openTxStorage();
        rekeyProjection(*storage, /*destination_staged=*/true);
    }, "");
}
#endif

/// A confirmation that fails to read is not a refusal: the commit reports that failure, with the copy's precommit
/// taken back.
TEST(CASTransactionSourceLiveness, AFailedSourceConfirmationPropagatesItsOwnError)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_85_85_0", {{"data.bin", "a source whose confirmation fails to read"}});
    ConfirmCounter counter(storage->store());
    counter.on_call = []
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected failure of the source confirmation");
    };

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_85_85_0/data.bin", table + "tmp_clone_all_86_86_0/data.bin");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { tx->commit(DB::NoCommitOptions{}); });
    EXPECT_EQ(counter.calls, 1u) << "positive control: the confirmation ran";
    EXPECT_FALSE(storage->existsDirectory(table + "tmp_clone_all_86_86_0"));
    for (const auto & [ref, manifest] : storage->store()->livePrecommitsForTest(ns))
        EXPECT_NE(ref, "tmp_clone_all_86_86_0") << "the failed copy left its precommit";
}

/// The refusal names what happened to the source and tells the user to run the statement again.
TEST(CASTransactionSourceLiveness, RefusalSaysTheSourceChangedAndToRunTheStatementAgain)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_87_87_0", {{"data.bin", "a source dropped while it is copied"}});

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_87_87_0/data.bin", table + "tmp_clone_all_88_88_0/data.bin");
    dropPartTx(*storage, table + "all_87_87_0");
    try
    {
        tx->commit(DB::NoCommitOptions{});
        ADD_FAILURE() << "the copy of a dropped source committed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NO_REPLICA_HAS_PART) << e.message();
        EXPECT_NE(e.message().find("changed or was removed while it was being copied"), std::string::npos) << e.message();
        EXPECT_NE(e.message().find("nothing was copied into"), std::string::npos) << e.message();
        EXPECT_NE(e.message().find("Run the statement again"), std::string::npos) << e.message();
    }
}

/// A source this server cannot confirm, here because its mount lost its fence, is not reported as changed: the message
/// says it could not be confirmed, with the same code and advice.
TEST(CASTransactionSourceLiveness, RefusalOfAnUnconfirmableSourceDoesNotSayItChanged)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_110_110_0", {{"data.bin", "a source this mount can no longer vouch for"}});
    ConfirmCounter counter(storage->store());
    counter.on_call = [&] { storage->store()->tripMountLost(); };

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_110_110_0/data.bin", table + "tmp_clone_all_111_111_0/data.bin");
    try
    {
        tx->commit(DB::NoCommitOptions{});
        ADD_FAILURE() << "the copy of an unconfirmed source committed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NO_REPLICA_HAS_PART) << e.message();
        EXPECT_EQ(e.message().find("changed or was removed"), std::string::npos) << e.message();
        EXPECT_NE(e.message().find("could not be confirmed"), std::string::npos) << e.message();
        EXPECT_NE(e.message().find("Run the statement again"), std::string::npos) << e.message();
    }
    EXPECT_EQ(counter.calls, 1u) << "positive control: the confirmation ran";
}

/// An abandon that fails after a refusal does not replace the refusal: the caller still gets the code it retries on.
TEST(CASTransactionSourceLiveness, RefusalSurvivesAFailedAbandon)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_89_89_0", {{"data.bin", "a source confirmed by a mount that lost its fence"}});
    ConfirmCounter counter(storage->store());
    /// A fenced-out mount answers the confirm `Unknown` and refuses the abandon's append.
    counter.on_call = [&] { storage->store()->tripMountLost(); };

    auto tx = storage->createTransaction();
    tx->createHardLink(table + "all_89_89_0/data.bin", table + "tmp_clone_all_90_90_0/data.bin");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { tx->commit(DB::NoCommitOptions{}); });
    EXPECT_EQ(counter.calls, 1u) << "positive control: the confirmation ran";
    EXPECT_FALSE(storage->store()->mayMutate()) << "positive control: the abandon ran on a fenced-out mount";
}

/// A hardlinked projection file keeps its committed source through the `.tmp_proj` re-key: the copy is still
/// refused once that source is dropped, and commits while it is live.
TEST(CASTransactionSourceLiveness, ProjectionRekeyKeepsTheCopySource)
{
    for (const bool drop_source : {false, true})
    {
        SCOPED_TRACE(drop_source ? "source dropped" : "source live");
        auto storage = openTxStorage();
        const std::string table = kLivenessTable + "/";
        commitPartTx(*storage, table, "all_91_91_0", {{"p.proj/data.bin", "a projection column of the source"}});
        const std::string part = table + "tmp_mut_all_91_91_0_2/";

        auto tx = storage->createTransaction();
        tx->createHardLink(table + "all_91_91_0/p.proj/data.bin", part + "p_1.tmp_proj/data.bin");
        writeFileTx(*tx, part + "p_1.tmp_proj/checksums.txt", "the projection's own file");
        tx->moveDirectory(part + "p_1.tmp_proj", part + "p.proj");
        if (drop_source)
        {
            dropPartTx(*storage, table + "all_91_91_0");
            DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { tx->commit(DB::NoCommitOptions{}); });
            EXPECT_FALSE(storage->existsDirectory(table + "tmp_mut_all_91_91_0_2"));
        }
        else
        {
            tx->commit(DB::NoCommitOptions{});
            EXPECT_TRUE(storage->existsFile(part + "p.proj/data.bin"));
            EXPECT_TRUE(storage->existsFile(part + "p.proj/checksums.txt"));
            EXPECT_FALSE(storage->existsFile(part + "p_1.tmp_proj/data.bin"));
        }
    }
}

/// Bytes the destination uploads itself need no source: a copy of a pending upload and a copied path that is
/// then overwritten both commit with no confirmation, even with the hardlinked source gone.
TEST(CASTransactionSourceLiveness, EntriesTheDestinationUploadsCarryNoSource)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    commitPartTx(*storage, table, "all_79_79_0", {{"data.bin", "a source that is overwritten in the copy"}});
    ConfirmCounter counter(storage->store());

    auto tx = storage->createTransaction();
    writeFileTx(*tx, table + "tmp_insert_all_80_80_0/data.bin", "bytes this transaction uploads");
    tx->createHardLink(table + "tmp_insert_all_80_80_0/data.bin", table + "tmp_clone_all_81_81_0/data.bin");
    tx->createHardLink(table + "all_79_79_0/data.bin", table + "tmp_clone_all_81_81_0/other.bin");
    writeFileTx(*tx, table + "tmp_clone_all_81_81_0/other.bin", "the copy's own bytes at the copied path");
    dropPartTx(*storage, table + "all_79_79_0");
    tx->commit(DB::NoCommitOptions{});

    EXPECT_EQ(counter.calls, 0u);
    EXPECT_TRUE(storage->existsFile(table + "tmp_clone_all_81_81_0/data.bin"));
    EXPECT_TRUE(storage->existsFile(table + "tmp_clone_all_81_81_0/other.bin"));
    const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
}

/// ---- A file staged in one part, hardlinked inside it, and moved to another part ----

namespace
{

const std::string kStagedBytes = "bytes staged in P and hardlinked inside it";

/// One way a staged file and its hardlink in part `p` are split or reduced. `kept` lists the files left in `p` that
/// must name the staged bytes after the commit.
struct StagedCopyShape
{
    const char * name;
    std::function<void(DB::IMetadataTransaction &, const std::string & p, const std::string & q1, const std::string & q2)> apply;
    std::vector<std::string> kept;
};

std::vector<StagedCopyShape> stagedCopyShapes()
{
    return {
        {"the original is moved, the hardlink stays",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string & q1, const std::string &)
         { tx.moveFile(p + "data.bin", q1 + "data.bin"); },
         {"kept.bin"}},
        {"the hardlink is moved, the original stays",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string & q1, const std::string &)
         { tx.moveFile(p + "kept.bin", q1 + "kept.bin"); },
         {"data.bin"}},
        {"both are moved to two parts",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string & q1, const std::string & q2)
         {
             tx.moveFile(p + "data.bin", q1 + "data.bin");
             tx.moveFile(p + "kept.bin", q2 + "kept.bin");
         },
         {}},
        {"hardlinked twice, the original is moved",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string & q1, const std::string &)
         {
             tx.createHardLink(p + "data.bin", p + "again.bin");
             tx.moveFile(p + "data.bin", q1 + "data.bin");
         },
         {"kept.bin", "again.bin"}},
        {"the original is removed",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string &, const std::string &)
         { tx.unlinkFile(p + "data.bin", /*if_exists=*/false, /*should_remove_objects=*/false); },
         {"kept.bin"}},
        {"the original is replaced",
         [](DB::IMetadataTransaction & tx, const std::string & p, const std::string &, const std::string &)
         {
             writeFileTx(tx, p + "other.bin", "other bytes that replace the original");
             tx.replaceFile(p + "other.bin", p + "data.bin");
         },
         {"kept.bin"}},
    };
}

/// Stages `data.bin` in `p`, hardlinks it to `kept.bin` in `p`, then applies `shape`.
void stageCopyShape(DB::IMetadataTransaction & tx, const StagedCopyShape & shape, const std::string & p,
                    const std::string & q1, const std::string & q2)
{
    writeFileTx(tx, p + "data.bin", kStagedBytes);
    tx.createHardLink(p + "data.bin", p + "kept.bin");
    shape.apply(tx, p, q1, q2);
}

bool bodyPresent(const DB::Cas::PoolPtr & store, const DB::Cas::BlobRef & ref)
{
    auto op = store->mountRequests().admit();
    return op.head(store->layout().blobKey(ref), DB::Cas::Retry::standard()).has_value();
}

/// Every file of `kept` in `part` is committed and names a body that is present.
void expectKeptFilesHaveBodies(DB::ContentAddressedMetadataStorage & storage, const DB::Cas::RootNamespace & ns,
                               const std::string & part, const std::vector<std::string> & kept)
{
    if (kept.empty())
        return;
    const auto view = storage.partAccess()->getView({ns, part}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr) << part << " is not committed";
    for (const auto & name : kept)
    {
        const auto * entry = view->findFile(name);
        ASSERT_NE(entry, nullptr) << name << " is missing from " << part;
        EXPECT_TRUE(bodyPresent(storage.store(), entry->ref)) << part << "/" << name << " names a body that was never uploaded";
    }
}

}

/// A committed part publishes before the part a staged file moved to, and that part then fails before its upload.
/// The committed part stays, so it must have uploaded every body it names itself.
TEST(CASTransactionStagedCopies, ACommittedPartUploadsWhatItKeepsWhenTheOtherPartFails)
{
    for (const auto & shape : stagedCopyShapes())
    {
        SCOPED_TRACE(shape.name);
        auto storage = openTxStorage();
        const std::string table = kLivenessTable + "/";
        const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
        commitPartTx(*storage, table, "all_100_100_0", {{"base.bin", "the committed base of P"}});
        commitPartTx(*storage, table, "all_103_103_0", {{"source.bin", "a source dropped before the commit"}});
        const std::string p = table + "all_100_100_0/";
        const std::string q1 = table + "tmp_q1_all_101_101_0/";
        const std::string q2 = table + "tmp_q2_all_102_102_0/";

        auto tx = storage->createTransaction();
        stageCopyShape(*tx, shape, p, q1, q2);
        /// `q1` sorts after `p` and is refused at its source confirmation, which runs before its upload.
        tx->createHardLink(table + "all_103_103_0/source.bin", q1 + "source.bin");
        dropPartTx(*storage, table + "all_103_103_0");
        try
        {
            tx->commit(DB::NoCommitOptions{});
            ADD_FAILURE() << "the commit succeeded with a refused source";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::NO_REPLICA_HAS_PART) << e.message();
        }

        EXPECT_FALSE(storage->existsDirectory(table + "tmp_q1_all_101_101_0"));
        ASSERT_NO_FATAL_FAILURE(expectKeptFilesHaveBodies(*storage, ns, "all_100_100_0", shape.kept));
        const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
        EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
    }
}

/// The same shapes with a new source part: every part commits and names only bodies that are present.
TEST(CASTransactionStagedCopies, ANewPartPublishesWhatItKeeps)
{
    for (const auto & shape : stagedCopyShapes())
    {
        SCOPED_TRACE(shape.name);
        auto storage = openTxStorage();
        const std::string table = kLivenessTable + "/";
        const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
        const std::string p = table + "tmp_p_all_104_104_0/";
        const std::string q1 = table + "tmp_q1_all_105_105_0/";
        const std::string q2 = table + "tmp_q2_all_106_106_0/";

        auto tx = storage->createTransaction();
        stageCopyShape(*tx, shape, p, q1, q2);
        ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

        ASSERT_NO_FATAL_FAILURE(expectKeptFilesHaveBodies(*storage, ns, "tmp_p_all_104_104_0", shape.kept));
        const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
        EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
    }
}

namespace
{

/// Records, in order, every request a local object storage receives from the CAS backend.
class WriteJournalObjectStorage : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    enum class Verb : uint8_t { Exists, Write, Read, Metadata, Remove, List, Copy, Mark };

    bool exists(const DB::StoredObject & object) const override
    {
        record(Verb::Exists, object.remote_path);
        return DB::LocalObjectStorage::exists(object);
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object, DB::WriteMode mode, std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size, const DB::WriteSettings & write_settings) override
    {
        record(Verb::Write, object.remote_path);
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object, const DB::ReadSettings & read_settings, std::optional<size_t> read_hint,
        bool use_external_buffer, bool restrict_seek) const override
    {
        record(Verb::Read, object.remote_path);
        return DB::LocalObjectStorage::readObject(object, read_settings, read_hint, use_external_buffer, restrict_seek);
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Verb::Metadata, path);
        return DB::LocalObjectStorage::getObjectMetadata(path, with_tags);
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Verb::Metadata, path);
        return DB::LocalObjectStorage::tryGetObjectMetadata(path, with_tags);
    }

    void removeObjectIfExists(const DB::StoredObject & object) override
    {
        record(Verb::Remove, object.remote_path);
        DB::LocalObjectStorage::removeObjectIfExists(object);
    }

    void removeObjectsIfExist(const DB::StoredObjects & objects) override
    {
        for (const DB::StoredObject & object : objects)
            record(Verb::Remove, object.remote_path);
        DB::LocalObjectStorage::removeObjectsIfExist(objects);
    }

    void listObjects(const std::string & path, DB::RelativePathsWithMetadata & children, size_t max_keys) const override
    {
        record(Verb::List, path);
        DB::LocalObjectStorage::listObjects(path, children, max_keys);
    }

    bool existsOrHasAnyChild(const std::string & path) const override
    {
        record(Verb::List, path);
        return DB::LocalObjectStorage::existsOrHasAnyChild(path);
    }

    void copyObject(
        const DB::StoredObject & object_from, const DB::StoredObject & object_to, const DB::ReadSettings & read_settings,
        const DB::WriteSettings & write_settings, std::optional<DB::ObjectAttributes> object_to_attributes) override
    {
        record(Verb::Copy, object_to.remote_path);
        DB::LocalObjectStorage::copyObject(object_from, object_to, read_settings, write_settings, object_to_attributes);
    }

    void mark() { record(Verb::Mark, {}); }

    void clear()
    {
        std::lock_guard lock(mutex);
        journal.clear();
    }

    /// The paths of every request after the first mark, in order.
    std::vector<std::string> pathsAfterMark() const
    {
        std::lock_guard lock(mutex);
        std::vector<std::string> paths;
        bool marked = false;
        for (const auto & [v, path] : journal)
        {
            if (v == Verb::Mark)
                marked = true;
            else if (marked)
                paths.push_back(path);
        }
        return paths;
    }

    /// Requests whose path contains `needle`, counted before and after the first mark.
    std::pair<size_t, size_t> count(Verb verb, const std::string & needle) const
    {
        std::lock_guard lock(mutex);
        std::pair<size_t, size_t> counts{0, 0};
        bool marked = false;
        for (const auto & [v, path] : journal)
        {
            if (v == Verb::Mark)
                marked = true;
            else if (v == verb && path.find(needle) != std::string::npos)
                ++(marked ? counts.second : counts.first);
        }
        return counts;
    }

private:
    void record(Verb verb, const std::string & path) const
    {
        std::lock_guard lock(mutex);
        journal.emplace_back(verb, path);
    }

    mutable std::mutex mutex;
    mutable std::vector<std::pair<Verb, std::string>> journal;
};

/// A started storage with GC off over a journaled local object storage in a fresh directory, removed at destruction.
struct JournaledTxStorage
{
    explicit JournaledTxStorage(const std::string & name, std::optional<uint64_t> manifest_decode_cache_bytes = std::nullopt)
        : root(std::filesystem::temp_directory_path() / (name + "_" + std::to_string(::getpid())))
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        object_storage = std::make_shared<WriteJournalObjectStorage>(
            DB::LocalObjectStorageSettings("test", (root / "store").string(), /*read_only_=*/false));
        auto settings = DB::Cas::tests::makeSettingsForTest("test", root / "scratch");
        settings[DB::ContentAddressedSetting::gc_enabled] = false;
        if (manifest_decode_cache_bytes)
            settings[DB::ContentAddressedSetting::manifest_decode_cache_bytes] = *manifest_decode_cache_bytes;
        storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "pool", "srv1", "", nullptr, settings);
        storage->startup();
    }

    ~JournaledTxStorage()
    {
        storage.reset();
        std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
    std::shared_ptr<WriteJournalObjectStorage> object_storage;
    std::shared_ptr<DB::ContentAddressedMetadataStorage> storage;
};

std::string joinPaths(const std::vector<std::string> & paths)
{
    std::string out;
    for (const auto & path : paths)
        out += path + "\n";
    return out;
}

}

/// A file written into a committed part and removed again in the same transaction changes nothing: the commit sends no
/// request and the part keeps its manifest.
TEST(CASTransactionStagedCopies, AFileWrittenAndRemovedInACommittedPartLeavesItsManifest)
{
    JournaledTxStorage env("ca_tx_write_unlink");
    auto & storage = *env.storage;
    const std::string table = kLivenessTable + "/";
    const std::string part = table + "all_96_96_0/";
    const DB::Cas::RootNamespace ns = storage.liveNamespace(kLivenessUuid);
    commitPartTx(storage, table, "all_96_96_0", {{"data.bin", "a committed part"}});
    const auto before = storage.partAccess()->getView({ns, "all_96_96_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(before, nullptr);

    auto tx = storage.createTransaction();
    writeFileTx(*tx, part + "extra.bin", "staged into the committed part and removed again");
    tx->unlinkFile(part + "extra.bin", /*if_exists=*/false, /*should_remove_objects=*/false);
    env.object_storage->mark();
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    const std::vector<std::string> sent = env.object_storage->pathsAfterMark();
    EXPECT_TRUE(sent.empty()) << joinPaths(sent);
    const auto after = storage.partAccess()->getView({ns, "all_96_96_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->manifestId(), before->manifestId());
    EXPECT_EQ(*after->manifest(), *before->manifest());
    EXPECT_FALSE(storage.existsFile(part + "extra.bin"));

    env.object_storage->clear();
    env.object_storage->mark();
    auto kept = storage.createTransaction();
    writeFileTx(*kept, part + "extra.bin", "staged into the committed part and kept");
    kept->commit(DB::NoCommitOptions{});
    EXPECT_FALSE(env.object_storage->pathsAfterMark().empty()) << "positive control: the same write without the removal publishes";
    EXPECT_TRUE(storage.existsFile(part + "extra.bin"));
}

namespace
{

/// The entry of `file` in the committed part `ref`, or nullopt.
std::optional<DB::Cas::ManifestEntry> committedEntry(DB::ContentAddressedMetadataStorage & storage, const DB::Cas::RootNamespace & ns,
                                                     const std::string & ref, const std::string & file)
{
    const auto view = storage.partAccess()->getView({ns, ref}, DB::Cas::Freshness::ForceFresh);
    if (!view)
        return std::nullopt;
    const auto * entry = DB::Cas::findEntry(view->manifest()->entries, file);
    if (!entry)
        return std::nullopt;
    return *entry;
}

bool sameContent(DB::Cas::ManifestEntry a, const DB::Cas::ManifestEntry & b)
{
    a.path = b.path;
    return a == b;
}

}

/// A rewrite of a committed file that is then removed removes the committed file too.
TEST(CASTransactionStagedCopies, ARewrittenCommittedFileThatIsUnlinkedLeavesTheManifest)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const std::string part = table + "all_96_96_0/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_96_96_0", {{"data.bin", "committed bytes"}, {"keep.bin", "kept"}});
    ASSERT_TRUE(committedEntry(*storage, ns, "all_96_96_0", "data.bin"));

    auto tx = storage->createTransaction();
    writeFileTx(*tx, part + "data.bin", "rewritten bytes");
    tx->unlinkFile(part + "data.bin", /*if_exists=*/false, /*should_remove_objects=*/false);
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    EXPECT_FALSE(committedEntry(*storage, ns, "all_96_96_0", "data.bin"));
    EXPECT_TRUE(committedEntry(*storage, ns, "all_96_96_0", "keep.bin"));
}

/// A rewrite of a committed file moved to another part leaves the source without it and the destination with the
/// rewritten bytes.
TEST(CASTransactionStagedCopies, ARewrittenCommittedFileMovedOutLeavesItsPart)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const std::string part = table + "all_96_96_0/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_96_96_0", {{"data.bin", "committed bytes"}, {"keep.bin", "kept"}});
    commitPartTx(*storage, table, "all_99_99_0", {{"data.bin", "rewritten bytes"}});
    const auto rewritten = committedEntry(*storage, ns, "all_99_99_0", "data.bin");
    ASSERT_TRUE(rewritten);

    auto tx = storage->createTransaction();
    writeFileTx(*tx, part + "data.bin", "rewritten bytes");
    tx->moveFile(part + "data.bin", table + "tmp_insert_all_97_97_0/data.bin");
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    EXPECT_FALSE(committedEntry(*storage, ns, "all_96_96_0", "data.bin"));
    EXPECT_TRUE(committedEntry(*storage, ns, "all_96_96_0", "keep.bin"));
    const auto moved = committedEntry(*storage, ns, "tmp_insert_all_97_97_0", "data.bin");
    ASSERT_TRUE(moved);
    EXPECT_TRUE(sameContent(*moved, *rewritten));
}

/// A staged projection directory re-keyed away from a name the committed part holds takes the committed files with it.
TEST(CASTransactionStagedCopies, ARewrittenCommittedProjectionReKeyedAwayLeavesItsName)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const std::string part = table + "all_96_96_0/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_96_96_0", {{"data.bin", "committed bytes"}, {"p_1.tmp_proj/data.bin", "committed projection"}});
    ASSERT_TRUE(committedEntry(*storage, ns, "all_96_96_0", "p_1.tmp_proj/data.bin"));

    auto tx = storage->createTransaction();
    writeFileTx(*tx, part + "p_1.tmp_proj/data.bin", "rewritten projection");
    tx->moveDirectory(part + "p_1.tmp_proj", part + "p.proj");
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    EXPECT_FALSE(committedEntry(*storage, ns, "all_96_96_0", "p_1.tmp_proj/data.bin"));
    EXPECT_TRUE(committedEntry(*storage, ns, "all_96_96_0", "p.proj/data.bin"));
}

/// Every file staged into a committed part moves to new parts: those parts publish, and the committed part keeps its
/// manifest.
TEST(CASTransactionStagedCopies, ACommittedPartEmptiedOfItsStagedWritesKeepsItsManifest)
{
    auto storage = openTxStorage();
    const std::string table = kLivenessTable + "/";
    const std::string part = table + "all_96_96_0/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    commitPartTx(*storage, table, "all_96_96_0", {{"data.bin", "a committed part"}});
    const auto before = storage->partAccess()->getView({ns, "all_96_96_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(before, nullptr);

    auto tx = storage->createTransaction();
    writeFileTx(*tx, part + "extra.bin", "staged into the committed part");
    tx->createHardLink(part + "extra.bin", part + "extra_copy.bin");
    tx->moveFile(part + "extra.bin", table + "tmp_insert_all_97_97_0/extra.bin");
    tx->moveFile(part + "extra_copy.bin", table + "tmp_insert_all_98_98_0/extra.bin");
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    EXPECT_TRUE(storage->existsFile(table + "tmp_insert_all_97_97_0/extra.bin"));
    EXPECT_TRUE(storage->existsFile(table + "tmp_insert_all_98_98_0/extra.bin"));
    const auto after = storage->partAccess()->getView({ns, "all_96_96_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->manifestId(), before->manifestId());
    EXPECT_EQ(*after->manifest(), *before->manifest());
    const DB::Cas::FsckReport report = storage->runFsckNow(/*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << DB::Cas::formatFsckSummary(report);
}

/// A borrow follows its source through the transaction's rename of it. Inside one table the move keeps the manifest, so
/// following it reads nothing; into another table the borrow takes the manifest the moved part commits.
TEST(CASTransactionSourceLiveness, ABorrowFollowsTheRenameOfItsSourceWithItsManifest)
{
    for (const bool other_table : {false, true})
    {
        SCOPED_TRACE(other_table ? "renamed into another table" : "renamed within the table");
        JournaledTxStorage env("ca_tx_borrow_rename", /*manifest_decode_cache_bytes=*/0);
        auto & storage = *env.storage;
        const std::string table = kLivenessTable + "/";
        const std::string renamed = other_table ? "b12/b12b12b1-1212-4121-8121-121212121212/all_91_91_0"
                                                : table + "detached/all_91_91_0";
        const DB::Cas::RootNamespace ns = storage.liveNamespace(kLivenessUuid);
        commitPartTx(storage, table, "all_91_91_0", {{"data.bin", "a source renamed by the copying transaction"}});
        const DB::Cas::ManifestId source_manifest
            = storage.partAccess()->getView({ns, "all_91_91_0"}, DB::Cas::Freshness::ForceFresh)->manifestId();
        const std::string source_manifest_key = storage.store()->layout().manifestKey(source_manifest);

        auto tx = storage.createTransaction();
        auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
        tx->createHardLink(table + "all_91_91_0/data.bin", table + "tmp_clone_all_92_92_0/data.bin");
        env.object_storage->clear();
        tx->moveDirectory(table + "all_91_91_0", renamed);
        const size_t source_manifest_reads = env.object_storage->count(WriteJournalObjectStorage::Verb::Read, source_manifest_key).first;

        const auto borrow = ca_tx.borrowedSourceForTest(renamed);
        ASSERT_NE(borrow, nullptr) << "the borrow did not follow the rename";
        EXPECT_EQ(borrow->manifest->ref, borrow->manifest_ref) << "the borrow holds a manifest it does not name";
        EXPECT_EQ(borrow->manifest->root_namespace_id.string(), borrow->part.ns.string());
        if (other_table)
        {
            EXPECT_FALSE(borrow->manifest_ref == source_manifest.ref);
            EXPECT_GE(source_manifest_reads, 1u) << "positive control: the republication reads the source manifest";
        }
        else
        {
            EXPECT_TRUE(borrow->manifest_ref == source_manifest.ref);
            EXPECT_EQ(source_manifest_reads, 0u) << "a move within the table read the manifest after the rename";
        }
        tx->commit(DB::NoCommitOptions{});
        EXPECT_TRUE(storage.existsFile(table + "tmp_clone_all_92_92_0/data.bin"));
    }
}

/// Both parts that name the staged bytes publish them: the first uploads the body once, the second only reads that it
/// is present.
TEST(CASTransactionStagedCopies, TheSecondPartThatNamesAStagedBodyChecksItAndDoesNotUploadIt)
{
    const auto root = std::filesystem::temp_directory_path() / ("ca_tx_staged_copy_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    SCOPE_EXIT({ std::filesystem::remove_all(root); });
    auto object_storage = std::make_shared<WriteJournalObjectStorage>(
        DB::LocalObjectStorageSettings("test", (root / "store").string(), /*read_only_=*/false));
    auto settings = DB::Cas::tests::makeSettingsForTest("test", root / "scratch");
    settings[DB::ContentAddressedSetting::gc_enabled] = false;
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "pool", "srv1", "", nullptr, settings);
    storage->startup();

    const std::string table = kLivenessTable + "/";
    const DB::Cas::RootNamespace ns = storage->liveNamespace(kLivenessUuid);
    const std::string p = table + "tmp_p_all_107_107_0/";
    const std::string q = table + "tmp_q_all_108_108_0/";
    auto tx = storage->createTransaction();
    writeFileTx(*tx, p + "data.bin", kStagedBytes);
    tx->createHardLink(p + "data.bin", p + "kept.bin");
    tx->moveFile(p + "data.bin", q + "data.bin");
    storage->setAfterPromoteHookForTest({ns, "tmp_p_all_107_107_0"}, [&] { object_storage->mark(); });
    ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));

    const auto view = storage->partAccess()->getView({ns, "tmp_p_all_107_107_0"}, DB::Cas::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr);
    ASSERT_NE(view->findFile("kept.bin"), nullptr);
    const std::string body_key = storage->store()->layout().blobKey(view->findFile("kept.bin")->ref);
    const auto [writes_by_p, writes_by_q] = object_storage->count(WriteJournalObjectStorage::Verb::Write, body_key);
    const auto [checks_by_p, checks_by_q] = object_storage->count(WriteJournalObjectStorage::Verb::Exists, body_key);
    EXPECT_EQ(writes_by_p, 1u) << "the first part did not upload the body once";
    EXPECT_GE(checks_by_p, 1u) << "the first part uploaded without checking the body first";
    EXPECT_EQ(writes_by_q, 0u) << "the second part uploaded the body again";
    EXPECT_GE(checks_by_q, 1u) << "the second part did not check the body";
    EXPECT_TRUE(storage->existsFile(q + "data.bin"));
}

namespace
{

/// Commits a part holding a blob entry with no pending upload and no copy source.
void commitUnprovenBlobEntry(DB::ContentAddressedMetadataStorage & storage)
{
    auto tx = storage.createTransaction();
    dynamic_cast<DB::ContentAddressedTransaction &>(*tx).stageUnprovenBlobEntryForTest(
        kLivenessTable + "/tmp_p_all_109_109_0/data.bin", DB::Cas::tests::blobRefOf(1), 16);
    tx->commit(DB::NoCommitOptions{});
}

}

#ifndef DEBUG_OR_SANITIZER_BUILD
/// A blob entry whose body nothing uploads or confirms is refused before its part precommits.
TEST(CASTransactionStagedCopies, ABlobEntryWithNoUploadAndNoSourceThrows)
{
    auto storage = openTxStorage();
    try
    {
        commitUnprovenBlobEntry(*storage);
        ADD_FAILURE() << "a blob entry with no upload and no source was committed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << e.message();
        EXPECT_NE(e.message().find("has neither a pending upload nor a copy source"), std::string::npos) << e.message();
    }
    EXPECT_TRUE(storage->store()->livePrecommitsForTest(storage->liveNamespace(kLivenessUuid)).empty());
}
#else
/// The storage is built inside the child, so the parent of this death suite constructs none.
TEST(CASTransactionStagedCopiesDeathTest, ABlobEntryWithNoUploadAndNoSourceAborts)
{
    EXPECT_DEATH({
        auto storage = openTxStorage();
        commitUnprovenBlobEntry(*storage);
    }, "has neither a pending upload nor a copy source");
}
#endif
