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

#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>

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
    writeObject(os, refKey("", sid, uuid, part).string(), part_id);

    const std::string logical = "uui/" + uuid + "/" + part + "/" + file; // <uuid[:3]>/<uuid>/<part>/<file>
    EXPECT_TRUE(ms->existsFile(logical));
    EXPECT_EQ(ms->getFileSize(logical), blob_data.size());
    auto objs = ms->getStorageObjects(logical);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, blobKey("", BlobHash(blob_csum)).string());
    EXPECT_EQ(readObject(os, objs[0].remote_path), blob_data);
    EXPECT_FALSE(ms->existsFile("uui/" + uuid + "/" + part + "/absent.bin")); // file not in manifest
}

TEST_F(ContentAddressedMetaTest, FailsClosedOnMissingManifest)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_failclose");
    auto os = getObjectStorage("cas_failclose");
    // ref present, but parts/<part_id> manifest absent → must THROW (B18), not return empty
    writeObject(os, refKey("", ms->serverIdForTest(), "uuid-3", "all_1_1_0").string(), "missingpid");
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
        writeObject(os, refKey("", sid, uuid, part).string(), pid);
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

    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_1_1_0").string(), s.pid_a);
    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_2_2_0").string(), s.pid_b);
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
    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_1_1_0_redo").string(), s.pid_a);

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
    ContentAddressedMetaTest::writeObject(os, refKey("", ms->serverIdForTest(), s.uuid, "all_3_3_0").string(), "deadc0de00000000000000000000beef");

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
