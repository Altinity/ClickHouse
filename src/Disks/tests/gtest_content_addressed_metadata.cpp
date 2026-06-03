#include <gtest/gtest.h>
#include <optional>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/DiskObjectStorage/DiskObjectStorageTransaction.h>
#include <Disks/DiskObjectStorage/Replication/ClusterConfiguration.h>
#include <Disks/DiskObjectStorage/Replication/ObjectStorageRouter.h>
#include <Disks/WriteMode.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/SharedThreadPools.h>

#include <Core/ServerUUID.h>
#include <Common/Exception.h>
#include <Common/Logger.h>
#include <Common/tests/gtest_global_context.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace DB::ContentAddressed;

/// A real server-local scratch dir for the write-buffer spill. The content-addressed write buffer
/// spills each part file here while hashing it before upload; this is a local filesystem path, never
/// an object-storage key prefix. The directory is created lazily by the buffer (create_directories).
static const std::string kCasTestScratch = "./cas_test_scratch";

TEST(ContentAddressedPoolPaths, ContentKeysFanOut)
{
    // Empty prefix yields the bare key byte-for-byte (no leading slash).
    EXPECT_EQ(blobKey("", BlobHash("abcdef0123")).string(), "blobs/ab/cd/abcdef0123");
    EXPECT_EQ(partKey("", PartId("0011223344")).string(), "parts/00/11/0011223344");
    EXPECT_EQ(blobKey("", BlobHash("ab")).string(), "blobs/ab"); // too short to fan out (test-only)

    // A non-empty prefix is prepended with a single '/' join (trailing slash collapsed).
    EXPECT_EQ(blobKey("pool", BlobHash("abcdef0123")).string(), "pool/blobs/ab/cd/abcdef0123");
    EXPECT_EQ(partKey("pool/", PartId("0011223344")).string(), "pool/parts/00/11/0011223344");
}

TEST(ContentAddressedPoolPaths, RefKeys)
{
    EXPECT_EQ(refsPrefix("", "srvA", "uuid-1"), "store/srvA/uuid-1/refs/");
    EXPECT_EQ(refKey("", "srvA", "uuid-1", "all_1_1_0").string(), "store/srvA/uuid-1/refs/all_1_1_0");
    EXPECT_EQ(refKey("pool", "srvA", "uuid-1", "all_1_1_0").string(), "pool/store/srvA/uuid-1/refs/all_1_1_0");
}

TEST(ContentAddressedPoolPaths, ParsePartFilePath)
{
    auto file = parsePartFilePath("uui/uuid-1/all_1_1_0/columns.txt");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "uuid-1");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "columns.txt");

    auto part_dir = parsePartFilePath("uui/uuid-1/all_1_1_0/"); // trailing slash, no file
    ASSERT_TRUE(part_dir.has_value());
    EXPECT_EQ(part_dir->part_name, "all_1_1_0");
    EXPECT_EQ(part_dir->file, "");

    EXPECT_FALSE(parsePartFilePath("uui/uuid-1").has_value());   // table dir, not a part
    EXPECT_FALSE(parsePartFilePath("123").has_value());          // shallower
}

// B40: a NON-Atomic database (Ordinary/Memory/Lazy) stores a table under data/<db>/<table>/, which is
// NOT the uuid-anchored store/ layout, so the parser must fall back to the MergeTree part-dir grammar
// to find the table/part boundary. Without this, every part file was misclassified as a verbatim
// non-part file (written under tmp_insert_<part>/, the tmp->final rename moved nothing, and the read
// of the final part dir found no object → read-miss data loss). The table identifier becomes the full
// data/<db>/<table> path, used identically on write and read so the ref/manifest/blob keys match.
TEST(ContentAddressedPoolPaths, ParseNonAtomicPartFilePath)
{
    // A plain part file: table id is the whole data/<db>/<table> path, the part dir is recognized by
    // its _<min>_<max>_<level> block-range suffix.
    auto file = parsePartFilePath("data/memory_01069/mt/all_1_1_0/data.cmrk4");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "data/memory_01069/mt");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "data.cmrk4");

    // A temporary insert part keeps the block-range suffix, so the SAME part is recognized before and
    // after the tmp->final rename (the rename must re-pin to the same table id, not drop the file).
    auto tmp = parsePartFilePath("data/memory_01069/mt/tmp_insert_all_1_1_0/data.cmrk4");
    ASSERT_TRUE(tmp.has_value());
    EXPECT_EQ(tmp->table_uuid, "data/memory_01069/mt");
    EXPECT_EQ(tmp->part_name, "tmp_insert_all_1_1_0");

    // A mutation-level part (4 numeric tail groups) is still a part dir.
    auto mut = parsePartFilePath("data/db/tbl/20200101_1_1_0_5/data.bin");
    ASSERT_TRUE(mut.has_value());
    EXPECT_EQ(mut->part_name, "20200101_1_1_0_5");

    // A table-level file (format_version.txt) has no part-dir component: it is NOT a part file.
    EXPECT_FALSE(isPartFilePath("data/memory_01069/mt/format_version.txt"));
    auto tf = parseTableFilePath("data/memory_01069/mt/format_version.txt");
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->table_uuid, "data/memory_01069/mt");
    EXPECT_EQ(tf->tail, "format_version.txt");

    // The table dir itself (no part) resolves to the full table id, so DROP/listing scope the refs.
    EXPECT_EQ(parseTableUuid("data/memory_01069/mt"), std::optional<std::string>("data/memory_01069/mt"));

    // A bare single-component disk-level path (e.g. the startup access-check probe) is neither a part
    // file, a table-level file, nor a table dir — it stays a verbatim disk file.
    EXPECT_FALSE(isPartFilePath("clickhouse_access_check_xyz"));
    EXPECT_FALSE(parseTableFilePath("clickhouse_access_check_xyz").has_value());
    EXPECT_FALSE(parseTableUuid("clickhouse_access_check_xyz").has_value());

    // The Atomic layout is unchanged: the table id stays the single <uuid> component (dedup/golden).
    auto atomic = parsePartFilePath("store/uui/uuid-1/all_1_1_0/data.bin");
    ASSERT_TRUE(atomic.has_value());
    EXPECT_EQ(atomic->table_uuid, "uuid-1");
    EXPECT_EQ(atomic->part_name, "all_1_1_0");
}

TEST(ContentAddressedPoolPaths, ParseTableUuid)
{
    EXPECT_EQ(parseTableUuid("uui/uuid-1/"), std::optional<std::string>("uuid-1"));
    EXPECT_EQ(parseTableUuid("uui/uuid-1"), std::optional<std::string>("uuid-1"));
    EXPECT_FALSE(parseTableUuid("uui/uuid-1/all_1_1_0").has_value()); // part dir, not table dir
}

namespace fs = std::filesystem;

class ContentAddressedMetaTest : public testing::Test
{
public:
    void SetUp() override
    {
        if (!initialized)
        {
            DB::ServerUUID::setRandomForUnitTests();
            DB::getIOThreadPool().initialize(1, 1, 0);
            initialized = true;
        }
    }

    std::shared_ptr<DB::ContentAddressedMetadataStorage> getMetadataStorage(const std::string & key_prefix)
    {
        std::unique_lock<std::mutex> lock(active_metadatas_mutex);

        if (!active_metadatas[key_prefix])
            active_metadatas[key_prefix] = createMetadataStorage(key_prefix);

        return std::dynamic_pointer_cast<DB::ContentAddressedMetadataStorage>(active_metadatas[key_prefix]);
    }

    std::shared_ptr<DB::IObjectStorage> getObjectStorage(const std::string & key_prefix)
    {
        std::unique_lock<std::mutex> lock(active_metadatas_mutex);
        if (!active_metadatas[key_prefix])
            active_metadatas[key_prefix] = createMetadataStorage(key_prefix);
        return active_object_storages.at(key_prefix);
    }

    static size_t writeObject(const std::shared_ptr<DB::IObjectStorage> & object_storage, const std::string & remote_path, const std::string & data)
    {
        DB::StoredObject object(remote_path);
        auto buffer = object_storage->writeObject(object, DB::WriteMode::Rewrite);
        buffer->write(data.data(), data.size());
        buffer->preFinalize();
        size_t written_bytes = buffer->count();
        buffer->finalize();
        return written_bytes;
    }

    static std::string readObject(const std::shared_ptr<DB::IObjectStorage> & object_storage, const std::string & remote_path)
    {
        DB::StoredObject object(remote_path);
        auto buffer = object_storage->readObject(object, DB::getReadSettings(), /*read_hint=*/std::nullopt);
        String content;
        DB::readStringUntilEOF(content, *buffer);
        return content;
    }

    void TearDown() override
    {
        for (const auto & [_, metadata] : active_metadatas)
            metadata->shutdown();

        for (const auto & [_, object_storage] : active_object_storages)
        {
            object_storage->shutdown();
            fs::remove_all(object_storage->getCommonKeyPrefix());
        }

        // These tests deliberately seed with an EMPTY key prefix (to prove the empty-prefix path is
        // byte-identical to the old bare keys), so the content-pool roots are created at CWD and not
        // covered by the per-storage cleanup above. Remove them so CI runs leave no stray dirs in the
        // repo root. The std::error_code overload makes a missing dir a no-op.
        std::error_code ec;
        fs::remove_all("blobs", ec);
        fs::remove_all("parts", ec);
        fs::remove_all("store", ec);
        fs::remove_all("cas_wbuf_tmp", ec);
        fs::remove_all(kCasTestScratch, ec);
    }

private:
    std::shared_ptr<DB::IMetadataStorage> createMetadataStorage(const std::string & key_prefix)
    {
        fs::remove_all("./" + key_prefix);
        DB::LocalObjectStorageSettings settings("test", "./" + key_prefix, /*read_only_=*/false);
        auto object_storage = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
        auto metadata_storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "", "test-server", kCasTestScratch);

        active_metadatas.emplace(key_prefix, metadata_storage);
        active_object_storages.emplace(key_prefix, object_storage);

        return metadata_storage;
    }

    static inline bool initialized = false;

    std::mutex active_metadatas_mutex;
    std::unordered_map<std::string, std::shared_ptr<DB::IMetadataStorage>> active_metadatas;
    std::unordered_map<std::string, std::shared_ptr<DB::IObjectStorage>> active_object_storages;
};

TEST_F(ContentAddressedMetaTest, ConstructAndType)
{
    auto ms = getMetadataStorage("cas_construct");
    EXPECT_EQ(ms->getType(), DB::MetadataStorageType::ContentAddressed);
    EXPECT_FALSE(ms->isReadOnly());
    EXPECT_FALSE(ms->areBlobPathsRandom());
    EXPECT_EQ(ms->getHardlinkCount("anything"), 0u);
}

TEST_F(ContentAddressedMetaTest, ResolvesAndReadsSeededPart)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_resolve");
    auto os = getObjectStorage("cas_resolve");
    const std::string sid = ms->serverIdForTest(); // "test-server"
    const std::string uuid = "uuid-2", part = "all_1_1_0", file = "data.bin";
    const std::string blob_data = "COLUMN-BYTES";
    const std::string blob_csum = "deadbeef01";
    const std::string part_id = "cafe112233";

    writeObject(os, blobKey("", BlobHash(blob_csum)).string(), blob_data);
    PartManifest f;
    f.blobs[file] = BlobEntry{BlobHash(blob_csum), blob_data.size(), blob_csum};
    writeObject(os, partKey("", PartId(part_id)).string(), f.serialize());
    writeObject(os, refKey("", sid, uuid, part).string(), serializeRefPayload(PartId(part_id)));

    const std::string logical = "uui/" + uuid + "/" + part + "/" + file; // <uuid[:3]>/<uuid>/<part>/<file>
    EXPECT_TRUE(ms->existsFile(logical));
    EXPECT_EQ(ms->getFileSize(logical), blob_data.size());
    auto objs = ms->getStorageObjects(logical);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, blobKey("", BlobHash(blob_csum)).string());
    EXPECT_EQ(readObject(os, objs[0].remote_path), blob_data);
    EXPECT_FALSE(ms->existsFile("uui/" + uuid + "/" + part + "/absent.bin")); // file not in manifest
}

TEST_F(ContentAddressedMetaTest, ExistsFileOnDirectoryShapedPoolPathReturnsFalse)
{
    using namespace DB::ContentAddressed;
    // B38: system.remote_data_paths traversal probes existsFile on pool sub-dirs (e.g. "store").
    // Such a path resolves to a directory object key; existsFile/existsFileOrDirectory/getFileSize
    // must treat a directory as not-a-file (return false / FILE_DOESNT_EXIST), never let the raw
    // filesystem "Is a directory" error escape.
    auto ms = getMetadataStorage("cas_dir_safe");

    // existsFile resolves a generic (non-part) path "store" to a verbatim object key probed via
    // LocalObjectStorage::tryGetObjectMetadata. With the unit-test empty key prefix that key is
    // relative to the CWD; make it a real directory so the probe hits a directory, exactly as the
    // pool sub-dir "store" is on a real server. The directory is cleaned up in TearDown.
    fs::create_directories("store");
    ASSERT_TRUE(fs::is_directory("store"));

    // A directory-shaped pool path is not a file and must not let the raw "Is a directory" FS error
    // escape: existsFile/existsFileOrDirectory return false; getFileSize fails closed (FILE_DOESNT_EXIST).
    EXPECT_NO_THROW(EXPECT_FALSE(ms->existsFile("store")));
    EXPECT_NO_THROW(EXPECT_FALSE(ms->existsFileOrDirectory("store")));
    EXPECT_THROW(ms->getFileSize("store"), DB::Exception);
}

TEST_F(ContentAddressedMetaTest, FailsClosedOnMissingManifest)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_failclose");
    auto os = getObjectStorage("cas_failclose");
    // ref present, but parts/<part_id> manifest absent → must THROW (B18), not return empty
    writeObject(os, refKey("", ms->serverIdForTest(), "uuid-3", "all_1_1_0").string(), serializeRefPayload(PartId("missingpid")));
    EXPECT_THROW(ms->getStorageObjects("uui/uuid-3/all_1_1_0/data.bin"), DB::Exception);
}

TEST_F(ContentAddressedMetaTest, ListsPartsAndPartFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_list");
    auto os = getObjectStorage("cas_list");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-4";

    auto seed = [&](const std::string & part, const std::string & pid, const PartManifest & f)
    {
        writeObject(os, partKey("", PartId(pid)).string(), f.serialize());
        writeObject(os, refKey("", sid, uuid, part).string(), serializeRefPayload(PartId(pid)));
    };
    /// Real part ids are 32-char lowercase hex (computePartId); the ref read path resolves the
    /// payload through partIdFromRefPayload (B28), so use valid-hex ids here as in production.
    PartManifest fa; fa.blobs["data.bin"] = {BlobHash("k1"), 3, "k1"}; fa.blobs["columns.txt"] = {BlobHash("k2"), 2, "k2"};
    seed("all_1_1_0", "aaaa000000000000000000000000aaaa", fa);
    PartManifest fb; fb.blobs["data.bin"] = {BlobHash("k3"), 4, "k3"};
    seed("all_2_2_0", "bbbb000000000000000000000000bbbb", fb);

    // table dir → part names
    auto parts = ms->listDirectory("uui/" + uuid + "/");
    std::set<std::string> got(parts.begin(), parts.end());
    EXPECT_EQ(got, (std::set<std::string>{"all_1_1_0", "all_2_2_0"}));

    // part dir → file names
    auto files = ms->listDirectory("uui/" + uuid + "/all_1_1_0");
    std::set<std::string> gotf(files.begin(), files.end());
    EXPECT_EQ(gotf, (std::set<std::string>{"data.bin", "columns.txt"}));

    // iterateDirectory over the table dir yields the same part names
    // StaticDirectoryIterator::name() returns the basename (filename component),
    // i.e. the part name, of each prepended child path.
    std::set<std::string> iter_names;
    for (auto it = ms->iterateDirectory("uui/" + uuid + "/"); it->isValid(); it->next())
        iter_names.insert(it->name());
    EXPECT_EQ(iter_names, (std::set<std::string>{"all_1_1_0", "all_2_2_0"}));

    EXPECT_TRUE(ms->existsDirectory("uui/" + uuid + "/all_1_1_0"));
    EXPECT_TRUE(ms->existsFileOrDirectory("uui/" + uuid + "/all_1_1_0/data.bin"));
}

TEST_F(ContentAddressedMetaTest, DetachedDirListsPartDirNamesNotInnerFiles)
{
    using namespace DB::ContentAddressed;
    // B36: a detached part clones file-by-file into detached/<detached_part>/<file>, so the ref named
    // "detached" carries manifest keys shaped <detached_part>/<file> plus a dir-stripped mutable file.
    // Enumerating the detached dir must yield the detached part DIRECTORY name (all_1_2_1), never the
    // inner files nor the dir-stripped mutable sidecar file (metadata_version.txt).
    auto ms = getMetadataStorage("cas_detached");
    auto os = getObjectStorage("cas_detached");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-det";

    PartManifest f;
    f.blobs["all_1_2_1/data.bin"] = {BlobHash("k1"), 3, "k1"};
    f.blobs["all_1_2_1/columns.txt"] = {BlobHash("k2"), 2, "k2"};
    // A dir-stripped mutable file (basename keying drops the all_1_2_1/ prefix): must NOT appear.
    f.blobs["metadata_version.txt"] = {BlobHash("k3"), 1, "k3"};
    writeObject(os, partKey("", PartId("dddd000000000000000000000000dddd")).string(), f.serialize());
    writeObject(os, refKey("", sid, uuid, "detached").string(), serializeRefPayload(PartId("dddd000000000000000000000000dddd")));

    auto names = ms->listDirectory("uui/" + uuid + "/detached");
    std::set<std::string> got(names.begin(), names.end());
    EXPECT_EQ(got, (std::set<std::string>{"all_1_2_1"}));

    std::set<std::string> iter_names;
    for (auto it = ms->iterateDirectory("uui/" + uuid + "/detached"); it->isValid(); it->next())
        iter_names.insert(it->name());
    EXPECT_EQ(iter_names, (std::set<std::string>{"all_1_2_1"}));
}

TEST_F(ContentAddressedMetaTest, TransactionWriteNotImplementedYet)
{
    auto ms = getMetadataStorage("cas_tx");
    auto tx = ms->createTransaction();
    ASSERT_NE(tx, nullptr);
    EXPECT_THROW(tx->writeStringToFile("uui/uuid-5/all_1_1_0/x", "y"), DB::Exception); // Phase 3
}

TEST_F(ContentAddressedMetaTest, WriteBufferUploadsContentAddressed)
{
    using namespace DB::ContentAddressed;
    auto os = getObjectStorage("cas_wbuf");
    std::string hash1;
    {
        ContentAddressedWriteBuffer buf(os, "", "./cas_wbuf_tmp");
        buf.write("HELLO-COLUMN", 12);
        buf.finalize();
        hash1 = buf.getBlobHash();
        EXPECT_EQ(buf.getSize(), 12u);
    }
    EXPECT_FALSE(hash1.empty());
    // bytes landed at blobs/<hash> and read back:
    EXPECT_EQ(readObject(os, blobKey("", BlobHash(hash1)).string()), "HELLO-COLUMN");

    // idempotent: identical content → same hash, no second upload (object already present)
    {
        ContentAddressedWriteBuffer buf2(os, "", "./cas_wbuf_tmp");
        buf2.write("HELLO-COLUMN", 12);
        buf2.finalize();
        EXPECT_EQ(buf2.getBlobHash(), hash1);
    }
    // different content → different hash + its own object
    std::string hash2;
    {
        ContentAddressedWriteBuffer buf3(os, "", "./cas_wbuf_tmp");
        buf3.write("OTHER", 5);
        buf3.finalize();
        hash2 = buf3.getBlobHash();
    }
    EXPECT_NE(hash2, hash1);
    EXPECT_EQ(readObject(os, blobKey("", BlobHash(hash2)).string()), "OTHER");
}

TEST(ContentAddressedPartId, DeterministicAndExcludesMutableFiles)
{
    using namespace DB::ContentAddressed;
    std::map<std::string, BlobEntry> a;
    a["a.bin"] = {BlobHash("h1"), 3, "h1"};
    a["b.bin"] = {BlobHash("h2"), 6, "h2"};

    // Order of insertion does not matter (std::map sorts), so an equal logical map => equal id.
    std::map<std::string, BlobEntry> a2;
    a2["b.bin"] = {BlobHash("h2"), 6, "h2"};
    a2["a.bin"] = {BlobHash("h1"), 3, "h1"};
    EXPECT_EQ(computePartId(a), computePartId(a2));

    // Mutable files do not contribute to the identity.
    std::map<std::string, BlobEntry> with_mutable = a;
    with_mutable["uuid.txt"] = {BlobHash("u"), 1, "u"};
    with_mutable["txn_version.txt"] = {BlobHash("t"), 1, "t"};
    with_mutable["metadata_version.txt"] = {BlobHash("m"), 1, "m"};
    EXPECT_EQ(computePartId(a), computePartId(with_mutable));

    // A different column checksum changes the identity.
    std::map<std::string, BlobEntry> b = a;
    b["a.bin"] = {BlobHash("h1x"), 3, "h1x"};
    EXPECT_NE(computePartId(a), computePartId(b));

    // Lowercase hex of a 128-bit value.
    const std::string id = computePartId(a).string();
    EXPECT_EQ(id.size(), 32u);
    EXPECT_EQ(id.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST_F(ContentAddressedMetaTest, WritePartThenReadBackAndDedup)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_wtxn");
    auto os = getObjectStorage("cas_wtxn");
    const std::string uuid = "uuid-9";

    auto write_part = [&](const std::string & part, const std::map<std::string,std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    write_part("all_1_1_0", {{"a.bin","AAA"}, {"b.bin","SHARED"}, {"columns.txt","a b"}});
    write_part("all_2_2_0", {{"a.bin","ZZZ"}, {"b.bin","SHARED"}, {"columns.txt","a b"}});

    // read back via the Phase-2 resolution path
    auto objs = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin");
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(readObject(os, objs[0].remote_path), "AAA");
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/b.bin")[0].remote_path), "SHARED");

    // dedup: the shared "SHARED" column is one blob object
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path,
              ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/b.bin")[0].remote_path);
}

// B40 (data-integrity, end-to-end): a part written and committed under a NON-Atomic database path
// (data/<db>/<table>/<part>/<file>), including the MergeTree tmp_insert_<part> -> final <part> rename,
// must be readable from the final part dir. Before the part-dir-grammar fallback the part files were
// written verbatim under tmp_insert_<part>/, moveDirectory moved nothing (it only handled the uuid
// layout), and the read of the final part found no object — the read-miss this test guards against.
TEST_F(ContentAddressedMetaTest, WriteNonAtomicPartViaTmpRenameThenReadBack)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_nonatomic");
    auto os = getObjectStorage("cas_nonatomic");

    // The non-Atomic relative_data_path of a Memory/Ordinary-DB table: data/<db>/<table>.
    const std::string table = "data/memory_db/mt";
    const std::string tmp_part = "tmp_insert_all_1_1_0";
    const std::string final_part = "all_1_1_0";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        // MergeTree assembles the part under a tmp dir, including a mutable per-part file.
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "PAYLOAD"}, {"primary.idx", "IDX"}, {"metadata_version.txt", "0"}})
        {
            auto buf = tx.writeFile(table + "/" + tmp_part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        // ... then renames the tmp dir to the final part name. The ref must publish under final_part.
        tx.moveDirectory(table + "/" + tmp_part, table + "/" + final_part);
        tx.commit(DB::NoCommitOptions{});
    }

    // The final part dir resolves and reads back the exact bytes (content + mutable file).
    const std::string base = table + "/" + final_part + "/";
    EXPECT_TRUE(ms->existsFile(base + "data.bin"));
    EXPECT_TRUE(ms->existsFile(base + "primary.idx"));
    EXPECT_TRUE(ms->existsFile(base + "metadata_version.txt"));
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "data.bin")[0].remote_path), "PAYLOAD");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "primary.idx")[0].remote_path), "IDX");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "metadata_version.txt")[0].remote_path), "0");

    // The tmp part dir is gone (the ref was re-pinned to the final name, not left dangling).
    EXPECT_FALSE(ms->existsDirectory(table + "/" + tmp_part));
    EXPECT_TRUE(ms->existsDirectory(table + "/" + final_part));

    // The table dir lists exactly the final part, and DROP (removeRecursive on the table dir) unlinks it.
    auto parts = ms->listDirectory(table);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], final_part);
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive(table, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(ms->existsDirectory(table));
}

// B42 (part-lifecycle idempotency): the "part to remove doesn't exist" failure on a non-Atomic-DB
// (Memory-engine) MergeTree table was a downstream symptom of B40 — the part's ref resolved under the
// wrong (verbatim) path, so the engine's outdated-part cleanup could not find it. With B40 fixed the
// part resolves; this test pins that the CA part-removal path is itself idempotent (a double-remove,
// or a remove after the ref is already unlinked, must NOT throw — it is a no-op), so a re-attempted
// outdated-part removal in the part lifecycle never escalates to an error.
TEST_F(ContentAddressedMetaTest, PartRemovalIsIdempotent)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_idempotent");

    const std::string table = "data/db_memory/mt"; // non-Atomic (Memory-DB) layout, as in 01625
    const std::string part = "all_1_1_0";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(table + "/" + part + "/data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "ROWS";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(ms->existsDirectory(table + "/" + part));

    auto remove_part = [&]
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive(table + "/" + part, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    };

    EXPECT_NO_THROW(remove_part());                       // first removal unlinks the ref
    EXPECT_FALSE(ms->existsDirectory(table + "/" + part));
    EXPECT_NO_THROW(remove_part());                       // double-remove is a no-op, must not throw

    // Removing a part that never existed is likewise a no-op.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        EXPECT_NO_THROW(tx.removeRecursive(table + "/all_9_9_0", /*should_remove_objects=*/nullptr));
        tx.commit(DB::NoCommitOptions{});
    }
}

// B41 (concurrent-blob race): two writers racing the SAME content hash must BOTH succeed — neither
// may observe a partially-written (size-0 / short) blob at the final key and fail closed with
// CORRUPTED_DATA. LocalObjectStorage writes objects in place, so before the temp+atomic-rename fix a
// second writer could see the first's half-written object. Drive many concurrent ContentAddressedWrite
// Buffers writing identical bytes to the same blob key and assert all finalize cleanly and the blob is
// correct. This mirrors a parallel INSERT that produces identical content (00276_sample,
// 01825_json_type_parallel_insert).
TEST_F(ContentAddressedMetaTest, ConcurrentIdenticalBlobWritesAreAtomic)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_concurrent");
    auto os = getObjectStorage("cas_concurrent");

    // Large enough that the in-place copy is not instantaneous, so a racing reader has a real window.
    const std::string payload(2 * 1024 * 1024, 'x');
    constexpr size_t kThreads = 16;

    std::atomic<size_t> failures = 0;
    std::mutex hash_mutex;
    std::string observed_hash;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (size_t i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&]
        {
            try
            {
                ContentAddressedWriteBuffer buf(os, /*key_prefix=*/"", kCasTestScratch);
                buf.write(payload.data(), payload.size());
                buf.finalize();
                std::lock_guard lock(hash_mutex);
                observed_hash = buf.getBlobHash();
            }
            catch (...)
            {
                failures.fetch_add(1);
            }
        });
    }
    for (auto & t : threads)
        t.join();

    EXPECT_EQ(failures.load(), 0u);
    ASSERT_FALSE(observed_hash.empty());

    // All writers deduplicate to ONE blob key that holds exactly the written content.
    const std::string blob_key = blobKey("", BlobHash(observed_hash)).string();
    auto meta = os->tryGetObjectMetadata(blob_key, /*with_tags=*/false);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->size_bytes, payload.size());
    EXPECT_EQ(readObject(os, blob_key), payload);

    // No temporary publish objects leaked next to the final blob.
    DB::RelativePathsWithMetadata listed;
    os->listObjects(blobsPrefix(""), listed, /*max_keys=*/0);
    for (const auto & e : listed)
        EXPECT_EQ(e->relative_path.find(".tmp."), std::string::npos) << e->relative_path;
}

// B40 (table rename): RENAME TABLE / a cross-engine table move renames the whole table data dir via
// disk->moveDirectory(old_table_path, new_table_path). The refs are keyed by the table identifier, so
// the read at the NEW identity would find no ref unless moveDirectory re-keys every ref/sidecar from
// the source table id to the destination table id. This covers the Ordinary(data/db/mt)->Atomic(uuid)
// move shape that 01114_database_atomic exercises.
TEST_F(ContentAddressedMetaTest, RenameTableDirRekeysRefs)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_renametable");
    auto os = getObjectStorage("cas_renametable");

    const std::string src_table = "data/db3/mt";              // Ordinary-DB layout
    const std::string dst_table = "sto/uuid-renamed";         // Atomic-DB layout (uuid anchor)
    const std::string part = "0_1_1_0";

    // Write a committed part under the Ordinary-DB table path.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "ROWS"}, {"metadata_version.txt", "0"}})
        {
            auto buf = tx.writeFile(src_table + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(ms->existsFile(src_table + "/" + part + "/data.bin"));

    // Rename the whole table dir (Ordinary -> Atomic).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory(src_table, dst_table);
        tx.commit(DB::NoCommitOptions{});
    }

    // The part reads back at the NEW table identity, and the source identity is gone.
    EXPECT_TRUE(ms->existsFile(dst_table + "/" + part + "/data.bin"));
    EXPECT_EQ(readObject(os, ms->getStorageObjects(dst_table + "/" + part + "/data.bin")[0].remote_path), "ROWS");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(dst_table + "/" + part + "/metadata_version.txt")[0].remote_path), "0");
    EXPECT_FALSE(ms->existsFile(src_table + "/" + part + "/data.bin"));
    EXPECT_FALSE(ms->existsDirectory(src_table));

    auto parts = ms->listDirectory(dst_table);
    ASSERT_EQ(parts.size(), 1u);
    EXPECT_EQ(parts[0], part);
}

// B36: moveDirectory of a COMMITTED part into the detached namespace must re-publish a "detached"
// ref whose manifest carries the source part's files re-keyed under the detached part dir name, so
// listDirectory(detached) yields the detached part DIRECTORY name (not the inner files) and the
// source ref is unlinked. This covers the rename-based detach path (renameToDetached on broken parts).
TEST_F(ContentAddressedMetaTest, MoveCommittedPartIntoDetachedRekeysRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_movedetach");
    auto os = getObjectStorage("cas_movedetach");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-movedet";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "DATA"}, {"columns.txt", "a"}, {"metadata_version.txt", "7"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/all_1_2_1/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // Move the committed part dir into detached/<part>.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/all_1_2_1", "uui/" + uuid + "/detached/all_1_2_1");
        tx.commit(DB::NoCommitOptions{});
    }

    // The active part dir no longer has the ref; the detached dir lists the part DIRECTORY name.
    EXPECT_FALSE(ms->existsDirectory("uui/" + uuid + "/all_1_2_1"));
    auto detached = ms->listDirectory("uui/" + uuid + "/detached");
    std::set<std::string> got(detached.begin(), detached.end());
    EXPECT_EQ(got, (std::set<std::string>{"all_1_2_1"}));

    // The detached part's content is resolvable under detached/<part>/<file>.
    auto objs = ms->getStorageObjects("uui/" + uuid + "/detached/all_1_2_1/data.bin");
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(readObject(os, objs[0].remote_path), "DATA");
}

// B23 Task 2 (collision regression): two parts with IDENTICAL column content but DIFFERENT mutable
// per-part files (uuid.txt / txn_version.txt). They MUST still dedup to the same part_id / manifest,
// but each MUST keep its own per-ref .meta sidecar, so resolving a mutable file returns that part's
// OWN bytes. Before the split both parts shared the one manifest (put-if-absent kept the first
// writer's copy), so the second part read the FIRST part's txn_version — silent per-part corruption.
TEST_F(ContentAddressedMetaTest, IdenticalContentDifferentMutableFilesDoNotCollide)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_collide");
    auto os = getObjectStorage("cas_collide");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-collide";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    // Identical column data ("AAA"), identical columns.txt — but DIFFERENT mutable files.
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"columns.txt", "a"}, {"uuid.txt", "UUID-ONE"}, {"txn_version.txt", "11"}});
    write_part("all_2_2_0", {{"a.bin", "AAA"}, {"columns.txt", "a"}, {"uuid.txt", "UUID-TWO"}, {"txn_version.txt", "22"}});

    // (a) Dedup preserved: both refs resolve to the SAME part_id / manifest object.
    auto pid1 = partIdFromRefPayload(readObject(os, refKey("", sid, uuid, "all_1_1_0").string()));
    auto pid2 = partIdFromRefPayload(readObject(os, refKey("", sid, uuid, "all_2_2_0").string()));
    EXPECT_EQ(pid1, pid2);

    // (b) Each part has its OWN per-ref sidecar object holding that part's DISTINCT mutable bytes.
    const std::string meta1 = refMetaKey("", sid, uuid, "all_1_1_0").string();
    const std::string meta2 = refMetaKey("", sid, uuid, "all_2_2_0").string();
    EXPECT_NE(meta1, meta2);
    ASSERT_TRUE(os->tryGetObjectMetadata(meta1, /*with_tags=*/false).has_value());
    ASSERT_TRUE(os->tryGetObjectMetadata(meta2, /*with_tags=*/false).has_value());

    // (c) The sidecars carry each part's OWN bytes (no collision). This is the regression the split
    // fixes: before it, both parts shared the one manifest and the second read the first's bytes.
    auto s1 = RefSidecar::deserialize(readObject(os, meta1));
    auto s2 = RefSidecar::deserialize(readObject(os, meta2));
    EXPECT_EQ(s1.files.at("txn_version.txt"), "11");
    EXPECT_EQ(s2.files.at("txn_version.txt"), "22");
    EXPECT_EQ(s1.files.at("uuid.txt"), "UUID-ONE");
    EXPECT_EQ(s2.files.at("uuid.txt"), "UUID-TWO");

    // (d) The shared manifest holds ONLY content-identical files — the mutable files are NOT in it.
    auto manifest = PartManifest::deserialize(readObject(os, partKey("", pid1).string()));
    EXPECT_TRUE(manifest.blobs.contains("a.bin"));
    EXPECT_TRUE(manifest.blobs.contains("columns.txt"));
    EXPECT_FALSE(manifest.blobs.contains("txn_version.txt"));
    EXPECT_FALSE(manifest.blobs.contains("uuid.txt"));

    // The content file still resolves via the shared manifest -> blob (one deduped blob for both).
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path,
              ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path);
}

// B23 Task 3: the read path overlays the per-ref sidecar for mutable per-part files. existsFile /
// getFileSize / getStorageObjects / listDirectory resolve a mutable file from the sidecar (correct
// per-part bytes), a missing sidecar entry behaves like a missing file (fail-close), and content
// files still resolve via manifest -> blob.
TEST_F(ContentAddressedMetaTest, ReadPathOverlaysMutableFilesFromSidecar)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_overlay");
    auto os = getObjectStorage("cas_overlay");
    const std::string uuid = "uuid-overlay";
    const std::string part = "all_1_1_0";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"a.bin", "COLDATA"}, {"columns.txt", "a"}, {"uuid.txt", "THE-UUID"}, {"txn_version.txt", "123"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    const std::string txn_path = "uui/" + uuid + "/" + part + "/txn_version.txt";
    const std::string uuid_path = "uui/" + uuid + "/" + part + "/uuid.txt";

    // existsFile / getFileSize resolve the mutable file from the sidecar.
    EXPECT_TRUE(ms->existsFile(txn_path));
    EXPECT_TRUE(ms->existsFile(uuid_path));
    EXPECT_EQ(ms->getFileSize(txn_path), 3u);   // "123"
    EXPECT_EQ(ms->getFileSize(uuid_path), 8u);  // "THE-UUID"

    // getStorageObjects returns an object whose bytes are EXACTLY that mutable file's content.
    auto txn_objs = ms->getStorageObjects(txn_path);
    ASSERT_EQ(txn_objs.size(), 1u);
    EXPECT_EQ(readObject(os, txn_objs[0].remote_path), "123");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(uuid_path)[0].remote_path), "THE-UUID");

    // The content file still resolves via the manifest -> blob path (a blobs/ key).
    auto a_objs = ms->getStorageObjects("uui/" + uuid + "/" + part + "/a.bin");
    ASSERT_EQ(a_objs.size(), 1u);
    EXPECT_EQ(a_objs[0].remote_path.rfind("blobs/", 0), 0u);
    EXPECT_EQ(readObject(os, a_objs[0].remote_path), "COLDATA");

    // listDirectory on the part dir includes BOTH content files and mutable files (overlaid).
    auto files = ms->listDirectory("uui/" + uuid + "/" + part);
    std::set<std::string> got(files.begin(), files.end());
    EXPECT_EQ(got, (std::set<std::string>{"a.bin", "columns.txt", "uuid.txt", "txn_version.txt"}));

    // Fail-close: a mutable file the part never wrote (no sidecar entry) behaves like a missing file.
    const std::string absent = "uui/" + uuid + "/" + part + "/metadata_version.txt";
    EXPECT_FALSE(ms->existsFile(absent));
    EXPECT_THROW(ms->getStorageObjects(absent), DB::Exception);
    EXPECT_THROW(ms->getFileSize(absent), DB::Exception);
}

// P3.5: removeRecursive = pointer-unlink + deferred GC. Dropping a part / table deletes only the
// ref pointer objects (and verbatim table-level files); the shared parts/ manifest and blobs/ content
// are intentionally KEPT — they are reclaimed later by the Phase-4 reachability GC. This proves the
// unlink-not-GC behaviour that lets DROP TABLE complete without leaking through a hung retry loop.
TEST_F(ContentAddressedMetaTest, RemoveUnlinksRefsKeepsBlobs)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_remove");
    auto os = getObjectStorage("cas_remove");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-remove";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    // Two parts sharing one column ("SHARED") so we can confirm a single-part removal leaves the
    // other part — and the shared blob — fully intact.
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    write_part("all_2_2_0", {{"a.bin", "ZZZ"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});

    // Capture the backing object keys (manifest + blobs) we expect to SURVIVE removal.
    const std::string blob_a1 = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path;
    const std::string blob_shared = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path;
    const std::string ref_1 = refKey("", sid, uuid, "all_1_1_0").string();
    const std::string ref_2 = refKey("", sid, uuid, "all_2_2_0").string();
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_1, /*with_tags=*/false).has_value());
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_2, /*with_tags=*/false).has_value());
    ASSERT_TRUE(os->tryGetObjectMetadata(blob_a1, /*with_tags=*/false).has_value());
    ASSERT_TRUE(os->tryGetObjectMetadata(blob_shared, /*with_tags=*/false).has_value());

    // (1) Removing a single part directory deletes ONLY that part's ref.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/all_1_1_0", /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_1, /*with_tags=*/false).has_value()); // gone
    EXPECT_TRUE(os->tryGetObjectMetadata(ref_2, /*with_tags=*/false).has_value());  // other part untouched
    // The manifest + blobs of the removed part survive (deferred GC); the surviving part still reads.
    EXPECT_TRUE(os->tryGetObjectMetadata(blob_a1, /*with_tags=*/false).has_value());
    EXPECT_TRUE(os->tryGetObjectMetadata(blob_shared, /*with_tags=*/false).has_value());
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), "ZZZ");
    EXPECT_EQ(readObject(os, blob_shared), "SHARED");

    // (2) Removing the whole table directory deletes ALL remaining refs but KEEPS the blobs.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_2, /*with_tags=*/false).has_value()); // last ref gone
    EXPECT_FALSE(ms->existsDirectory("uui/" + uuid + "/"));                          // table dir empty
    // Unlink-not-GC: the manifest object and both blob objects are STILL present after DROP.
    EXPECT_TRUE(os->tryGetObjectMetadata(blob_a1, /*with_tags=*/false).has_value());
    EXPECT_TRUE(os->tryGetObjectMetadata(blob_shared, /*with_tags=*/false).has_value());
    EXPECT_EQ(readObject(os, blob_shared), "SHARED"); // blob content intact, reclaimable by GC
}

// B45: the MergeTree FAST removal path (DataPartStorageOnDiskBase::clearDirectory, used for a
// complete part) unlinks the part's files one by one and then calls disk->removeDirectory(<part>) —
// it never calls removeRecursive on the part directory. For a content-addressed disk the per-file
// unlinks are no-ops on a committed ref, so removeDirectory(<part>) is the ONLY point at which the
// ref can be unlinked. If it stays a no-op the ref lingers and the part is rediscovered by the
// re-attach part-load scan (which enumerates strictly from refs). This pins that removeDirectory on a
// complete part directory unlinks the ref so a re-list of the table dir no longer returns it.
TEST_F(ContentAddressedMetaTest, RemoveDirectoryUnlinksPartRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_rmdir");
    auto os = getObjectStorage("cas_rmdir");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-rmdir";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    // Two complete parts. One shared column blob proves a single-part removal leaves the other intact.
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    write_part("all_2_2_0", {{"a.bin", "ZZZ"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});

    const std::string blob_shared = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path;
    const std::string ref_1 = refKey("", sid, uuid, "all_1_1_0").string();

    // Before removal the table dir lists both parts.
    {
        auto parts = ms->listDirectory("uui/" + uuid + "/");
        std::set<std::string> got(parts.begin(), parts.end());
        EXPECT_EQ(got, (std::set<std::string>{"all_1_1_0", "all_2_2_0"}));
    }

    // Remove one part via removeDirectory (the fast-removal entry point) — NOT removeRecursive.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeDirectory("uui/" + uuid + "/all_1_1_0");
        tx.commit(DB::NoCommitOptions{});
    }

    // The ref is gone, so a re-list (the re-attach part-load scan) NO LONGER returns the removed part.
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_1, /*with_tags=*/false).has_value());
    {
        auto parts = ms->listDirectory("uui/" + uuid + "/");
        std::set<std::string> got(parts.begin(), parts.end());
        EXPECT_EQ(got, (std::set<std::string>{"all_2_2_0"}));
    }
    EXPECT_FALSE(ms->existsDirectory("uui/" + uuid + "/all_1_1_0/"));

    // The surviving part still resolves and the shared blob is intact (deferred GC keeps blobs).
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), "ZZZ");
    EXPECT_EQ(readObject(os, blob_shared), "SHARED");

    // removeDirectory on the TABLE dir is still a no-op (table drop goes through removeRecursive), so
    // it must not unlink the surviving ref.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeDirectory("uui/" + uuid);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(os->tryGetObjectMetadata(refKey("", sid, uuid, "all_2_2_0").string(), /*with_tags=*/false).has_value());
}

// B45: MergeTree renames a COMMITTED part to delete_tmp_<part> (via disk->moveDirectory, with nothing
// staged in the transaction) BEFORE removing it. Content addressing has no rename, so moveDirectory
// must re-key the committed source ref to the destination; the subsequent removeDirectory then unlinks
// it. If moveDirectory only adopts the destination as a staged target (the INSERT tmp->final case) the
// committed source ref survives the whole remove and the part is rediscovered on the next ATTACH. This
// pins the full rename->remove sequence: after it the source part is no longer listed.
TEST_F(ContentAddressedMetaTest, RenameCommittedPartThenRemoveUnlinksRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_renamerm");
    auto os = getObjectStorage("cas_renamerm");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-renamerm";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    // Two committed parts; all_1_1_0 also carries a MUTABLE per-part file (metadata_version.txt) so the
    // rename must move the per-ref sidecar objects too, not just the bare ref.
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"columns.txt", "a"}, {"metadata_version.txt", "7"}});
    write_part("all_2_2_0", {{"a.bin", "ZZZ"}, {"columns.txt", "a"}});

    const std::string ref_src = refKey("", sid, uuid, "all_1_1_0").string();
    const std::string ref_dst = refKey("", sid, uuid, "delete_tmp_all_1_1_0").string();

    // (1) Rename the committed part to delete_tmp_<part> with NOTHING staged (a fresh transaction),
    // exactly as DataPartStorageOnDiskBase::remove does before deleting.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/all_1_1_0", "uui/" + uuid + "/delete_tmp_all_1_1_0");
        tx.commit(DB::NoCommitOptions{});
    }
    // The ref moved: source gone, destination present, and the renamed part resolves + reads.
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_src, /*with_tags=*/false).has_value());
    EXPECT_TRUE(os->tryGetObjectMetadata(ref_dst, /*with_tags=*/false).has_value());
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/delete_tmp_all_1_1_0/a.bin")[0].remote_path), "AAA");
    // The mutable sidecar file followed the rename (resolves from the destination ref's sidecar).
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/delete_tmp_all_1_1_0/metadata_version.txt")[0].remote_path), "7");
    // The table dir lists the renamed name, not the original.
    {
        auto parts = ms->listDirectory("uui/" + uuid + "/");
        std::set<std::string> got(parts.begin(), parts.end());
        EXPECT_EQ(got, (std::set<std::string>{"delete_tmp_all_1_1_0", "all_2_2_0"}));
    }

    // (2) Remove the renamed directory (the fast-removal entry point). Now nothing remains of the part.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeDirectory("uui/" + uuid + "/delete_tmp_all_1_1_0");
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_dst, /*with_tags=*/false).has_value());
    {
        auto parts = ms->listDirectory("uui/" + uuid + "/");
        std::set<std::string> got(parts.begin(), parts.end());
        EXPECT_EQ(got, (std::set<std::string>{"all_2_2_0"})); // the dropped part is NOT rediscovered
    }
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), "ZZZ");
}

// P3.5: non-part / table-level files (e.g. format_version.txt) are written verbatim to a direct
// object key (no content addressing, no ref, no manifest) and resolve straight back. A part file in
// the same table still resolves via the ref/manifest/blob path, so the two schemes coexist.
TEST_F(ContentAddressedMetaTest, NonPartFilePassthrough)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_nonpart");
    auto os = getObjectStorage("cas_nonpart");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-nonpart";
    const std::string format_version = "uui/" + uuid + "/format_version.txt";

    // Write a table-level file through the transaction.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(format_version, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "1";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{}); // no part recorded → publishes nothing
    }

    // It resolves directly (no ref/manifest) and reads back byte-for-byte.
    EXPECT_TRUE(ms->existsFile(format_version));
    EXPECT_EQ(ms->getFileSize(format_version), 1u);
    auto objs = ms->getStorageObjects(format_version);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, tableFileKey("", sid, uuid, "format_version.txt"));
    EXPECT_EQ(readObject(os, objs[0].remote_path), "1");

    // No content-addressed structures were created for it.
    EXPECT_FALSE(ms->existsFile("uui/" + uuid + "/format_version.txt/anything"));

    // A part file in the same table still resolves via the ref/manifest/blob path.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "PART-BYTES";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    auto part_objs = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/data.bin");
    ASSERT_EQ(part_objs.size(), 1u);
    EXPECT_EQ(part_objs[0].remote_path.rfind("blobs/", 0), 0u); // content-addressed blob key
    EXPECT_EQ(readObject(os, part_objs[0].remote_path), "PART-BYTES");

    // The table-level file is unaffected by the part commit.
    EXPECT_EQ(readObject(os, ms->getStorageObjects(format_version)[0].remote_path), "1");
}

// P3.5 (B27): a generic disk-level file — one that is neither a part file nor a table-level file,
// e.g. the server's startup access-check probe clickhouse_access_check_<uuid> written at the disk
// root — is written verbatim to its diskFileKey, resolves straight back byte-for-byte, and is
// removed by unlinkFile so the access-check write -> read -> unlink round-trip leaves nothing behind.
TEST_F(ContentAddressedMetaTest, GenericDiskFilePassthroughRoundTrips)
{
    using namespace DB::ContentAddressed;
    // A real disk always has a non-empty object-storage common key prefix, so the verbatim key for
    // a disk-root file (e.g. clickhouse_access_check_<uuid>) has a parent directory. Mirror that by
    // pairing a metadata storage and its transactions on the SAME non-empty prefix (read and write
    // must agree on it). With an empty prefix the key for a disk-root file would be a bare filename
    // whose (empty) parent the local-fs harness cannot create — not a production shape.
    const std::string prefix = "cas_generic_pool";
    auto os = getObjectStorage("cas_generic");
    DB::ContentAddressedMetadataStorage ms(os, prefix, "test-server", kCasTestScratch);

    const std::string probe = "clickhouse_access_check_0123abcd";
    const std::string bytes = "0123abcd"; // the access-check writes a uuid string and reads it back

    // It is classified as neither a part file nor a table-level file (it has no <uuid[:3]>/<uuid>
    // anchor), so it must take the generic verbatim passthrough.
    ASSERT_FALSE(isPartFilePath(probe));
    ASSERT_FALSE(parseTableFilePath(probe).has_value());

    // Write through the transaction (commit publishes nothing — no part recorded).
    {
        DB::ContentAddressedTransaction tx(ms, prefix, kCasTestScratch);
        auto buf = tx.writeFile(probe, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }

    // The verbatim object lands at <prefix>/<path> and resolves straight back byte-for-byte.
    EXPECT_EQ(diskFileKey(prefix, probe), prefix + "/" + probe);
    EXPECT_TRUE(ms.existsFile(probe));
    EXPECT_EQ(ms.getFileSize(probe), bytes.size());
    auto objs = ms.getStorageObjects(probe);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, diskFileKey(prefix, probe));
    EXPECT_EQ(readObject(os, objs[0].remote_path), bytes);

    // unlinkFile removes the verbatim object — afterwards it is gone.
    {
        DB::ContentAddressedTransaction tx(ms, prefix, kCasTestScratch);
        tx.unlinkFile(probe, /*if_exists=*/false, /*should_remove_objects=*/true);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(ms.existsFile(probe));

    std::error_code ec;
    fs::remove_all(prefix, ec); // CWD-relative pool dir created by the verbatim write
}

TEST_F(ContentAddressedMetaTest, MutationCarryForwardReusesBlob)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_cf");
    auto os = getObjectStorage("cas_cf");
    const std::string uuid = "uuid-10";
    // source part
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (auto & [n,b] : std::map<std::string,std::string>{{"a.bin","A0"},{"b.bin","B0"},{"columns.txt","a b"}})
        { auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/" + n, 4096, DB::WriteMode::Rewrite, {}); buf->write(b.data(), b.size()); buf->finalize(); }
        tx.commit(DB::NoCommitOptions{});
    }
    // mutation: rewrite a.bin, carry-forward b.bin + columns.txt
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0_1/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write("A1", 2); buf->finalize();
        tx.createHardLinkFrom("uui/" + uuid + "/all_1_1_0/b.bin", "uui/" + uuid + "/all_1_1_0_1/b.bin");
        tx.createHardLinkFrom("uui/" + uuid + "/all_1_1_0/columns.txt", "uui/" + uuid + "/all_1_1_0_1/columns.txt");
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0_1/a.bin")[0].remote_path), "A1");
    // b.bin carried forward → same blob object as the source
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/all_1_1_0_1/b.bin")[0].remote_path,
              ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path);
}

// Task 3: the disk-transaction write path must route through the content-addressed buffer when
// the metadata storage is content-addressed. This drives the real DiskObjectStorageTransaction
// (the same object MergedBlockOutputStream writes through), so the gated hook is exercised
// end-to-end: writeFile -> ContentAddressedWriteBuffer, commit -> ContentAddressedTransaction::commit
// (manifest + ref), and read-back via the Phase-2 resolution path.
TEST_F(ContentAddressedMetaTest, DiskTransactionRoutesContentAddressedWrite)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_disktxn");
    auto os = getObjectStorage("cas_disktxn");
    const std::string uuid = "uuid-disktxn";

    // A minimal single-location cluster: one local, enabled location. The content-addressed
    // branch fires before any cluster/router use, so the router contents are not consulted, but
    // the transaction constructor needs a valid local+enabled location to exist.
    const DB::Location local_location = "local";
    auto cluster = std::make_shared<DB::ClusterConfiguration>(
        "cas_disktxn_disk",
        std::unordered_map<DB::Location, DB::LocationInfo>{
            {local_location, DB::LocationInfo{/*enabled=*/true, /*local=*/true, /*config_prefix=*/""}}});
    auto router = std::make_shared<DB::ObjectStorageRouter>(
        std::unordered_map<DB::Location, DB::ObjectStoragePtr>{{local_location, os}});

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        auto disk_tx = std::make_shared<DB::DiskObjectStorageTransaction>(
            cluster, ms, router, /*blob_killer=*/nullptr, /*wait_blob_removal=*/false,
            /*read_resource_name=*/"", /*write_resource_name=*/"");
        for (const auto & [name, bytes] : files)
        {
            auto buf = disk_tx->writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        disk_tx->commit();
    };

    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    write_part("all_2_2_0", {{"a.bin", "ZZZ"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});

    // The manifest + ref published by commit make the part files resolvable, and the bytes read back.
    auto objs = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin");
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(readObject(os, objs[0].remote_path), "AAA");
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), "ZZZ");

    // Content addressing dedups the shared column to a single blob across both parts.
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path,
              ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/b.bin")[0].remote_path);
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path), "SHARED");
}

// C1: the production carry-forward path goes through the disk layer's createHardLink, which
// delegates to metadata_transaction->createHardLink. This exercises a mutation that rewrites
// one file and hardlinks an unchanged one through the same DiskObjectStorageTransaction the
// real write path uses (the direct createHardLinkFrom tests above masked the missing override).
TEST_F(ContentAddressedMetaTest, DiskTransactionCarryForwardThroughCreateHardLink)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_disktxn_cf");
    auto os = getObjectStorage("cas_disktxn_cf");
    const std::string uuid = "uuid-disktxn-cf";

    const DB::Location local_location = "local";
    auto cluster = std::make_shared<DB::ClusterConfiguration>(
        "cas_disktxn_cf_disk",
        std::unordered_map<DB::Location, DB::LocationInfo>{
            {local_location, DB::LocationInfo{/*enabled=*/true, /*local=*/true, /*config_prefix=*/""}}});
    auto router = std::make_shared<DB::ObjectStorageRouter>(
        std::unordered_map<DB::Location, DB::ObjectStoragePtr>{{local_location, os}});

    auto new_disk_tx = [&]
    {
        return std::make_shared<DB::DiskObjectStorageTransaction>(
            cluster, ms, router, /*blob_killer=*/nullptr, /*wait_blob_removal=*/false,
            /*read_resource_name=*/"", /*write_resource_name=*/"");
    };

    // Source part: a.bin = A0, b.bin = B0.
    {
        auto disk_tx = new_disk_tx();
        for (const auto & [name, bytes] : std::map<std::string, std::string>{{"a.bin", "A0"}, {"b.bin", "B0"}})
        {
            auto buf = disk_tx->writeFile("uui/" + uuid + "/all_1_1_0/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        disk_tx->commit();
    }

    // Mutation: rewrite a.bin -> A1, carry forward b.bin via the disk-layer createHardLink.
    {
        auto disk_tx = new_disk_tx();
        auto buf = disk_tx->writeFile("uui/" + uuid + "/all_1_1_0_1/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write("A1", 2);
        buf->finalize();
        disk_tx->createHardLink("uui/" + uuid + "/all_1_1_0/b.bin", "uui/" + uuid + "/all_1_1_0_1/b.bin");
        disk_tx->commit();
    }

    // Rewritten file holds the new content.
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0_1/a.bin")[0].remote_path), "A1");
    // Carried-forward b.bin points at the same blob as the source (no new upload).
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/all_1_1_0_1/b.bin")[0].remote_path,
              ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path);
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0_1/b.bin")[0].remote_path), "B0");
}

// Phase-4 GC scan + sweep. Seed a pool by hand (no transaction) so the live/orphan split is exact:
//   - 2 live refs -> 2 distinct part ids (pidA, pidB), 2 manifests, 3 referenced blobs (b1, b2 shared,
//     b3); pidA manifest = {b1, b2}, pidB manifest = {b2, b3} so b2 is dedup-shared across both parts.
//   - 1 orphan manifest (pidO, no ref) -> 1 orphan blob (bO, referenced only by the orphan manifest).
// parts/ then has 3 manifests, blobs/ has 4 blobs, and exactly 2 part ids are live.
namespace
{
struct CasGcSeed
{
    std::string uuid = "uuid-gc";
    std::string pid_a = "aaaa000000000000000000000000aaaa";
    std::string pid_b = "bbbb000000000000000000000000bbbb";
    std::string pid_orphan = "0000000000000000000000000000dead";
    std::string b1 = "1111111111", b2 = "2222222222", b3 = "3333333333", b_orphan = "9999999999";
};

CasGcSeed seedGcPool(const std::shared_ptr<DB::IObjectStorage> & os, const std::string & sid)
{
    using namespace DB::ContentAddressed;
    CasGcSeed s;
    auto put_blob = [&](const std::string & csum) { ContentAddressedMetaTest::writeObject(os, blobKey("", BlobHash(csum)).string(), csum); };
    put_blob(s.b1); put_blob(s.b2); put_blob(s.b3); put_blob(s.b_orphan);

    auto put_manifest = [&](const std::string & pid, const std::vector<std::pair<std::string, std::string>> & files)
    {
        PartManifest f;
        for (const auto & [name, csum] : files)
            /// Store the BARE content hash as the manifest key, exactly as the production write path
            /// records it (BlobEntry{blob_hash, size, blob_hash}); the read path and the GC project it
            /// to the full object key via blobKey. Seeding the full key here would mask the GC key-space
            /// bug (the reachable set must be comparable to the listed blobs/ object keys).
            f.blobs[name] = BlobEntry{BlobHash(csum), csum.size(), csum};
        ContentAddressedMetaTest::writeObject(os, partKey("", PartId(pid)).string(), f.serialize());
    };
    put_manifest(s.pid_a, {{"a.bin", s.b1}, {"shared.bin", s.b2}});
    put_manifest(s.pid_b, {{"shared.bin", s.b2}, {"c.bin", s.b3}});
    put_manifest(s.pid_orphan, {{"o.bin", s.b_orphan}});

    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_1_1_0").string(), serializeRefPayload(PartId(s.pid_a)));
    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_2_2_0").string(), serializeRefPayload(PartId(s.pid_b)));
    return s;
}
}

TEST_F(ContentAddressedMetaTest, PoolScanListsLivePartsAndObjects)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_scan");
    auto os = getObjectStorage("cas_scan");
    auto s = seedGcPool(os, ms->serverIdForTest());

    // Exactly the 2 referenced part ids are live; the orphan manifest's id is NOT (no ref points at it).
    EXPECT_EQ(listLivePartIds(os, ""), (std::set<PartId>{PartId(s.pid_a), PartId(s.pid_b)}));

    // parts/ holds all 3 manifests (2 live + 1 orphan); the keys match partKey.
    auto part_keys = listKeysUnder(os, partsPrefix(""));
    std::set<std::string> parts(part_keys.begin(), part_keys.end());
    EXPECT_EQ(parts, (std::set<std::string>{
        partKey("", PartId(s.pid_a)).string(), partKey("", PartId(s.pid_b)).string(), partKey("", PartId(s.pid_orphan)).string()}));

    // blobs/ holds all 4 blobs (3 referenced + 1 orphan); the keys match blobKey.
    auto blob_keys = listKeysUnder(os, blobsPrefix(""));
    std::set<std::string> blobs(blob_keys.begin(), blob_keys.end());
    EXPECT_EQ(blobs, (std::set<std::string>{
        blobKey("", BlobHash(s.b1)).string(), blobKey("", BlobHash(s.b2)).string(),
        blobKey("", BlobHash(s.b3)).string(), blobKey("", BlobHash(s.b_orphan)).string()}));
}

namespace
{
bool objectExists(const std::shared_ptr<DB::IObjectStorage> & os, const std::string & key)
{
    return os->tryGetObjectMetadata(key, /*with_tags=*/false).has_value();
}
}

// Grace-from-unreachability sweep: nothing is deleted before grace elapses, and the orphans' timers
// start at the first sweep that observes them unreferenced.
TEST_F(ContentAddressedMetaTest, SweepHonoursGraceWindow)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_grace");
    auto os = getObjectStorage("cas_gc_grace");
    auto s = seedGcPool(os, ms->serverIdForTest());

    DB::ContentAddressed::ContentAddressedGC gc(os, "");

    // (a) First sweep at t=0: the 2 orphans (manifest + blob) just became unreachable → delete nothing.
    auto r0 = gc.runSweepOnce(/*now=*/0, /*grace=*/100);
    EXPECT_EQ(r0.deleted_parts, 0u);
    EXPECT_EQ(r0.deleted_blobs, 0u);
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));

    // (b) Still within grace at t=50: nothing deleted.
    auto r1 = gc.runSweepOnce(/*now=*/50, /*grace=*/100);
    EXPECT_EQ(r1.deleted_parts, 0u);
    EXPECT_EQ(r1.deleted_blobs, 0u);
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));

    // (c) Past grace at t=200: exactly the orphan manifest + orphan blob are reclaimed; the 2 live
    //     manifests and 3 referenced blobs survive (reachable from the live refs).
    auto r2 = gc.runSweepOnce(/*now=*/200, /*grace=*/100);
    EXPECT_EQ(r2.deleted_parts, 1u);
    EXPECT_EQ(r2.deleted_blobs, 1u);
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_b)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b2)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b3)).string()));
}

// Dedup safety: a blob referenced by TWO live parts (b2/shared.bin) stays reachable after one of the
// two refs is unlinked, because the other live part's manifest still references it.
TEST_F(ContentAddressedMetaTest, SweepKeepsBlobStillReferencedByAnotherPart)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_dedup");
    auto os = getObjectStorage("cas_gc_dedup");
    auto s = seedGcPool(os, ms->serverIdForTest());

    DB::ContentAddressed::ContentAddressedGC gc(os, "");

    // Unlink part A's ref. pidA's manifest is now orphaned, but its blob b2 is also in pidB's manifest.
    os->removeObjectsIfExist({DB::StoredObject(refKey("", ms->serverIdForTest(), s.uuid, "all_1_1_0").string())});

    gc.runSweepOnce(/*now=*/0, /*grace=*/100);
    auto r = gc.runSweepOnce(/*now=*/200, /*grace=*/100);

    // pidA manifest + b1 (only in pidA) are reclaimed; the orphan manifest + orphan blob too.
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    // The shared blob b2 SURVIVES — still reachable through pidB's live ref.
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b2)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_b)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b3)).string()));
    EXPECT_EQ(r.deleted_parts, 2u); // pidA + orphan
    EXPECT_EQ(r.deleted_blobs, 2u); // b1 + orphan
}

// Reachable-again clears the timer: an object unreferenced at t0 then made reachable again before
// grace elapses must never be deleted (its first_unreachable entry is dropped on the reachable sweep).
TEST_F(ContentAddressedMetaTest, SweepClearsTimerWhenReachableAgain)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_again");
    auto os = getObjectStorage("cas_gc_again");
    const std::string sid = ms->serverIdForTest();
    auto s = seedGcPool(os, sid);

    DB::ContentAddressed::ContentAddressedGC gc(os, "");

    // Unlink pidA's ref so pidA's manifest + its exclusive blob b1 become unreferenced at t=0.
    os->removeObjectsIfExist({DB::StoredObject(refKey("", sid, s.uuid, "all_1_1_0").string())});
    gc.runSweepOnce(/*now=*/0, /*grace=*/100); // records first_unreachable for pidA manifest + b1

    // Re-add a ref pointing at pidA before grace elapses (e.g. an identical part re-inserted).
    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_1_1_0_redo").string(), serializeRefPayload(PartId(s.pid_a)));

    // Past the original grace: pidA is reachable again, so its timer was cleared and nothing of it
    // is deleted. Only the never-referenced orphan manifest + orphan blob are reclaimed.
    auto r = gc.runSweepOnce(/*now=*/200, /*grace=*/100);
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    EXPECT_EQ(r.deleted_parts, 1u);
    EXPECT_EQ(r.deleted_blobs, 1u);
}

// Fail-close (B18): a LIVE ref whose manifest is missing makes the sweep THROW and delete nothing.
TEST_F(ContentAddressedMetaTest, SweepThrowsAndDeletesNothingOnMissingManifest)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_failclose");
    auto os = getObjectStorage("cas_gc_failclose");
    auto s = seedGcPool(os, ms->serverIdForTest());

    // Corrupt the pool: a live ref points at a part id with no manifest object.
    ContentAddressedMetaTest::writeObject(os, refKey("", ms->serverIdForTest(), s.uuid, "all_3_3_0").string(), serializeRefPayload(PartId("deadc0de00000000000000000000beef")));

    DB::ContentAddressed::ContentAddressedGC gc(os, "");
    EXPECT_THROW(gc.runSweepOnce(/*now=*/200, /*grace=*/100), DB::Exception);

    // Nothing was deleted — even the otherwise-collectable orphans remain (the reachable set never
    // computed cleanly, so the sweep aborted before any removal).
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
}

// Background-thread driver (Task 3a): the ContentAddressedGCThread runs runSweepOnce on the schedule
// pool. With a tiny grace, one triggerAndWait() round must reclaim the orphan manifest + orphan blob
// while the live manifests and referenced blobs survive — proving the thread starts, runs the sweep,
// stops cleanly, and uses the round counter (not a sleep) for synchronisation.
TEST_F(ContentAddressedMetaTest, GCThreadSweepsOrphansAndKeepsLive)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_thread");
    auto os = getObjectStorage("cas_gc_thread");
    auto s = seedGcPool(os, ms->serverIdForTest());

    DB::ContentAddressedGCThread thread(
        "cas_gc_thread_disk",
        getContext().context,
        os,
        "",
        std::make_shared<std::mutex>(),
        std::make_shared<const std::set<std::string>>(),
        getLogger("ContentAddressedGCThreadTest"));

    /// grace 0 so a single round past first-unreachable reclaims orphans immediately.
    Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration());
    cfg->setInt("disk.content_addressed_gc_grace_sec", 0);
    cfg->setInt("disk.content_addressed_gc_interval_sec", 600);
    thread.applyNewSettings(*cfg, "disk");

    thread.startup();
    thread.triggerAndWait();

    /// The orphan manifest + orphan blob are gone; the 2 live manifests + 3 referenced blobs remain.
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_b)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b2)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b3)).string()));

    /// Clean shutdown must not hang or crash (deactivates the scheduled task).
    thread.shutdown();
}

// Regression for the GC blob key-space bug (P1 data loss). This test does NOT hand-seed the pool: it
// drives the REAL write path (ContentAddressedTransaction::writeFile + commit), so the production key
// convention is exercised — the manifest stores the BARE content hash, the blob object lives at the
// blobKey fan-out, and the read path projects the bare hash to the full key. Before Fix 1 the
// reachable set held the bare hashes while the sweep listed full object keys, so they never matched
// and the sweep deleted EVERY live blob; this test failed (LIVE blobs gone, getStorageObjects threw).
// After Fix 1 markReachableBlobs projects through blobKey, so the LIVE part survives and only the
// genuinely-orphaned DOOMED blobs are reclaimed.
TEST_F(ContentAddressedMetaTest, GCKeepsLiveBlobsWrittenThroughRealWritePath)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_realwrite");
    auto os = getObjectStorage("cas_gc_realwrite");
    const std::string uuid = "uuid-realwrite";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    // Two parts with NO shared blobs: every column has distinct content across the two parts.
    write_part("all_1_1_0", {{"a.bin", "LIVE-A"}, {"b.bin", "LIVE-B"}, {"columns.txt", "live-cols"}});
    write_part("all_2_2_0", {{"a.bin", "DOOMED-A"}, {"b.bin", "DOOMED-B"}, {"columns.txt", "doomed-cols"}});

    // Capture the LIVE part's backing blob object keys (full keys via getStorageObjects) — these must
    // survive — and the DOOMED part's blob keys — these must be reclaimed once its ref is unlinked.
    const std::string live_a = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path;
    const std::string live_b = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path;
    const std::string doomed_a = ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path;
    const std::string doomed_b = ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/b.bin")[0].remote_path;
    ASSERT_TRUE(objectExists(os, live_a));
    ASSERT_TRUE(objectExists(os, doomed_a));

    // Unlink the DOOMED part's ref (DROP-style); keep the LIVE ref.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/all_2_2_0", /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }

    // One sweep with grace=0: anything unreferenced this round is reclaimed immediately.
    DB::ContentAddressed::ContentAddressedGC gc(os, "");
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);

    // (a) The LIVE part's blobs survive AND read back the original bytes through the resolution path.
    EXPECT_TRUE(objectExists(os, live_a));
    EXPECT_TRUE(objectExists(os, live_b));
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path), "LIVE-A");
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/b.bin")[0].remote_path), "LIVE-B");

    // (b) The DOOMED part's now-orphan blobs were deleted.
    EXPECT_FALSE(objectExists(os, doomed_a));
    EXPECT_FALSE(objectExists(os, doomed_b));
}

// B49/B52 (CRITICAL, GC-on): the dedup / carry-forward old-blob race, now PREVENTED by the in-process
// blob PIN (B52). With background GC enabled, an in-flight identical-content INSERT that dedup-skips
// re-uploading an existing blob B (finalizeImpl) PINS B in the pool's in-flight set under the GC lock
// BEFORE making the skip decision. A concurrent sweep — sharing the SAME lock AND pin set — therefore
// observes the pin and treats B as reachable even though no committed ref names it yet, so it does NOT
// reclaim B. The insert then COMMITS SUCCESSFULLY (no fail-close, no dangling ref, no data loss). This
// deterministic variant runs the sweep BEFORE the commit (the reclaim would have won the race without
// the pin); with the pin B survives and T2.commit succeeds. The B49 commit re-validate stays as a
// never-expected safety net (see DedupCommitBeforeSweepKeepsReusedBlobAlive for the commit-wins order).
//
// Pre-pin, the old behavior of this test was: sweep deletes B, commit FAILS CLOSED with "concurrently
// reclaimed by GC". That fail-close fired on ~20 ordinary stateless tests under the shared-pool GC-on
// default (tiny deduped files becoming transiently unreferenced then reused), so it was a real
// availability bug; the pin converts those into SUCCESSFUL inserts.
TEST_F(ContentAddressedMetaTest, DedupCommitFailsClosedWhenBlobConcurrentlyReclaimed)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_b49_failclose");
    auto os = getObjectStorage("cas_b49_failclose");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-b49-fc";
    const std::string content = "B49-SHARED-CONTENT";

    // P1 is the SOLE referencer of blob B (a single-file part). Write + commit it through the real path.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write(content.data(), content.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string blob_b = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path;
    ASSERT_TRUE(objectExists(os, blob_b));

    // Drop P1's ref (T0): B is now unreferenced, but still PRESENT (GC has not run yet).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/all_1_1_0", /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(objectExists(os, blob_b));

    // T2: a dedup INSERT of IDENTICAL content. finalizeImpl finds B already present and SKIPS the
    // upload (B stays present), but PINS B in the pool's in-flight set under the GC lock (B52). The
    // transaction is NOT yet committed.
    DB::ContentAddressedTransaction t2(*ms, /*key_prefix=*/"", kCasTestScratch);
    {
        auto buf = t2.writeFile("uui/" + uuid + "/all_2_2_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write(content.data(), content.size());
        buf->finalize();
    }
    EXPECT_TRUE(objectExists(os, blob_b)); // dedup-skip: B was NOT re-uploaded, still present
    EXPECT_TRUE(ms->inFlightPinnedBlobs()->contains(blob_b)); // pinned by the in-flight insert

    // The sweep runs while T2 is in-flight, sharing the SAME per-pool GC lock AND the SAME pin set as
    // the storage (production wiring). With grace=0 B would otherwise be reclaimed (T2's ref is not yet
    // published) — but the pin makes B reachable, so the sweep MUST NOT delete it.
    DB::ContentAddressed::ContentAddressedGC gc(os, "", ms->gcLock(), ms->inFlightPinnedBlobs());
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);
    EXPECT_TRUE(objectExists(os, blob_b)); // pinned -> kept (NOT reclaimed)

    // T2.commit must SUCCEED: B is still present, so the re-validate passes (the fail-close is the
    // never-expected safety net). The pin is released once the ref is published.
    EXPECT_NO_THROW(t2.commit(DB::NoCommitOptions{}));

    // The ref WAS published for P2, B survives a post-commit sweep (reachable via the ref), and the
    // part reads back the original bytes — no data loss, no fail-close.
    EXPECT_TRUE(objectExists(os, refKey("", sid, uuid, "all_2_2_0").string()));
    EXPECT_FALSE(ms->inFlightPinnedBlobs()->contains(blob_b)); // released on commit
    gc.runSweepOnce(/*now=*/200, /*grace=*/0);
    EXPECT_TRUE(objectExists(os, blob_b));
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), content);
}

// B49 companion: the SAME setup, but the commit wins the race — the dedup INSERT publishes its ref
// BEFORE the sweep runs. The legitimately-reused blob B must SURVIVE (it is reachable again through
// the new ref), and the part reads back the original bytes. This proves the lock + ordering does not
// over-reclaim a blob that a concurrent insert is legitimately reusing.
TEST_F(ContentAddressedMetaTest, DedupCommitBeforeSweepKeepsReusedBlobAlive)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_b49_survive");
    auto os = getObjectStorage("cas_b49_survive");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-b49-sv";
    const std::string content = "B49-REUSED-CONTENT";

    // P1 is the sole referencer of blob B.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write(content.data(), content.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string blob_b = ms->getStorageObjects("uui/" + uuid + "/all_1_1_0/a.bin")[0].remote_path;
    ASSERT_TRUE(objectExists(os, blob_b));

    // Drop P1's ref: B is unreferenced but still present.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/all_1_1_0", /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(objectExists(os, blob_b));

    // T2: dedup INSERT of identical content (finalizeImpl skips the upload) AND commits — the commit
    // wins the race, publishing the ref while B is still present (re-validate passes).
    {
        DB::ContentAddressedTransaction t2(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = t2.writeFile("uui/" + uuid + "/all_2_2_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write(content.data(), content.size());
        buf->finalize();
        EXPECT_TRUE(objectExists(os, blob_b)); // dedup-skip
        EXPECT_NO_THROW(t2.commit(DB::NoCommitOptions{}));
    }
    EXPECT_TRUE(objectExists(os, refKey("", sid, uuid, "all_2_2_0").string()));

    // Now the sweep runs (sharing the same lock). B is reachable again through P2's ref, so it SURVIVES.
    DB::ContentAddressed::ContentAddressedGC gc(os, "", ms->gcLock());
    gc.runSweepOnce(/*now=*/100, /*grace=*/0);
    EXPECT_TRUE(objectExists(os, blob_b));

    // P2 reads back the original content through the resolution path.
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/a.bin")[0].remote_path), content);
}

// B52: a blob pinned by an in-flight transaction is KEPT by a sweep during the transaction, then once
// the transaction unpins it (here: an ABORTED insert destroyed without commit), a LATER sweep DOES
// reclaim it. This proves both halves of the pin lifecycle in one test: (a) the pin blocks the sweep
// while the transaction is live, and (b) unpin restores GC eligibility so true orphans are still
// collected. The companion DedupCommitFailsClosedWhenBlobConcurrentlyReclaimed proves the commit (not
// abort) unpin path: there the ref published at commit keeps the blob reachable after the pin drops.
TEST_F(ContentAddressedMetaTest, OrphanBlobReclaimedAfterTransactionUnpins)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_b52_abort");
    auto os = getObjectStorage("cas_b52_abort");
    const std::string uuid = "uuid-b52-abort";
    const std::string content = "B52-ABORTED-CONTENT";

    // The sweep shares the SAME per-pool GC lock AND in-flight pin set as the storage (production wiring).
    DB::ContentAddressed::ContentAddressedGC gc(os, "", ms->gcLock(), ms->inFlightPinnedBlobs());

    std::string blob_key;
    {
        // A fresh insert that uploads B then is ABANDONED (never committed) — e.g. a cancelled query.
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write(content.data(), content.size());
        buf->finalize();
        // The part's ref is NOT published (uncommitted), so resolve the blob key from the pin set the
        // write buffer just populated under the GC lock (exactly one staged blob in this transaction).
        ASSERT_EQ(ms->inFlightPinnedBlobs()->size(), 1u);
        blob_key = *ms->inFlightPinnedBlobs()->begin();
        EXPECT_TRUE(objectExists(os, blob_key)); // freshly uploaded

        // (a) A sweep DURING the live transaction must NOT reclaim B: it is pinned (no ref names it yet).
        gc.runSweepOnce(/*now=*/100, /*grace=*/0);
        EXPECT_TRUE(objectExists(os, blob_key)); // pinned -> kept

        // tx goes out of scope WITHOUT commit -> destructor must release the pin.
    }
    EXPECT_FALSE(ms->inFlightPinnedBlobs()->contains(blob_key)); // released on destruction

    // (b) The orphaned blob (no ref ever published, pin dropped) is now reclaimable by a LATER sweep.
    gc.runSweepOnce(/*now=*/200, /*grace=*/0);
    EXPECT_FALSE(objectExists(os, blob_key));
}

// B23 Task 4: removeRecursive deletes the per-ref sidecar objects WITH the ref. A part's mutable
// state is ref-scoped, so dropping the part (or the table) must reclaim its .meta sidecar(s)
// synchronously — they are NOT content-addressed, so the reachability sweep (blobs/+parts/ only)
// would never reclaim them; if removeRecursive left them they would leak. This pins both: the
// sidecar is gone after drop, and it never appeared under blobs/ or parts/ (not a CA object).
TEST_F(ContentAddressedMetaTest, RemoveDeletesSidecarsAndTheyAreNotContentAddressed)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_sidecar_gc");
    auto os = getObjectStorage("cas_sidecar_gc");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-sidecar-gc";

    auto write_part = [&](const std::string & part, const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };

    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"uuid.txt", "U1"}, {"txn_version.txt", "1"}});
    write_part("all_2_2_0", {{"a.bin", "AAA"}, {"uuid.txt", "U2"}, {"txn_version.txt", "2"}});

    const std::string bundle_1 = refMetaKey("", sid, uuid, "all_1_1_0").string();
    const std::string file_1_uuid = refMutableFileKey("", sid, uuid, "all_1_1_0", "uuid.txt").string();
    const std::string file_1_txn = refMutableFileKey("", sid, uuid, "all_1_1_0", "txn_version.txt").string();
    const std::string bundle_2 = refMetaKey("", sid, uuid, "all_2_2_0").string();
    ASSERT_TRUE(objectExists(os, bundle_1));
    ASSERT_TRUE(objectExists(os, file_1_uuid));
    ASSERT_TRUE(objectExists(os, file_1_txn));

    // The sidecar objects are NEVER content-addressed: they live under store/.../refs/, never blobs/ or parts/.
    EXPECT_NE(bundle_1.rfind("store/", 0), std::string::npos);
    EXPECT_EQ(bundle_1.rfind("blobs/", 0), std::string::npos);
    EXPECT_EQ(bundle_1.rfind("parts/", 0), std::string::npos);
    for (const auto & k : listKeysUnder(os, blobsPrefix("")))
        EXPECT_FALSE(isRefMetaKey(k)) << k;
    for (const auto & k : listKeysUnder(os, partsPrefix("")))
        EXPECT_FALSE(isRefMetaKey(k)) << k;

    // (1) Dropping a single part deletes ITS bundle + per-file sidecar objects, leaving the other part's.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/all_1_1_0", /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(objectExists(os, bundle_1));
    EXPECT_FALSE(objectExists(os, file_1_uuid));
    EXPECT_FALSE(objectExists(os, file_1_txn));
    EXPECT_TRUE(objectExists(os, bundle_2)); // the surviving part keeps its sidecar
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/uuid.txt")[0].remote_path), "U2");

    // (2) Dropping the whole table reclaims the remaining sidecar objects too.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(objectExists(os, bundle_2));
    EXPECT_FALSE(objectExists(os, refMutableFileKey("", sid, uuid, "all_2_2_0", "uuid.txt").string()));

    // A GC sweep does NOT need to (and does not) reclaim sidecars — they are already gone, and a
    // sweep run now is a clean no-op for them (they are not under blobs/+parts/).
    DB::ContentAddressed::ContentAddressedGC gc(os, "");
    EXPECT_NO_THROW(gc.runSweepOnce(/*now=*/100, /*grace=*/0));
}

// --- Honest capability predicate (B31, Part A) -------------------------------------------------

TEST_F(ContentAddressedMetaTest, IsContentAddressedPredicate)
{
    // The metadata storage advertises content-addressing; this is the single predicate the disk
    // layer (DiskObjectStorage::isContentAddressed / supportsHardLinks / supportZeroCopyReplication)
    // and MergeTree's CREATE/ATTACH gate consult to scope the disk honestly.
    auto ms = getMetadataStorage("cas_predicate");
    EXPECT_TRUE(ms->isContentAddressed());
    EXPECT_EQ(ms->getType(), DB::MetadataStorageType::ContentAddressed);
}

// --- _pool_meta ownership self-check (B11, Part C) ---------------------------------------------

TEST_F(ContentAddressedMetaTest, PoolMetaSerializeRoundTrips)
{
    PoolMeta meta;
    meta.version = PoolMeta::CURRENT_VERSION;
    meta.owner_server_id = "server-abc";
    meta.claimed_at_unix = 1234567;

    auto parsed = PoolMeta::deserialize(meta.serialize());
    EXPECT_EQ(parsed.version, PoolMeta::CURRENT_VERSION);
    EXPECT_EQ(parsed.owner_server_id, "server-abc");
    EXPECT_EQ(parsed.claimed_at_unix, 1234567);
}

TEST_F(ContentAddressedMetaTest, PoolMetaRejectsBadMagic)
{
    EXPECT_ANY_THROW(PoolMeta::deserialize("not a pool meta object"));
    EXPECT_ANY_THROW(PoolMeta::deserialize(""));
}

// Task 4: `_pool_meta` is on the shared codec now. Pin the on-object bytes (cross-arch determinism)
// and reject an unknown ENCODING version fail-closed (distinct from the body POOL-content version,
// which the caller gates — see PoolMetaUnknownVersionFailsClosed).
TEST_F(ContentAddressedMetaTest, PoolMetaGoldenBytesAndRejectsUnknownEncoding)
{
    PoolMeta meta;
    meta.version = 1;
    meta.owner_server_id = "srv";
    meta.claimed_at_unix = 1;
    const std::string expected =
        std::string("CAPM\x01", 5)                              // magic(4) + encoding version(1)
        + std::string("\x01\x00\x00\x00", 4)                    // pool content version u32 LE = 1
        + std::string("\x03", 1) + "srv"                        // owner server id (length-prefixed)
        + std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8);   // claimed_at_unix i64 LE = 1
    EXPECT_EQ(meta.serialize(), expected);
    EXPECT_EQ(PoolMeta::deserialize(meta.serialize()).owner_server_id, "srv");

    std::string future = meta.serialize();
    future[4] = static_cast<char>(PoolMeta::ENCODING_VERSION + 1); // bump the ENCODING version byte
    EXPECT_THROW(PoolMeta::deserialize(future), DB::Exception);
}

// These tests run claimPoolOwnership directly against a LocalObjectStorage. They use a NON-EMPTY
// key prefix (equal to the per-test os key) so each marker object key (`<prefix>/_pool_meta`) has a
// distinct parent directory: this fixture writes object keys verbatim relative to CWD, so an empty
// prefix would put `_pool_meta` at the root with no parent dir to create, and a shared prefix would
// collide across tests. In a real server the disk's key prefix is always non-empty and per-disk.

TEST_F(ContentAddressedMetaTest, PoolMetaClaimCreatesMarkerWhenAbsent)
{
    const std::string prefix = "cas_pool_claim";
    auto os = getObjectStorage(prefix);
    const std::string key = poolMetaKey(prefix);
    EXPECT_FALSE(os->tryGetObjectMetadata(key, /*with_tags=*/false).has_value());

    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test"));

    auto meta_after = os->tryGetObjectMetadata(key, /*with_tags=*/false);
    ASSERT_TRUE(meta_after.has_value());
    auto parsed = PoolMeta::deserialize(readObject(os, key));
    EXPECT_EQ(parsed.owner_server_id, "server-one");
    EXPECT_EQ(parsed.version, PoolMeta::CURRENT_VERSION);
}

TEST_F(ContentAddressedMetaTest, PoolMetaSameServerReMountIsOk)
{
    const std::string prefix = "cas_pool_same";
    auto os = getObjectStorage(prefix);
    // First mount claims, second mount by the SAME server must succeed without throwing and must
    // not change the owner (the common case — every M6 test run re-mounts the same server id).
    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test"));
    EXPECT_NO_THROW(DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test")));
    EXPECT_EQ(PoolMeta::deserialize(readObject(os, poolMetaKey(prefix))).owner_server_id, "server-one");
}

TEST_F(ContentAddressedMetaTest, PoolMetaSecondMounterFailsClosed)
{
    const std::string prefix = "cas_pool_conflict";
    auto os = getObjectStorage(prefix);
    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test"));
    // A DIFFERENT server mounting the same pool must fail closed (concurrent multi-mounter use is
    // not supported yet), and the marker must still name the original owner.
    EXPECT_THROW(
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-two", /*allow_shared=*/false, getLogger("test")),
        DB::Exception);
    EXPECT_EQ(PoolMeta::deserialize(readObject(os, poolMetaKey(prefix))).owner_server_id, "server-one");
}

TEST_F(ContentAddressedMetaTest, PoolMetaSecondMounterAllowedWithSharedFlag)
{
    const std::string prefix = "cas_pool_shared";
    auto os = getObjectStorage(prefix);
    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test"));
    // With the explicit shared/takeover opt-in, a different server proceeds and does NOT rewrite the
    // marker (so the other owner stays visible).
    EXPECT_NO_THROW(
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-two", /*allow_shared=*/true, getLogger("test")));
    EXPECT_EQ(PoolMeta::deserialize(readObject(os, poolMetaKey(prefix))).owner_server_id, "server-one");
}

TEST_F(ContentAddressedMetaTest, PoolMetaUnknownVersionFailsClosed)
{
    const std::string prefix = "cas_pool_badver";
    auto os = getObjectStorage(prefix);
    // Seed a marker with a version this build does not understand.
    PoolMeta future;
    future.version = PoolMeta::CURRENT_VERSION + 100;
    future.owner_server_id = "server-future";
    future.claimed_at_unix = 1;
    writeObject(os, poolMetaKey(prefix), future.serialize());

    EXPECT_THROW(
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test")),
        DB::Exception);
}

// M7 T1: a mutation rewrites ONE column and carries the rest forward by reference — the new part's
// unchanged-column blobs MUST be the SAME blob objects as the source (no re-upload); only the
// changed column is a fresh blob. This is the content-addressing sweet spot the mutation path relies
// on (MutateTask: createHardLinkFrom unchanged + writeFile changed, one commit).
TEST_F(ContentAddressedMetaTest, MutationCarryForwardReusesUnchangedBlobs)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mut_cf");
    auto os = getObjectStorage("cas_mut_cf");
    const std::string uuid = "uuid-mut";
    const std::string src = "all_1_1_0";
    const std::string dst = "all_1_1_0_2"; // mutation version 2

    {
        DB::ContentAddressedTransaction tx(*ms, "", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"a.bin", "AAA"}, {"b.bin", "BBB"}, {"columns.txt", "a b"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + src + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    auto src_a = ms->getStorageObjects("uui/" + uuid + "/" + src + "/a.bin")[0].remote_path;
    auto src_b = ms->getStorageObjects("uui/" + uuid + "/" + src + "/b.bin")[0].remote_path;

    {
        DB::ContentAddressedTransaction tx(*ms, "", kCasTestScratch);
        tx.createHardLinkFrom("uui/" + uuid + "/" + src + "/a.bin",       "uui/" + uuid + "/" + dst + "/a.bin");
        tx.createHardLinkFrom("uui/" + uuid + "/" + src + "/columns.txt", "uui/" + uuid + "/" + dst + "/columns.txt");
        auto buf = tx.writeFile("uui/" + uuid + "/" + dst + "/b.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string nb = "NEWB";
        buf->write(nb.data(), nb.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }

    // (a) unchanged columns carried forward -> SAME blob objects (no re-upload)
    EXPECT_EQ(ms->getStorageObjects("uui/" + uuid + "/" + dst + "/a.bin")[0].remote_path, src_a);
    // (b) changed column -> a DIFFERENT, fresh blob; content is the new bytes
    auto dst_b = ms->getStorageObjects("uui/" + uuid + "/" + dst + "/b.bin")[0].remote_path;
    EXPECT_NE(dst_b, src_b);
    EXPECT_EQ(readObject(os, dst_b), "NEWB");
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + dst + "/a.bin")[0].remote_path), "AAA");
    // (c) the source part is untouched (its b.bin still resolves to BBB)
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + src + "/b.bin")[0].remote_path), "BBB");
}

// A mutation writes a table-level entry file `tmp_mutation_N.txt` then renames it to `mutation_N.txt`
// (MergeTreeMutationEntry::commit -> DiskObjectStorage::moveFile). These are NOT part files: they are
// stored verbatim under a path-derived key. moveFile must physically move the verbatim object to the
// new key so the read path (which recomputes the key from the path) finds it under the new name.
// Before the fix moveFile threw LOGICAL_ERROR ("requires two part-file paths"), aborting the server
// under abort_on_logical_error on the very first ALTER ... UPDATE.
TEST_F(ContentAddressedMetaTest, MoveTableLevelFileRenamesVerbatimObject)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mv_tf");
    auto os = getObjectStorage("cas_mv_tf");
    const std::string uuid = "uuid-mv";
    const std::string from = "uui/" + uuid + "/tmp_mutation_5.txt";
    const std::string to   = "uui/" + uuid + "/mutation_5.txt";
    const std::string bytes = "format version: 1\n1\n";

    // Write the table-level file verbatim (writeFile's non-part branch is durable on finalize).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(from, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    EXPECT_TRUE(ms->existsFile(from));

    // Rename it, as the mutation entry commit does.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveFile(from, to);
        tx.commit(DB::NoCommitOptions{});
    }

    // The file resolves under the NEW name with the original bytes; the old name is gone.
    EXPECT_TRUE(ms->existsFile(to));
    EXPECT_FALSE(ms->existsFile(from));
    auto objs = ms->getStorageObjects(to);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(readObject(os, objs[0].remote_path), bytes);
}
