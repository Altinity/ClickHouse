#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <Common/Exception.h>
#include <Common/MemoryTracker.h>
#include <Common/ThreadStatus.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>

#include <filesystem>
#include <future>
#include <map>

namespace DB::ErrorCodes
{
    extern const int INCORRECT_DATA;
}

/// A part or a nested scratch storage whose small files sum past the manifest's inline budget must
/// still commit: the budget bounds what rides inside the manifest, not what a part may contain.

namespace
{

constexpr size_t kib = 1024;
constexpr size_t mib = 1024 * kib;
constexpr size_t inline_file_threshold = mib;
constexpr size_t inline_total_budget = 16 * mib;

std::shared_ptr<DB::ContentAddressedMetadataStorage> openStorage(
    DB::ObjectStoragePtr objects = DB::Cas::tests::makeLocalObjectStorageForTest())
{
    auto settings = DB::Cas::tests::makeSettingsForTest(
        "test", std::filesystem::path(objects->getCommonKeyPrefix()) / "scratch");
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        objects, "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Incompressible and different for every `seed`, so no two files share a content hash.
std::string payload(size_t size, uint64_t seed)
{
    std::string bytes(size, '\0');
    uint64_t state = seed * 0x9E3779B97F4A7C15ULL + 0xD1B54A32D192ED03ULL;
    for (size_t i = 0; i < size; ++i)
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        bytes[i] = static_cast<char>(state >> 56);
    }
    return bytes;
}

void writeFileTx(DB::IMetadataTransaction & tx, const std::string & path, const std::string & bytes)
{
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(tx);
    auto buf = ca_tx.writeFile(path, 65536, DB::WriteMode::Rewrite, {});
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

/// Reads a committed part file the way a reader does: from the manifest when inline, else from its blob.
std::string readPartFile(const DB::ContentAddressedMetadataStorage & storage, const std::string & path)
{
    if (auto in_manifest = storage.tryGetInManifestBytes(path))
        return *in_manifest;
    const auto objects = storage.getStorageObjects(path);
    if (objects.size() != 1)
        throw std::runtime_error("expected one object for " + path);
    const DB::Cas::BlobLocation location{
        .key = objects[0].remote_path,
        .offset = storage.store()->poolMeta().blob_header_len,
        .length = objects[0].bytes_size};
    auto buf = storage.readBlobPayload(location, path, DB::ReadSettings{});
    std::string got;
    DB::readStringUntilEOF(got, *buf);
    return got;
}

void expectReadsBack(
    const DB::ContentAddressedMetadataStorage & storage, const std::string & part_dir,
    const std::map<std::string, std::string> & files)
{
    for (const auto & [name, bytes] : files)
    {
        const std::string got = readPartFile(storage, part_dir + "/" + name);
        EXPECT_EQ(got.size(), bytes.size()) << name;
        EXPECT_TRUE(got == bytes) << name << " does not read back byte-identical";
    }
}

std::string commitErrorOf(DB::IMetadataTransaction & tx)
{
    try
    {
        tx.commit(DB::NoCommitOptions{});
    }
    catch (const DB::Exception & e)
    {
        return std::to_string(e.code()) + ": " + e.message();
    }
    return {};
}

DB::Cas::PartManifest manifestOf(
    DB::ContentAddressedMetadataStorage & storage, const std::string & table_uuid, const std::string & ref)
{
    const auto resolved = storage.store()->resolveRef(storage.liveNamespace(table_uuid), ref);
    if (!resolved)
        throw std::runtime_error("ref " + ref + " does not resolve");
    return storage.store()->readManifest(resolved->manifest_id);
}

uint64_t inlineTotal(const DB::Cas::PartManifest & manifest)
{
    uint64_t total = 0;
    for (const auto & e : manifest.entries)
        if (e.placement == DB::Cas::EntryPlacement::Inline)
            total += e.inline_bytes.size();
    return total;
}

/// The files one text-index segment leaves in `text_index_tmp/`, named as `BuildTextIndexTransform` names them.
std::string segmentFile(size_t segment, const std::string & index, const std::string & substream)
{
    return "text_index_tmp/tmp_all_1_2_1_" + std::to_string(segment) + "_skp_idx_" + index + substream + ".idx";
}

}

/// A merge that rebuilds text indexes commits `<part>/text_index_tmp/` as its own manifest, before the
/// part's own files exist. Its segment files are each under the per-file threshold and are many.
TEST(CASInlineCapGaps, TextIndexScratchOverTheInlineBudgetCommits)
{
    auto storage = openStorage();
    const std::string part_dir = "e01/e01e01e0-0101-4101-8101-010101010101/tmp_merge_all_1_2_1";

    std::map<std::string, std::string> files;
    for (size_t segment = 0; segment < 20; ++segment)
        files[segmentFile(segment, "idx_s", ".dct")] = payload(900 * kib, segment);

    auto tx = storage->createTransaction();
    for (const auto & [name, bytes] : files)
        writeFileTx(*tx, part_dir + "/" + name, bytes);

    const std::string error = commitErrorOf(*tx);
    ASSERT_TRUE(error.empty()) << "20 files of 900 KiB did not commit: " << error;
    expectReadsBack(*storage, part_dir, files);
    EXPECT_LE(inlineTotal(manifestOf(*storage, "e01e01e0-0101-4101-8101-010101010101", "tmp_merge_all_1_2_1")),
              inline_total_budget);
}

/// The same with every stream of a segment: a small sparse index, a dictionary and postings per segment,
/// for two indexes. No single file is large; the sum is.
TEST(CASInlineCapGaps, TextIndexScratchWithAllSubstreamsCommits)
{
    auto storage = openStorage();
    const std::string part_dir = "e02/e02e02e0-0202-4202-8202-020202020202/tmp_merge_all_1_2_1";

    std::map<std::string, std::string> files;
    uint64_t seed = 1000;
    for (const char * index : {"idx_body", "idx_attrs"})
    {
        for (size_t segment = 0; segment < 40; ++segment)
        {
            files[segmentFile(segment, index, "")] = payload(4 * kib, ++seed);
            files[segmentFile(segment, index, ".dct")] = payload(200 * kib, ++seed);
            files[segmentFile(segment, index, ".pst")] = payload(60 * kib, ++seed);
        }
    }

    auto tx = storage->createTransaction();
    for (const auto & [name, bytes] : files)
        writeFileTx(*tx, part_dir + "/" + name, bytes);

    const std::string error = commitErrorOf(*tx);
    ASSERT_TRUE(error.empty()) << "240 segment files summing to about 20 MiB did not commit: " << error;
    expectReadsBack(*storage, part_dir, files);
}

/// An ordinary part: index files are written first and the files a part load reads are written last, so a
/// rule that fills the budget in write order would push exactly the load-time files out of the manifest.
TEST(CASInlineCapGaps, PartOverTheInlineBudgetCommitsAndKeepsLoadTimeFilesInline)
{
    auto storage = openStorage();
    const std::string table = "e03/e03e03e0-0303-4303-8303-030303030303/";
    const std::string tmp_part = table + "tmp_insert_all_1_1_0";
    const std::string final_part = table + "all_1_1_0";

    std::vector<std::pair<std::string, std::string>> in_write_order;
    in_write_order.emplace_back("data.bin", payload(64 * kib, 1));
    for (size_t index = 0; index < 20; ++index)
        in_write_order.emplace_back("skp_idx_t" + std::to_string(index) + ".dct.idx", payload(900 * kib, 100 + index));
    const std::vector<std::string> load_time_files{"count.txt", "columns.txt", "serialization.json", "checksums.txt"};
    in_write_order.emplace_back("count.txt", "1000");
    in_write_order.emplace_back("columns.txt", payload(34 * kib, 2));
    in_write_order.emplace_back("serialization.json", payload(222 * kib, 3));
    in_write_order.emplace_back("checksums.txt", payload(150 * kib, 4));

    auto tx = storage->createTransaction();
    for (const auto & [name, bytes] : in_write_order)
        writeFileTx(*tx, tmp_part + "/" + name, bytes);
    tx->moveDirectory(tmp_part, final_part);

    const std::string error = commitErrorOf(*tx);
    ASSERT_TRUE(error.empty()) << "a part with 18 MiB of small index files did not commit: " << error;

    expectReadsBack(*storage, final_part, {in_write_order.begin(), in_write_order.end()});
    const auto manifest = manifestOf(*storage, "e03e03e0-0303-4303-8303-030303030303", "all_1_1_0");
    for (const auto & name : load_time_files)
    {
        const auto * entry = DB::Cas::findEntry(manifest.entries, name);
        ASSERT_TRUE(entry) << name;
        EXPECT_EQ(entry->placement, DB::Cas::EntryPlacement::Inline) << name << " must stay in the manifest";
    }
    EXPECT_LE(inlineTotal(manifest), inline_total_budget);
}

/// Rewriting an existing path and adding an empty file consume no additional inline bytes.
TEST(CASInlineCapGaps, InlineTotalExactlyAtTheBudgetCommits)
{
    auto storage = openStorage();
    const std::string part_dir = "e04/e04e04e0-0404-4404-8404-040404040404/tmp_merge_all_1_2_1";

    std::map<std::string, std::string> files;
    for (size_t segment = 0; segment < 16; ++segment)
        files[segmentFile(segment, "idx_s", ".dct")] = payload(inline_file_threshold, segment);

    auto tx = storage->createTransaction();
    for (const auto & [name, bytes] : files)
        writeFileTx(*tx, part_dir + "/" + name, bytes);

    writeFileTx(*tx, part_dir + "/" + files.begin()->first, files.begin()->second);
    files["text_index_tmp/empty.idx"] = "";
    writeFileTx(*tx, part_dir + "/text_index_tmp/empty.idx", "");

    const std::string error = commitErrorOf(*tx);
    ASSERT_TRUE(error.empty()) << "16 files of 1 MiB did not commit: " << error;
    const auto manifest = manifestOf(*storage, "e04e04e0-0404-4404-8404-040404040404", "tmp_merge_all_1_2_1");
    ASSERT_EQ(manifest.entries.size(), files.size());
    EXPECT_EQ(inlineTotal(manifest), inline_total_budget);
    for (const auto & entry : manifest.entries)
        EXPECT_EQ(entry.placement, DB::Cas::EntryPlacement::Inline) << entry.path;
    expectReadsBack(*storage, part_dir, files);
}

/// One byte more than the budget must not turn a committable write into a permanent failure.
TEST(CASInlineCapGaps, InlineTotalOneByteOverTheBudgetCommits)
{
    auto storage = openStorage();

    std::map<std::string, std::string> files;
    for (size_t segment = 0; segment < 16; ++segment)
        files[segmentFile(segment, "idx_s", ".dct")] = payload(inline_file_threshold, segment);
    files[segmentFile(16, "idx_s", "")] = "x";

    const std::string table_uuid = "e05e05e0-0505-4505-8505-050505050505";
    for (const bool reverse : {false, true})
    {
        const std::string ref = reverse ? "tmp_merge_all_3_4_1" : "tmp_merge_all_1_2_1";
        const std::string directory = "e05/" + table_uuid + "/" + ref;
        auto tx = storage->createTransaction();
        if (reverse)
        {
            for (auto it = files.rbegin(); it != files.rend(); ++it)
                writeFileTx(*tx, directory + "/" + it->first, it->second);
        }
        else
        {
            for (const auto & [name, bytes] : files)
                writeFileTx(*tx, directory + "/" + name, bytes);
        }
        ASSERT_NO_THROW(tx->commit(DB::NoCommitOptions{}));
        const auto manifest = manifestOf(*storage, table_uuid, ref);
        EXPECT_EQ(inlineTotal(manifest), 15 * mib + 1);
        for (const auto & entry : manifest.entries)
        {
            const auto placement = entry.path == files.begin()->first ? DB::Cas::EntryPlacement::Blob : DB::Cas::EntryPlacement::Inline;
            EXPECT_EQ(entry.placement, placement) << entry.path;
        }
        expectReadsBack(*storage, directory, files);
    }
}

/// A standalone rewrite of one small file of a committed part republishes the whole manifest, so the
/// files the part already holds inline count against the budget again.
TEST(CASInlineCapGaps, StandaloneWriteOntoPartAtTheBudgetCommits)
{
    auto storage = openStorage();
    const std::string table = "e06/e06e06e0-0606-4606-8606-060606060606/";
    const std::string tmp_part = table + "tmp_insert_all_1_1_0";
    const std::string final_part = table + "all_1_1_0";

    std::map<std::string, std::string> files;
    files["data.bin"] = payload(64 * kib, 1);
    for (size_t index = 0; index < 16; ++index)
        files["skp_idx_t" + std::to_string(index) + ".dct.idx"] = payload(inline_file_threshold, 100 + index);
    {
        auto tx = storage->createTransaction();
        for (const auto & [name, bytes] : files)
            writeFileTx(*tx, tmp_part + "/" + name, bytes);
        tx->moveDirectory(tmp_part, final_part);
        const std::string error = commitErrorOf(*tx);
        ASSERT_TRUE(error.empty()) << "the part at exactly the budget did not commit: " << error;
    }

    files["metadata_version.txt"] = "7";
    auto tx = storage->createTransaction();
    writeFileTx(*tx, final_part + "/metadata_version.txt", files["metadata_version.txt"]);
    const std::string error = commitErrorOf(*tx);
    ASSERT_TRUE(error.empty()) << "a 1-byte file could not be added to a committed part: " << error;
    expectReadsBack(*storage, final_part, files);
}

namespace
{

const std::string uuid = "e07e07e0-0707-4707-8707-070707070707";
const std::string table_dir = "e07/" + uuid + "/";

void writeFullBudget(DB::IMetadataTransaction & tx, const std::string & directory, const std::string & prefix = {})
{
    for (size_t i = 0; i < 16; ++i)
        writeFileTx(tx, directory + "/" + prefix + std::to_string(i) + ".idx", payload(mib, i));
}

void expectBlob(DB::ContentAddressedMetadataStorage & storage, const std::string & ref, const std::string & path, const std::string & bytes)
{
    const auto manifest = manifestOf(storage, uuid, ref);
    const auto * entry = DB::Cas::findEntry(manifest.entries, path);
    ASSERT_NE(entry, nullptr) << path;
    EXPECT_EQ(entry->placement, DB::Cas::EntryPlacement::Blob);
    EXPECT_EQ(entry->blob_size, bytes.size());
    EXPECT_EQ(entry->ref, DB::Cas::poolContentHash(storage.store()->writeAlgo(), bytes));
    EXPECT_TRUE(readPartFile(storage, table_dir + ref + "/" + path) == bytes);
}

}

TEST(CASInlineCapGaps, RepointCountsOnlyRetainedBasePaths)
{
    auto storage = openStorage();
    const std::string ref = "all_1_1_0";
    const std::string dir = table_dir + ref;
    auto tx = storage->createTransaction();
    writeFullBudget(*tx, dir);
    tx->commit(DB::NoCommitOptions{});

    tx = storage->createTransaction();
    tx->unlinkFile(dir + "/0.idx", false, true);
    writeFileTx(*tx, dir + "/1.idx", "short");
    writeFileTx(*tx, dir + "/extra.idx", payload(mib, 58));
    tx->commit(DB::NoCommitOptions{});

    const auto manifest = manifestOf(*storage, uuid, ref);
    EXPECT_EQ(inlineTotal(manifest), 15 * mib + 5);
    EXPECT_FALSE(storage->existsFile(dir + "/0.idx"));
    EXPECT_EQ(readPartFile(*storage, dir + "/1.idx"), "short");
    EXPECT_TRUE(readPartFile(*storage, dir + "/extra.idx") == payload(mib, 58));
}

TEST(CASInlineCapGaps, SpillsCommittedHardlinkWithoutChangingItsSource)
{
    auto storage = openStorage();
    const std::string src = table_dir + "all_1_1_0";
    const std::string dst_ref = "all_2_2_0";
    const std::string dst = table_dir + dst_ref;
    auto tx = storage->createTransaction();
    writeFileTx(*tx, src + "/link.idx", "x");
    writeFullBudget(*tx, dst);
    tx->commit(DB::NoCommitOptions{});

    tx = storage->createTransaction();
    tx->createHardLink(src + "/link.idx", dst + "/new.idx");
    tx->commit(DB::NoCommitOptions{});
    expectBlob(*storage, dst_ref, "new.idx", "x");
    EXPECT_EQ(storage->tryGetInManifestBytes(src + "/link.idx"), "x");
    EXPECT_TRUE(readPartFile(*storage, dst + "/15.idx") == payload(mib, 15));
}

TEST(CASInlineCapGaps, PartialMutationSpillsLargestBorrowedEntry)
{
    auto storage = openStorage();
    const std::string src = table_dir + "all_1_1_0";
    const std::string ref = "tmp_mut_all_1_1_0_2";
    const auto borrowed = payload(mib, 91);
    auto tx = storage->createTransaction();
    writeFileTx(*tx, src + "/source.idx", borrowed);
    tx->commit(DB::NoCommitOptions{});

    tx = storage->createTransaction();
    tx->createHardLink(src + "/source.idx", table_dir + ref + "/a.idx");
    writeFullBudget(*tx, table_dir + ref, "fresh");
    tx->commit(DB::NoCommitOptions{});
    expectBlob(*storage, ref, "a.idx", borrowed);
    EXPECT_TRUE(readPartFile(*storage, src + "/source.idx") == borrowed);
    for (size_t i = 0; i < 16; ++i)
        EXPECT_TRUE(readPartFile(*storage, table_dir + ref + "/fresh" + std::to_string(i) + ".idx") == payload(mib, i));
}

TEST(CASInlineCapGaps, MovingStagedInlineIntoNestedBasePreservesThatBase)
{
    auto storage = openStorage();
    const std::string ref = "tmp_merge_all_1_2_1";
    const std::string dst = table_dir + ref;
    const std::string src = table_dir + "all_3_3_0";
    auto tx = storage->createTransaction();
    writeFullBudget(*tx, dst, "text_index_tmp/");
    writeFileTx(*tx, src + "/keep.idx", "keep");
    tx->commit(DB::NoCommitOptions{});

    tx = storage->createTransaction();
    writeFileTx(*tx, src + "/transfer.idx", "m");
    tx->moveFile(src + "/transfer.idx", dst + "/new.idx");
    tx->commit(DB::NoCommitOptions{});
    expectBlob(*storage, ref, "new.idx", "m");
    EXPECT_TRUE(readPartFile(*storage, dst + "/text_index_tmp/0.idx") == payload(mib, 0));
    EXPECT_FALSE(storage->existsFile(src + "/transfer.idx"));
    EXPECT_EQ(readPartFile(*storage, src + "/keep.idx"), "keep");
}

TEST(CASInlineCapGaps, ScratchExtensionAndReplacementHaveSeparateBudgets)
{
    auto storage = openStorage();
    const std::string ref = "tmp_merge_all_1_2_1";
    const std::string dir = table_dir + ref;
    auto tx = storage->createTransaction();
    writeFullBudget(*tx, dir, "text_index_tmp/");
    writeFileTx(*tx, dir + "/text_index_tmp/small", "s");
    tx->commit(DB::NoCommitOptions{});
    expectBlob(*storage, ref, "text_index_tmp/0.idx", payload(mib, 0));

    tx = storage->createTransaction();
    writeFileTx(*tx, dir + "/text_index_tmp/later", "later");
    tx->commit(DB::NoCommitOptions{});
    EXPECT_TRUE(storage->existsFile(dir + "/text_index_tmp/0.idx"));

    tx = storage->createTransaction();
    writeFullBudget(*tx, dir);
    writeFileTx(*tx, dir + "/small", "f");
    tx->commit(DB::NoCommitOptions{});
    expectBlob(*storage, ref, "0.idx", payload(mib, 0));
    EXPECT_EQ(inlineTotal(manifestOf(*storage, uuid, ref)), 15 * mib + 1);
    EXPECT_FALSE(storage->existsFile(dir + "/text_index_tmp/later"));
    EXPECT_FALSE(storage->existsFile(dir + "/text_index_tmp/0.idx"));
}

TEST(CASInlineCapGaps, UploadReadersShareBytesAndOwnIndependentPositions)
{
    std::unique_ptr<DB::ReadBuffer> first;
    std::unique_ptr<DB::ReadBuffer> second;
    {
        auto source = DB::Cas::BlobSource::fromString("abcdef");
        auto copy = source;
        first = source.open();
        second = copy.open();
    }
    EXPECT_EQ(first->position(), second->position());
    char prefix[2];
    first->readStrict(prefix, sizeof(prefix));
    std::string tail, full;
    DB::readStringUntilEOF(tail, *first);
    DB::readStringUntilEOF(full, *second);
    EXPECT_EQ(tail, "cdef");
    EXPECT_EQ(full, "abcdef");
}

namespace
{

/// The emulated backend writes a blob into a unique temporary object before renaming it.
class FailBlobWriteStorage : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;
    std::string fail_key;
    std::atomic<size_t> failures{0};

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object, DB::WriteMode mode, std::optional<DB::ObjectAttributes> attributes,
        size_t buffer_size, const DB::WriteSettings & settings) override
    {
        if (!fail_key.empty() && object.remote_path.find(fail_key + ".publish-") != std::string::npos)
        {
            ++failures;
            throw DB::Exception(DB::ErrorCodes::INCORRECT_DATA, "Fault while publishing the selected spill");
        }
        return LocalObjectStorage::writeObject(object, mode, std::move(attributes), buffer_size, settings);
    }
};

enum class Publication
{
    NewRef,
    Repoint,
    ReplaceScratch,
};

void checkSpillFailure(Publication mode)
{
    const auto directory_owner = DB::Cas::tests::makeLocalObjectStorageForTest();
    const auto root = directory_owner->getCommonKeyPrefix();
    auto objects = std::make_shared<FailBlobWriteStorage>(DB::LocalObjectStorageSettings("fault", root, false));
    auto storage = openStorage(objects);
    const auto ns = storage->liveNamespace(uuid);
    const std::string ref = "tmp_merge_all_1_2_1";
    const std::string dst = table_dir + ref;
    const std::string input = table_dir + "all_1_1_0";
    const bool repoint = mode == Publication::Repoint;
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, input + "/source.idx", "input");
        if (repoint)
            writeFullBudget(*tx, dst);
        else if (mode == Publication::ReplaceScratch)
            writeFileTx(*tx, dst + "/text_index_tmp/segment", "scratch");
        tx->commit(DB::NoCommitOptions{});
    }
    const auto base = storage->store()->resolveRef(ns, ref);
    const auto bytes = repoint ? std::string("fresh") : payload(mib, 0);
    objects->fail_key = storage->store()->layout().blobKey(DB::Cas::poolContentHash(storage->store()->writeAlgo(), bytes));
    auto publish = [&]
    {
        auto tx = storage->createTransaction();
        if (!repoint)
            writeFullBudget(*tx, dst);
        writeFileTx(*tx, dst + "/extra.idx", "fresh");
        tx->commit(DB::NoCommitOptions{});
    };

    EXPECT_THROW(publish(), DB::Exception);
    ASSERT_GT(objects->failures.load(), 0);
    EXPECT_TRUE(storage->store()->livePrecommitsForTest(ns).empty());
    const auto after = storage->store()->resolveRef(ns, ref);
    if (repoint)
    {
        ASSERT_TRUE(after && base);
        EXPECT_EQ(after->manifest_id, base->manifest_id);
        EXPECT_TRUE(readPartFile(*storage, dst + "/0.idx") == payload(mib, 0));
    }
    else
        EXPECT_FALSE(after);
    EXPECT_EQ(readPartFile(*storage, input + "/source.idx"), "input");

    objects->fail_key.clear();
    if (mode == Publication::ReplaceScratch)
    {
        auto tx = storage->createTransaction();
        writeFileTx(*tx, dst + "/text_index_tmp/segment", "rebuilt scratch");
        tx->commit(DB::NoCommitOptions{});
    }
    ASSERT_NO_THROW(publish());
    expectBlob(*storage, ref, repoint ? "extra.idx" : "0.idx", bytes);
}
}

TEST(CASInlineCapGaps, FailedSpillDoesNotPublishNewRef)
{
    checkSpillFailure(Publication::NewRef);
}

TEST(CASInlineCapGaps, FailedSpillPreservesRepointBase)
{
    checkSpillFailure(Publication::Repoint);
}

TEST(CASInlineCapGaps, FailedScratchReplacementCanRebuildFromInput)
{
    checkSpillFailure(Publication::ReplaceScratch);
}

TEST(CASInlineCapGaps, RepointRefusesBaseChangedAfterSpillUpload)
{
    auto storage = openStorage();
    const std::string ref = "all_1_1_0";
    const std::string dir = table_dir + ref;
    auto tx = storage->createTransaction();
    writeFullBudget(*tx, dir);
    tx->commit(DB::NoCommitOptions{});

    tx = storage->createTransaction();
    writeFileTx(*tx, dir + "/spill.idx", "x");
    bool uploaded = false;
    bool interposed = false;
    bool rival_committed = false;
    /// The next manifest PUT after the blob PUT stages the combined manifest on the writer
    /// thread, after fan-out has joined and before promotion checks the expected base.
    storage->store()->setEventSink([&](const DB::Cas::CasEvent & event)
    {
        if (event.type == DB::Cas::CasEventType::BlobPut)
            uploaded = true;
        if (event.type == DB::Cas::CasEventType::ManifestPut && uploaded && !interposed)
        {
            interposed = true;
            auto rival = storage->createTransaction();
            writeFileTx(*rival, dir + "/0.idx", "winner");
            rival->commit(DB::NoCommitOptions{});
            rival_committed = true;
        }
    });
    const std::string failure = commitErrorOf(*tx);
    storage->store()->setEventSink(nullptr);
    ASSERT_TRUE(rival_committed);
    EXPECT_FALSE(failure.empty());
    tx.reset();
    EXPECT_EQ(readPartFile(*storage, dir + "/0.idx"), "winner");
    EXPECT_FALSE(storage->existsFile(dir + "/spill.idx"));
    EXPECT_TRUE(storage->store()->livePrecommitsForTest(storage->liveNamespace(uuid)).empty());
}

namespace
{

void checkInlineCandidateWritePeak(size_t num_files, size_t chunks_per_file, Int64 limit)
{
#if defined(SANITIZER)
    UNUSED(num_files, chunks_per_file, limit);
    GTEST_SKIP() << "operator new is not charged to the memory tracker under sanitizers";
#else
    auto storage = openStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    const std::string directory = table_dir + "all_1_1_0/";
    const Int64 peak = std::async(std::launch::async, [&]
    {
        MemoryTracker tracker(&total_memory_tracker, VariableContext::Process, /*log_peak_memory_usage_in_destructor_=*/false);
        DB::ThreadStatus thread_status;
        thread_status.memory_tracker.setParent(&tracker);
        thread_status.untracked_memory_limit = 0;

        const std::string chunk(mib, 'x');
        std::vector<std::unique_ptr<DB::WriteBufferFromFileBase>> writers;
        for (size_t i = 0; i < num_files; ++i)
            writers.push_back(ca_tx.writeFile(directory + std::to_string(i) + ".dct.idx", mib, DB::WriteMode::Rewrite, {}));
        for (size_t i = 0; i < chunks_per_file; ++i)
            for (auto & writer : writers)
                writer->write(chunk.data(), chunk.size());
        for (auto & writer : writers)
            writer->finalize();
        return tracker.getPeak();
    }).get();
    ::testing::Test::RecordProperty("peak_bytes", std::to_string(peak));
    EXPECT_LE(peak, limit);
#endif
}

}

TEST(CASInlineCandidateMemory, ALargeInlineCandidateIsNotHeldWholeInMemory)
{
    checkInlineCandidateWritePeak(1, 64, 8 * mib);
}

TEST(CASInlineCandidateMemory, TwoLargeInlineCandidatesOpenAtOnceStayBounded)
{
    checkInlineCandidateWritePeak(2, 32, 12 * mib);
}

TEST(CASInlineCandidateMemory, BytesPastTheThresholdReachScratchBeforeFinalize)
{
    auto storage = openStorage();
    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    const std::string path = table_dir + "all_1_1_0/skip.dct.idx";
    auto writer = ca_tx.writeFile(path, 64 * kib, DB::WriteMode::Rewrite, {});
    const auto bytes = payload(2 * mib, 13);
    writer->write(bytes.data(), bytes.size());
    writer->sync();
    const auto & scratch = storage->scratchPath();
    size_t staged_bytes = 0;
    if (std::filesystem::exists(scratch))
        for (const auto & entry : std::filesystem::directory_iterator(scratch))
            if (entry.is_regular_file())
                staged_bytes += entry.file_size();
    EXPECT_EQ(staged_bytes, bytes.size());

    writer->cancel();
    EXPECT_TRUE(!std::filesystem::exists(scratch) || std::filesystem::is_empty(scratch));
    EXPECT_FALSE(tx->tryGetInFlightFileSize(path).has_value());
}

TEST(CASInlineCandidateMemory, ThresholdPreservesPlacementAndPayload)
{
    auto storage = openStorage();
    const std::string ref = "all_1_1_0";
    const std::string dir = table_dir + ref + "/";
    const auto at_limit = payload(mib, 5);
    const auto over_limit = at_limit + 'Z';
    auto tx = storage->createTransaction();
    writeFileTx(*tx, dir + "at.idx", at_limit);
    writeFileTx(*tx, dir + "over.idx", over_limit);
    tx->commit(DB::NoCommitOptions{});

    EXPECT_TRUE(storage->tryGetInManifestBytes(dir + "at.idx") == at_limit);
    expectBlob(*storage, ref, "over.idx", over_limit);
}
