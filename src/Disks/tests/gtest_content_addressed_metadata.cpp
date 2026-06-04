#include <gtest/gtest.h>
#include <algorithm>
#include <optional>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGCThread.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
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
#include <set>
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

// A table-level file may live in a SUBDIRECTORY under the table dir (e.g. the non-replicated
// deduplication log deduplication_logs/deduplication_log_N.txt). The Atomic-layout parse keeps the
// full sub-path as the tail; a single-component tail is unchanged; the bare table dir is not a file.
TEST(ContentAddressedPoolPaths, ParseTableFilePathNested)
{
    using namespace DB::ContentAddressed;
    EXPECT_FALSE(isPartFilePath("uui/uuid-1/deduplication_logs/deduplication_log_1.txt"));
    auto tf = parseTableFilePath("uui/uuid-1/deduplication_logs/deduplication_log_1.txt");
    ASSERT_TRUE(tf.has_value());
    EXPECT_EQ(tf->table_uuid, "uuid-1");
    EXPECT_EQ(tf->tail, "deduplication_logs/deduplication_log_1.txt");

    auto flat = parseTableFilePath("uui/uuid-1/format_version.txt");
    ASSERT_TRUE(flat.has_value());
    EXPECT_EQ(flat->table_uuid, "uuid-1");
    EXPECT_EQ(flat->tail, "format_version.txt");

    EXPECT_FALSE(parseTableFilePath("uui/uuid-1").has_value());
    EXPECT_FALSE(parseTableFilePath("uui/uuid-1/").has_value());

    // A part file is still a part file, never mistaken for a (nested) table-level file.
    EXPECT_TRUE(isPartFilePath("uui/uuid-1/all_1_1_0/data.bin"));
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
        fs::remove_all("shadow", ec); // FREEZE shadow-ref namespace (shadowRefKey, empty key prefix)
        fs::remove_all("sessions", ec);
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

// The foundational create-if-absent compare-and-set primitive: the first writer creates the object and
// wins (true); a second create on the same key loses the CAS (false) and the first writer's bytes are
// preserved (first-writer-wins). On the gtest's LocalObjectStorage this exercises the atomic O_EXCL
// seam (the S3 If-None-Match path is exercised later on MinIO).
TEST_F(ContentAddressedMetaTest, CondCreateIfAbsentIsAtomic)
{
    using namespace DB::ContentAddressed;
    auto os = getObjectStorage("cas_cas");
    // For LocalObjectStorage the key is the local path verbatim; keep it under the storage root so the
    // fixture's getCommonKeyPrefix() cleanup removes it.
    const std::string key = "cas_cas/cas_lock";

    EXPECT_TRUE(condCreateIfAbsent(*os, key, "A"));  // first writer creates and wins
    EXPECT_FALSE(condCreateIfAbsent(*os, key, "B")); // second writer loses the CAS
    EXPECT_EQ(readObject(os, key), "A");             // first writer's bytes are preserved
}

// The fence-token allocator built on create-if-absent only: repeated allocations are strictly
// increasing and never repeat (each token is a uniquely-created fence/<n> object). Two interleaved
// allocators that share the same pool never collide on a token.
TEST_F(ContentAddressedMetaTest, FenceTokensAreMonotonicAndUnique)
{
    using namespace DB::ContentAddressed;
    auto os = getObjectStorage("cas_fence");
    const std::string prefix = "cas_fence";

    // Sequential allocations from hint 1 yield 1, 2, 3 (the first free token each time).
    EXPECT_EQ(allocateFenceToken(*os, prefix, 1), 1u);
    EXPECT_EQ(allocateFenceToken(*os, prefix, 1), 2u); // 1 is taken; scan up to 2
    EXPECT_EQ(allocateFenceToken(*os, prefix, 1), 3u); // 1,2 taken; scan up to 3

    // A hint at/below the high-water mark still converges by scanning upward; never repeats.
    uint64_t t4 = allocateFenceToken(*os, prefix, 2);
    EXPECT_EQ(t4, 4u);

    // A hint above the high-water mark jumps straight there (strictly higher than anything before).
    uint64_t t10 = allocateFenceToken(*os, prefix, 10);
    EXPECT_EQ(t10, 10u);
    EXPECT_GT(t10, t4);

    // Two distinct callers can never be handed the same token.
    uint64_t a = allocateFenceToken(*os, prefix, 1);
    uint64_t b = allocateFenceToken(*os, prefix, 1);
    EXPECT_NE(a, b);
}

// The fenced GC-leader lock: one holder at a time while the lease is live; an expired lease can be
// stolen, and the steal takes a STRICTLY HIGHER fence token. The previous holder then loses the renew
// (its fence is no longer on disk), while the successor renews fine. The lock object is only a liveness
// hint — the fence token is the authority a later task re-checks before deleting.
TEST_F(ContentAddressedMetaTest, GcLockGrantsOneHolderAndStealsAfterLease)
{
    using namespace DB::ContentAddressed;
    auto os = getObjectStorage("cas_gclock");
    const std::string prefix = "cas_gclock";

    // A takes the lock at now=1000 with a 100s lease (deadline 1100), getting some fence fA.
    auto a = tryAcquireGcLock(*os, prefix, "serverA", /*lease_seconds=*/100, /*now_unix=*/1000);
    ASSERT_TRUE(a.has_value());
    const uint64_t fA = a->fence_token;

    // B tries at now=1050 while A's lease (1100) is still live -> denied.
    auto b = tryAcquireGcLock(*os, prefix, "serverB", /*lease_seconds=*/100, /*now_unix=*/1050);
    EXPECT_FALSE(b.has_value());

    // C tries at now=1200 after A's lease expired -> steals with a STRICTLY HIGHER fence.
    auto c = tryAcquireGcLock(*os, prefix, "serverC", /*lease_seconds=*/100, /*now_unix=*/1200);
    ASSERT_TRUE(c.has_value());
    EXPECT_GT(c->fence_token, fA);
    EXPECT_EQ(c->server_id, "serverC");

    // A tries to renew at now=1300 -> false: C took over with a higher fence, A lost leadership.
    EXPECT_FALSE(renewGcLock(*os, prefix, *a, /*lease_seconds=*/100, /*now_unix=*/1300));

    // C renews fine (its fence is still the one on disk).
    EXPECT_TRUE(renewGcLock(*os, prefix, *c, /*lease_seconds=*/100, /*now_unix=*/1300));
    EXPECT_EQ(c->lease_deadline_unix, 1400u);

    // C releases; the lock is now free for a fresh acquire.
    releaseGcLock(*os, prefix, *c);
    auto d = tryAcquireGcLock(*os, prefix, "serverD", /*lease_seconds=*/100, /*now_unix=*/1500);
    ASSERT_TRUE(d.has_value());
    EXPECT_GT(d->fence_token, c->fence_token); // a re-create after release still allocates a higher fence
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

// B67: a transactional COMMIT appends the CSN line to the TABLE-LEVEL mutation entry mutation_<n>.txt
// via WriteMode::Append (MergeTreeMutationEntry::writeCSN, inside the noexcept afterCommit). A
// content-addressed metadata storage reports no native append, but it CAN service an append on a
// non-part / table-level verbatim file by read-modify-rewrite: read the existing bytes at the stable
// key, rewrite them, then write the appended bytes after them. Pin that behavior here (write Rewrite,
// then Append, then read back the concatenation), and pin that an Append on a PART file path is still
// rejected (a part file is a content blob or a whole-rewritten mutable file — append is meaningless).
TEST_F(ContentAddressedMetaTest, AppendOnTableLevelVerbatimFileRewrites)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_append");
    auto os = getObjectStorage("cas_append");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-append";
    // A table-level file (no part component), e.g. the mutation entry mutation_5.txt.
    const std::string path = "uui/" + uuid + "/mutation_5.txt";

    // The verbatim object lives at the table-file key derived from the path; resolve it the same way
    // the write path does so the read-back checks the actual on-disk bytes.
    const std::string key = tableFileKey("", sid, uuid, "mutation_5.txt");

    // Initial create with Rewrite (the mutation entry body).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
        const std::string body = "commands: x\n";
        buf->write(body.data(), body.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_EQ(readObject(os, key), "commands: x\n");

    // Append the CSN line (read-modify-rewrite): the existing bytes are carried forward and the new
    // bytes land after them.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(path, 256, DB::WriteMode::Append, {});
        const std::string csn = "csn: 7\n";
        buf->write(csn.data(), csn.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_EQ(readObject(os, key), "commands: x\ncsn: 7\n");

    // The read-modify-rewrite Append is serviced ONLY for a non-part / table-level verbatim file. The
    // DiskObjectStorageTransaction guard that rejects Append on a part file (a content blob / a
    // whole-rewritten mutable file — append is meaningless there) keys on isPartFilePath; pin that
    // classification so the table-level path takes the serviceable branch and a part file does not.
    EXPECT_FALSE(isPartFilePath(path));                                  // mutation_5.txt -> serviceable append
    EXPECT_TRUE(isPartFilePath("uui/" + uuid + "/all_1_1_0/data.bin"));  // a part file -> append rejected upstream
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

// B-Task W2: listing/existence of a SINGLE detached part DIRECTORY (detached/<detached_part>). Its files
// are re-keyed inside the shared "detached" ref's manifest (and per-ref sidecar) as <detached_part>/<file>
// (B36), so the dir is NOT a ref of its own. listDirectory on it must return the detached part's COMPLETE
// inner file set — the <inner> names (NOT the <detached_part>/<inner> keys, NOT empty), including the
// mutable sidecar file — exactly as an active part dir lists its files. Before the fix the part-dir branch
// was gated on file.empty() and fell through to {}, producing an incomplete clone manifest (no ref for
// .../primary.idx|data.cmrk4) on ATTACH PARTITION / ATTACH PART of a detached source, and existsDirectory
// returned false so getDiskForDetachedPart threw BAD_DATA_PART_NAME.
TEST_F(ContentAddressedMetaTest, ListDetachedPartDirReturnsInnerFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_detlist");
    auto os = getObjectStorage("cas_detlist");
    const std::string uuid = "uuid-detlist";

    // Commit an active part with several content files + a mutable per-part file, then move it into
    // detached/<part> so it goes through the real re-keying path (republishCommittedPartIntoDetached).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"a.bin", "AAA"},
                 {"primary.idx", "IDX"},
                 {"data.cmrk4", "MRK"},
                 {"columns.txt", "cols"},
                 {"metadata_version.txt", "0"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/all_1_2_1/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/all_1_2_1", "uui/" + uuid + "/detached/all_1_2_1");
        tx.commit(DB::NoCommitOptions{});
    }

    const std::string detached_part = "uui/" + uuid + "/detached/all_1_2_1";

    // The single detached part dir lists its COMPLETE inner file set: content blobs (incl. primary.idx /
    // data.cmrk4) AND the mutable per-part sidecar file (metadata_version.txt) — as <inner> names only.
    auto names = ms->listDirectory(detached_part);
    std::set<std::string> got(names.begin(), names.end());
    EXPECT_EQ(
        got,
        (std::set<std::string>{"a.bin", "primary.idx", "data.cmrk4", "columns.txt", "metadata_version.txt"}));

    // existsDirectory is true for the single detached part dir.
    EXPECT_TRUE(ms->existsDirectory(detached_part));

    // iterateDirectory mirrors listDirectory (path-prefixed child names).
    std::set<std::string> iter_names;
    for (auto it = ms->iterateDirectory(detached_part); it->isValid(); it->next())
        iter_names.insert(it->name());
    EXPECT_EQ(
        iter_names,
        (std::set<std::string>{"a.bin", "primary.idx", "data.cmrk4", "columns.txt", "metadata_version.txt"}));

    // The CONTAINER detached/ still lists the part-DIRECTORY name, not the inner files (unbroken).
    auto container = ms->listDirectory("uui/" + uuid + "/detached");
    std::set<std::string> container_got(container.begin(), container.end());
    EXPECT_EQ(container_got, (std::set<std::string>{"all_1_2_1"}));
}

// B64: a projection sub-directory must be recognized even when NESTED one level deeper than a direct
// child of a part — the shape read during ATTACH PARTITION, where the part is loaded from its detached
// STAGING directory detached/attaching_<part>/<proj>.proj. The CA metadata storage recognizes a
// projection dir by its LAST path component (.proj/.tmp_proj), so both the direct child shape
// (<part>/<proj>.proj) and the nested staging shape resolve. Before the fix the projection branches were
// gated on a SINGLE-component .proj file, so the nested staging projection was missed:
// existsDirectory("<proj>.proj") returned false during the attach-time load, and
// IMergeTreeDataPart::loadProjections registered the surviving projection part with empty columns /
// rows_count == 0, breaking it (CHECK TABLE then threw BROKEN_PROJECTION).
TEST_F(ContentAddressedMetaTest, NestedStagingProjectionDirIsRecognized)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_nestedproj");
    auto os = getObjectStorage("cas_nestedproj");
    const std::string uuid = "uuid-nestedproj";

    // Commit an active part that carries a projection p (its files nested under p.proj/ in the manifest),
    // then move it into detached/<part> and rename to detached/attaching_<part> (the staging name the
    // ATTACH PARTITION load reads from). This goes through the real re-keying path so the manifest keys
    // become attaching_<part>/<file> and attaching_<part>/p.proj/<file>.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"columns.txt", "cols"},
                 {"data.bin", "DAT"},
                 {"p.proj/columns.txt", "pcols"},
                 {"p.proj/count.txt", "7"},
                 {"p.proj/data.bin", "PDAT"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/all_1_1_0", "uui/" + uuid + "/detached/all_1_1_0");
        tx.commit(DB::NoCommitOptions{});
    }
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/detached/all_1_1_0", "uui/" + uuid + "/detached/attaching_all_1_1_0");
        tx.commit(DB::NoCommitOptions{});
    }

    const std::string staging_proj = "uui/" + uuid + "/detached/attaching_all_1_1_0/p.proj";

    // The NESTED staging projection dir is recognized as a directory (was false before the fix).
    EXPECT_TRUE(ms->existsDirectory(staging_proj));

    // Listing it yields the projection's OWN inner files (the p.proj/ prefix stripped), so the projection's
    // child DataPartStorage enumerates and reads exactly its files.
    auto names = ms->listDirectory(staging_proj);
    std::set<std::string> got(names.begin(), names.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "count.txt", "data.bin"}));

    // The projection's inner files resolve to their blobs through the manifest (full read-time state).
    auto cols = ms->getStorageObjects(staging_proj + "/columns.txt");
    ASSERT_EQ(cols.size(), 1u);
    EXPECT_EQ(readObject(os, cols[0].remote_path), "pcols");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(staging_proj + "/count.txt")[0].remote_path), "7");

    // The DIRECT-child projection shape is still recognized too (regression guard for the common case).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/detached/attaching_all_1_1_0", "uui/" + uuid + "/all_2_2_0");
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_TRUE(ms->existsDirectory("uui/" + uuid + "/all_2_2_0/p.proj"));
    auto active_names = ms->listDirectory("uui/" + uuid + "/all_2_2_0/p.proj");
    std::set<std::string> active_got(active_names.begin(), active_names.end());
    EXPECT_EQ(active_got, (std::set<std::string>{"columns.txt", "count.txt", "data.bin"}));
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

// CA GC S1 (B9): the incremental reverse index (InMemoryBlobRefIndex), wired on the commit/drop path,
// must EXACTLY agree with the authoritative full-scan reachable-blob set on a single-node-from-empty
// pool — the spec's S1 gate ("the validator must never disagree"). This drives the real transaction
// commit/drop path (so the index is updated through the production seams), and after EACH operation
// asserts that the index's reachable view ({ blobKey(prefix, H) : refcount(H) > 0 }) equals
// markReachableBlobs(prefix, listLivePartIds(...)). Covers shared-blob dedup (two parts pin the same
// blob -> refcount 2 -> drop one -> still reachable -> drop both -> unreferenced) and idempotent re-add.
TEST_F(ContentAddressedMetaTest, ReverseIndexEqualsAuthoritativeScan)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_revidx");
    auto os = getObjectStorage("cas_revidx");
    const std::string uuid = "uuid-revidx";

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

    auto drop_part = [&](const std::string & part)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/" + part, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    };

    /// The authoritative reachable-blob set (full scan): resolve every live ref to its manifest by
    /// reading the parts/<part_id> object directly (the same key the sweep's resolver reads), then
    /// project to full blob object keys via blobKey — using only public surfaces (no friend access).
    PartManifestResolver resolve = [&](const PartId & pid) -> PartManifest
    { return PartManifest::deserialize(readObject(os, partKey("", pid).string())); };
    auto authoritative = [&]() -> std::set<BlobObjectKey>
    { return markReachableBlobs("", listLivePartIds(os, ""), resolve); };

    /// Resolve a live part's manifest through its published ref payload (the read path's resolution),
    /// again with only public surfaces, so the test can read a specific blob's hash.
    auto manifest_of = [&](const std::string & part) -> PartManifest
    {
        const PartId pid = partIdFromRefPayload(readObject(os, refKey("", ms->serverIdForTest(), uuid, part).string()));
        return resolve(pid);
    };

    /// The index's reachable view: every bare hash with a positive per-process refcount, projected to a
    /// full blob object key with the SAME blobKey fan-out, so the two sets are directly comparable.
    auto from_index = [&]() -> std::set<BlobObjectKey>
    {
        std::set<BlobObjectKey> r;
        for (const auto & h : ms->blobRefIndex()->referenced())
            r.insert(blobKey("", h));
        return r;
    };

    /// Empty pool: nothing reachable, nothing in the index.
    EXPECT_EQ(from_index(), authoritative());
    EXPECT_TRUE(authoritative().empty());

    /// Two parts SHARING one blob ("SHARED"): after both commits, the shared blob is pinned twice
    /// (refcount 2) while the index's reachable set still equals the scan (a blob is reachable iff
    /// refcount > 0, regardless of the count).
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    EXPECT_EQ(from_index(), authoritative());
    write_part("all_2_2_0", {{"a.bin", "ZZZ"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    EXPECT_EQ(from_index(), authoritative());

    /// The shared blob's refcount is exactly 2 (pinned by both parts).
    const PartManifest m1 = manifest_of("all_1_1_0");
    const BlobHash shared_hash = m1.blobs.at("b.bin").key;
    EXPECT_EQ(ms->blobRefIndex()->refcount(shared_hash), 2);

    /// Idempotent re-add: re-committing the SAME part (same content -> same part_id) must not double
    /// count (applied_parts guard) and must keep index == scan.
    write_part("all_1_1_0", {{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    EXPECT_EQ(ms->blobRefIndex()->refcount(shared_hash), 2);
    EXPECT_EQ(from_index(), authoritative());

    /// Drop ONE of the two sharers: the shared blob is STILL reachable (refcount drops 2 -> 1) and the
    /// dropped part's private blob ("AAA") becomes unreferenced — index and scan agree on both.
    drop_part("all_1_1_0");
    EXPECT_EQ(ms->blobRefIndex()->refcount(shared_hash), 1);
    EXPECT_EQ(from_index(), authoritative());

    /// Drop the LAST sharer: the shared blob is now unreferenced (refcount 0) and the index's reachable
    /// set is empty, exactly matching the now-empty authoritative scan.
    drop_part("all_2_2_0");
    EXPECT_EQ(ms->blobRefIndex()->refcount(shared_hash), 0);
    EXPECT_EQ(from_index(), authoritative());
    EXPECT_TRUE(from_index().empty());
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

// A table-level file in a SUBDIRECTORY (the non-replicated deduplication log lives at
// deduplication_logs/deduplication_log_N.txt) round-trips through the table-level files/ namespace:
// it writes/reads back verbatim, the subdirectory is discoverable via existsDirectory, the log files
// enumerate via listDirectory (and iterateDirectory), the subdir shows up as one child of the table
// dir, and removal unlinks just that object. This is exactly what MergeTreeDeduplicationLog needs to
// load and rotate its logs on a content-addressed disk (the same rewrite-per-record path as plain s3).
TEST_F(ContentAddressedMetaTest, TableLevelSubdirectoryRoundTripsLikeDedupLog)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_subdir");
    auto os = getObjectStorage("cas_subdir");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-subdir";
    const std::string dir = "uui/" + uuid + "/deduplication_logs";
    const std::string log1 = dir + "/deduplication_log_1.txt";
    const std::string log2 = dir + "/deduplication_log_2.txt";

    auto writeTableFile = [&](const std::string & path, const std::string & bytes)
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    };

    // Before any write the subdirectory does not exist (mirrors a fresh table on load()).
    EXPECT_FALSE(ms->existsDirectory(dir));

    writeTableFile(log1, "1\tall_1_1_0\tblock-a\n");

    // The log file resolves verbatim under the table's files/ namespace, keeping its sub-path.
    EXPECT_TRUE(ms->existsFile(log1));
    auto objs = ms->getStorageObjects(log1);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, tableFileKey("", sid, uuid, "deduplication_logs/deduplication_log_1.txt"));
    EXPECT_EQ(readObject(os, objs[0].remote_path), "1\tall_1_1_0\tblock-a\n");

    // The subdirectory is now discoverable and lists its single log file.
    EXPECT_TRUE(ms->existsDirectory(dir));
    {
        auto names = ms->listDirectory(dir);
        ASSERT_EQ(names.size(), 1u);
        EXPECT_EQ(names[0], "deduplication_log_1.txt");
    }

    // iterateDirectory prepends the queried path to each child (as MergeTreeDeduplicationLog::load relies on).
    {
        std::vector<std::string> it_paths;
        for (auto it = ms->iterateDirectory(dir); it->isValid(); it->next())
            it_paths.push_back(it->path());
        ASSERT_EQ(it_paths.size(), 1u);
        EXPECT_EQ(it_paths[0], dir + "/deduplication_log_1.txt");
    }

    // The table dir lists the subdirectory as a single child entry (first-component collapse).
    {
        auto table_children = ms->listDirectory("uui/" + uuid);
        EXPECT_NE(std::find(table_children.begin(), table_children.end(), "deduplication_logs"), table_children.end());
    }

    // A second rotated log file is independently enumerated.
    writeTableFile(log2, "2\tall_1_1_0\tblock-a\n");
    {
        auto names = ms->listDirectory(dir);
        ASSERT_EQ(names.size(), 2u);
    }

    // Dropping an outdated log unlinks just that object; the other remains.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.unlinkFile(log1, /*if_exists=*/false, /*should_remove_objects=*/true);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(ms->existsFile(log1));
    EXPECT_TRUE(ms->existsFile(log2));
    {
        auto names = ms->listDirectory(dir);
        ASSERT_EQ(names.size(), 1u);
        EXPECT_EQ(names[0], "deduplication_log_2.txt");
    }

    // Removing the last log empties the subdirectory.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.unlinkFile(log2, /*if_exists=*/false, /*should_remove_objects=*/true);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(ms->existsDirectory(dir));
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

/// `prefix` is the pool key prefix the seeded objects live under (default empty for the legacy direct
/// tests). The coordinated-GC tests pass a NON-empty prefix so the GC-leader lock / fence objects have a
/// parent dir — the LocalObjectStorage O_EXCL create cannot materialize a file at the empty-prefix root.
CasGcSeed seedGcPool(const std::shared_ptr<DB::IObjectStorage> & os, const std::string & sid, const std::string & prefix = "")
{
    using namespace DB::ContentAddressed;
    CasGcSeed s;
    auto put_blob = [&](const std::string & csum) { ContentAddressedMetaTest::writeObject(os, blobKey(prefix, BlobHash(csum)).string(), csum); };
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
        ContentAddressedMetaTest::writeObject(os, partKey(prefix, PartId(pid)).string(), f.serialize());
    };
    put_manifest(s.pid_a, {{"a.bin", s.b1}, {"shared.bin", s.b2}});
    put_manifest(s.pid_b, {{"shared.bin", s.b2}, {"c.bin", s.b3}});
    put_manifest(s.pid_orphan, {{"o.bin", s.b_orphan}});

    ContentAddressedMetaTest::writeObject(os, refKey(prefix, sid, s.uuid, "all_1_1_0").string(), serializeRefPayload(PartId(s.pid_a)));
    ContentAddressedMetaTest::writeObject(os, refKey(prefix, sid, s.uuid, "all_2_2_0").string(), serializeRefPayload(PartId(s.pid_b)));
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

// CAS M8: a blob with NO ref but pinned by a LIVE write session is a root — a sweep past grace must
// NOT reclaim it (cross-mounter generalization of the in-process B52 pin). The session object lives in
// the bucket under sessionsPrefix and lists the blob's bare hash in `pending`; the sweep projects that
// to the FULL blob object key (same blobKey fan-out as the reachable set) so it is kept. Once the
// session is gone (or its lease expired), the blob is reclaimable again.
TEST_F(ContentAddressedMetaTest, SweepTreatsLiveWriteSessionPinAsRoot)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_session_root");
    auto os = getObjectStorage("cas_gc_session_root");

    // A single blob with NO ref or manifest pointing at it — normally collectable past grace.
    const std::string hash = "5555555555";
    ContentAddressedMetaTest::writeObject(os, blobKey("", BlobHash(hash)).string(), hash);

    // A LIVE write session (lease in the future) pins it via `pending`.
    WriteSession session;
    session.server_id = "server-pin";
    session.lease_deadline_unix = 10000; // >= the sweep's `now` below
    session.part_id = PartId("aaaa000000000000000000000000aaaa");
    session.pending = {BlobHash(hash)};
    ContentAddressedMetaTest::writeObject(os, sessionKey("", "sess-1"), session.serialize());

    DB::ContentAddressed::ContentAddressedGC gc(os, "");

    // First sweep records the (would-be) timer; the second is past grace. The live session keeps the
    // blob reachable across both, so it is NEVER deleted.
    gc.runSweepOnce(/*now=*/100, /*grace=*/100);
    gc.runSweepOnce(/*now=*/300, /*grace=*/100);
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(hash)).string())) << "live-session-pinned blob must survive";

    // Remove the session pin: the blob is now a true orphan. A sweep past grace reclaims it.
    os->removeObjectsIfExist({DB::StoredObject(sessionKey("", "sess-1"))});
    gc.runSweepOnce(/*now=*/500, /*grace=*/100); // first sweep without the pin: starts the timer
    gc.runSweepOnce(/*now=*/700, /*grace=*/100); // past grace -> reclaimed
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(hash)).string())) << "unpinned orphan blob must be reclaimed";
}

// CAS M8: re-validate-under-lock. The sweep re-reads the live roots (refs + live sessions) immediately
// before deletion, so a ref that became present is honored even if a stale snapshot would have marked
// the blob unreferenced. Here we add a ref to the otherwise-orphan part BETWEEN the timer-starting sweep
// and the past-grace sweep: the re-validation re-reads the ref set and finds the new ref, so neither the
// part manifest nor its blob is deleted. This pins the invariant that a blob/part with a live ref is
// never deleted, which is exactly what the re-validate pass re-checks.
TEST_F(ContentAddressedMetaTest, SweepRevalidatesBeforeDelete)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_revalidate");
    auto os = getObjectStorage("cas_gc_revalidate");
    const std::string sid = ms->serverIdForTest();
    auto s = seedGcPool(os, sid);

    DB::ContentAddressed::ContentAddressedGC gc(os, "");

    // Unlink pidA's ref so pidA's manifest + its exclusive blob b1 become unreferenced at the snapshot.
    os->removeObjectsIfExist({DB::StoredObject(refKey("", sid, s.uuid, "all_1_1_0").string())});
    gc.runSweepOnce(/*now=*/0, /*grace=*/100); // starts the timer for pidA manifest + b1 + the orphan

    // Re-add a ref pointing at pidA BEFORE the past-grace sweep. The candidate set (computed from the
    // snapshot timers) still lists pidA's manifest + b1, but the re-validate pass re-reads the refs and
    // finds pidA live again -> it must NOT delete pidA's manifest or b1.
    ContentAddressedMetaTest::writeObject(os, refKey("", sid, s.uuid, "all_1_1_0_redo").string(), serializeRefPayload(PartId(s.pid_a)));

    auto r = gc.runSweepOnce(/*now=*/200, /*grace=*/100);

    // pidA's manifest + b1 SURVIVE (re-validated reachable); only the never-referenced orphan is reclaimed.
    EXPECT_TRUE(objectExists(os, partKey("", PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, blobKey("", BlobHash(s.b1)).string()));
    EXPECT_FALSE(objectExists(os, partKey("", PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey("", BlobHash(s.b_orphan)).string()));
    EXPECT_EQ(r.deleted_parts, 1u); // orphan manifest only
    EXPECT_EQ(r.deleted_blobs, 1u); // orphan blob only
}

// CAS M8: the fence-ownership guard (the safety backstop). A holder takes the GC-leader lock (fence f1).
// A peer then steals it after the lease expires (fence f2 > f1). When the stale-f1 holder runs a sweep
// passing its now-superseded lock, the guard re-reads gc.lock, sees the higher fence, and deletes
// NOTHING — a paused holder must never delete after a successor took a higher fence. With the CURRENT
// holder's lock the same sweep deletes normally.
TEST_F(ContentAddressedMetaTest, SweepStopsWhenLeadershipLost)
{
    using namespace DB::ContentAddressed;
    // A non-empty key_prefix so the lock/fence objects (gc.lock, fence/<n>) have a parent dir — the
    // LocalObjectStorage CAS create cannot materialize a file at the bucket root (empty parent path).
    // The GC and the seeded orphan use the SAME prefix so the reachable set and lock key are consistent.
    const std::string p = "cas_gc_fence";
    auto ms = getMetadataStorage(p);
    auto os = getObjectStorage(p);

    // A single reclaimable orphan blob: no ref, no manifest points at it.
    const std::string hash = "7777777777";
    ContentAddressedMetaTest::writeObject(os, blobKey(p, BlobHash(hash)).string(), hash);

    // Holder A takes leadership at now=1000 (lease 100 -> deadline 1100), fence f1.
    auto a = tryAcquireGcLock(*os, p, "serverA", /*lease_seconds=*/100, /*now_unix=*/1000);
    ASSERT_TRUE(a.has_value());

    // Peer B steals after A's lease expired (now=1200), getting a strictly higher fence f2.
    auto b = tryAcquireGcLock(*os, p, "serverB", /*lease_seconds=*/100, /*now_unix=*/1200);
    ASSERT_TRUE(b.has_value());
    ASSERT_GT(b->fence_token, a->fence_token);

    // A's sweep with its STALE lock must delete NOTHING (the on-disk fence is now f2 != f1).
    DB::ContentAddressed::ContentAddressedGC gc_stale(os, p);
    gc_stale.runSweepOnce(/*now=*/1300, /*grace=*/0, /*held=*/a);
    EXPECT_TRUE(objectExists(os, blobKey(p, BlobHash(hash)).string())) << "stale holder must not delete";

    // The CURRENT holder B's sweep deletes the orphan normally (its fence matches the on-disk lock).
    DB::ContentAddressed::ContentAddressedGC gc_live(os, p);
    auto r = gc_live.runSweepOnce(/*now=*/1300, /*grace=*/0, /*held=*/b);
    EXPECT_FALSE(objectExists(os, blobKey(p, BlobHash(hash)).string()));
    EXPECT_EQ(r.deleted_blobs, 1u);
}

// Background-thread driver (Task 3a): the ContentAddressedGCThread runs runSweepOnce on the schedule
// pool. With a tiny grace, one triggerAndWait() round must reclaim the orphan manifest + orphan blob
// while the live manifests and referenced blobs survive — proving the thread starts, runs the sweep,
// stops cleanly, and uses the round counter (not a sleep) for synchronisation.
TEST_F(ContentAddressedMetaTest, GCThreadSweepsOrphansAndKeepsLive)
{
    using namespace DB::ContentAddressed;
    /// A NON-empty key prefix so the GC-leader lock objects (gc.lock, fence/<n>) the coordinated
    /// background thread now creates have a parent dir — the LocalObjectStorage O_EXCL create cannot
    /// materialize a file at the empty-prefix root. Seed + GC + assertions all use the SAME prefix.
    const std::string p = "cas_gc_thread";
    auto ms = getMetadataStorage(p);
    auto os = getObjectStorage(p);
    auto s = seedGcPool(os, ms->serverIdForTest(), p);

    DB::ContentAddressedGCThread thread(
        "cas_gc_thread_disk",
        getContext().context,
        os,
        p,
        ms->serverIdForTest(),
        std::make_shared<std::mutex>(),
        std::make_shared<const std::set<std::string>>(),
        /*blob_ref_index_=*/nullptr, /// hand-seeded pool (no transaction) -> drift validator skipped
        getLogger("ContentAddressedGCThreadTest"));

    /// grace 0 so a single round past first-unreachable reclaims orphans immediately.
    Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration());
    cfg->setInt("disk.content_addressed_gc_grace_sec", 0);
    cfg->setInt("disk.content_addressed_gc_interval_sec", 600);
    thread.applyNewSettings(*cfg, "disk");

    thread.startup();
    thread.triggerAndWait();

    /// The orphan manifest + orphan blob are gone; the 2 live manifests + 3 referenced blobs remain.
    EXPECT_FALSE(objectExists(os, partKey(p, PartId(s.pid_orphan)).string()));
    EXPECT_FALSE(objectExists(os, blobKey(p, BlobHash(s.b_orphan)).string()));
    EXPECT_TRUE(objectExists(os, partKey(p, PartId(s.pid_a)).string()));
    EXPECT_TRUE(objectExists(os, partKey(p, PartId(s.pid_b)).string()));
    EXPECT_TRUE(objectExists(os, blobKey(p, BlobHash(s.b1)).string()));
    EXPECT_TRUE(objectExists(os, blobKey(p, BlobHash(s.b2)).string()));
    EXPECT_TRUE(objectExists(os, blobKey(p, BlobHash(s.b3)).string()));

    /// Clean shutdown must not hang or crash (deactivates the scheduled task).
    thread.shutdown();
}

// CAS M8 (multi-mount + exclusive coordinated GC): TWO ContentAddressedMetadataStorage with DISTINCT
// server ids share ONE object storage / key prefix (the shared-pool case). Storage A writes and COMMITS
// a part (publishes a ref). Storage B writes a part but holds its transaction OPEN — its blobs are
// uploaded and pinned by a LIVE cross-mounter write session (no ref yet). A coordinated sweep run as the
// GC-leader (holding the fenced GcLock) must delete NEITHER A's referenced blobs NOR B's session-pinned
// blobs, and both parts must read back (A immediately; B after it commits). This proves a shared pool is
// safe: the fence-guarded sweep keeps reachable + session-pinned data while reclaiming only true orphans.
TEST_F(ContentAddressedMetaTest, TwoMetadataStoragesShareOnePoolGcIsExclusiveAndPinSafe)
{
    using namespace DB::ContentAddressed;

    /// A NON-empty pool key prefix so the GC-leader lock / fence objects have a parent dir (the
    /// LocalObjectStorage O_EXCL create cannot materialize a file at the empty-prefix root). Both
    /// mounters AND the leader sweep use the SAME prefix so refs, manifests, blobs and the lock key
    /// all live in one consistent pool. Resolved verbatim from CWD by LocalObjectStorage.
    const std::string p = "cas_share_pool";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));

    /// Two storages over the SAME object storage + key prefix, distinct server ids, allow_shared=true.
    /// No context => no background GC thread (we drive the coordinated sweep by hand below).
    auto make_storage = [&](const std::string & sid)
    {
        return std::make_shared<DB::ContentAddressedMetadataStorage>(
            os, /*storage_path_prefix=*/p, sid, kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    };
    auto a = make_storage("serverA");
    auto b = make_storage("serverB");

    /// startup registers each mounter in the registry; the second (B) must NOT fail-close because
    /// allow_shared is set (it registers and proceeds — the coordination makes it safe).
    a->startup();
    b->startup();
    EXPECT_TRUE(objectExists(os, poolMounterKey(p, "serverA")));
    EXPECT_TRUE(objectExists(os, poolMounterKey(p, "serverB")));

    const std::string uuid_a = "uuid-a";
    const std::string uuid_b = "uuid-b";

    /// Storage A writes + COMMITS a part: blobs uploaded, manifest written, ref published.
    {
        DB::ContentAddressedTransaction tx(*a, /*key_prefix=*/p, kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid_a + "/all_1_1_0/a.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data_a = "A-DATA";
        buf->write(data_a.data(), data_a.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    /// Record A's blob object key (the resolution path projects the bare hash to the full key).
    auto a_objs = a->getStorageObjects("uui/" + uuid_a + "/all_1_1_0/a.bin");
    ASSERT_EQ(a_objs.size(), 1u);
    const std::string a_blob_key = a_objs[0].remote_path;
    EXPECT_TRUE(objectExists(os, a_blob_key));

    /// Storage B writes a part but holds the transaction OPEN: its blob is uploaded and a LIVE write
    /// session pins it, but no ref is published yet. The transaction object must outlive the sweep.
    auto tx_b = std::make_unique<DB::ContentAddressedTransaction>(*b, /*key_prefix=*/p, kCasTestScratch);
    {
        auto buf = tx_b->writeFile("uui/" + uuid_b + "/all_1_1_0/b.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data_b = "B-DATA-UNCOMMITTED";
        buf->write(data_b.data(), data_b.size());
        buf->finalize();
    }
    /// B's blob exists on disk and exactly one live write session pins it (no ref yet).
    auto session_keys = listKeysUnder(os, sessionsPrefix(p));
    ASSERT_EQ(session_keys.size(), 1u) << "B's open transaction must leave one live write-session pin";
    auto b_session = WriteSession::deserialize(readObject(os, session_keys[0]));
    ASSERT_EQ(b_session.pending.size(), 1u);
    const std::string b_blob_key = blobKey(p, b_session.pending.front()).string();
    EXPECT_TRUE(objectExists(os, b_blob_key)) << "B's uploaded blob must exist before commit";

    /// Run a coordinated sweep as the GC-LEADER: acquire the fenced GcLock and pass it to runSweepOnce.
    /// The session lease is wall-clock seconds (set by the transaction); use a `now` below the lease so
    /// B's session is LIVE, with grace 0 so any true orphan would be reclaimed this round.
    const int64_t now = 1;
    auto held = tryAcquireGcLock(*os, p, "serverGc", /*lease_seconds=*/100, /*now_unix=*/static_cast<uint64_t>(now));
    ASSERT_TRUE(held.has_value());

    DB::ContentAddressed::ContentAddressedGC gc(os, p);
    gc.runSweepOnce(now, /*grace=*/0, /*held=*/held);

    /// Neither A's referenced blob nor B's session-pinned blob was deleted.
    EXPECT_TRUE(objectExists(os, a_blob_key)) << "A's referenced blob must survive the coordinated sweep";
    EXPECT_TRUE(objectExists(os, b_blob_key)) << "B's live-session-pinned blob must survive the coordinated sweep";

    /// A's part reads back unchanged.
    EXPECT_EQ(readObject(os, a->getStorageObjects("uui/" + uuid_a + "/all_1_1_0/a.bin")[0].remote_path), "A-DATA");

    /// B commits AFTER the sweep (its blob survived), then its part reads back too — the pin held the
    /// just-uploaded blob reachable across the window between upload and ref publish.
    tx_b->commit(DB::NoCommitOptions{});
    tx_b.reset();
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid_b + "/all_1_1_0/b.bin")[0].remote_path), "B-DATA-UNCOMMITTED");

    releaseGcLock(*os, p, *held);

    a->shutdown();
    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
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
    meta.pool_uuid = "01234567-89ab-cdef-0123-456789abcdef";

    auto parsed = PoolMeta::deserialize(meta.serialize());
    EXPECT_EQ(parsed.version, PoolMeta::CURRENT_VERSION);
    EXPECT_EQ(parsed.owner_server_id, "server-abc");
    EXPECT_EQ(parsed.claimed_at_unix, 1234567);
    EXPECT_EQ(parsed.pool_uuid, "01234567-89ab-cdef-0123-456789abcdef");
}

TEST_F(ContentAddressedMetaTest, PoolMetaRejectsBadMagic)
{
    EXPECT_ANY_THROW(PoolMeta::deserialize("not a pool meta object"));
    EXPECT_ANY_THROW(PoolMeta::deserialize(""));
}

// CAS M8 Phase B: the WriteSession PIN object is on the shared codec. A full round-trip preserves every
// field (incl. the pending hash vector in order), and a corrupted magic or a bumped/unknown ENCODING
// version both fail closed (a remote GC must never misparse a foreign object, and an older build must
// never reinterpret a session written by a newer one — both would risk reclaiming pinned blobs).
TEST_F(ContentAddressedMetaTest, WriteSessionRoundTripsAndRejectsBadVersion)
{
    WriteSession session;
    session.server_id = "server-write-1";
    session.lease_deadline_unix = 1700000123;
    session.fence_token = 42;
    session.part_id = PartId("aaaa000000000000000000000000aaaa");
    session.pending = {BlobHash("deadbeef01"), BlobHash("cafef00d02"), BlobHash("0badc0de03")};

    auto parsed = WriteSession::deserialize(session.serialize());
    EXPECT_EQ(parsed.server_id, session.server_id);
    EXPECT_EQ(parsed.lease_deadline_unix, session.lease_deadline_unix);
    EXPECT_EQ(parsed.fence_token, session.fence_token);
    EXPECT_EQ(parsed.part_id, session.part_id);
    EXPECT_EQ(parsed.pending, session.pending);

    // A stray / foreign object is rejected (bad magic), as is an empty body.
    EXPECT_THROW(WriteSession::deserialize("not a write session object"), DB::Exception);
    EXPECT_THROW(WriteSession::deserialize(""), DB::Exception);

    // A bumped ENCODING version is rejected fail-closed (never reinterpret a newer object).
    std::string future = session.serialize();
    future[4] = static_cast<char>(WriteSession::ENCODING_VERSION + 1); // bump the ENCODING version byte
    EXPECT_THROW(WriteSession::deserialize(future), DB::Exception);
}

// CAS M8 Phase B (cross-mounter pin, end-to-end): while a part is being written but BEFORE its ref is
// published, the transaction must have published exactly one WriteSession object that lists the part's
// blob hashes — so a GC sweep on ANOTHER mounter treats those just-uploaded-but-not-yet-referenced
// blobs as reachable. Once the ref is published (commit) the session object is removed (the ref now
// keeps the blobs reachable). This is the cross-process generalization of the in-process pin (B52).
TEST_F(ContentAddressedMetaTest, SessionPinListsPendingBlobsBeforeRefPublish)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_session_pin");
    auto os = getObjectStorage("cas_session_pin");
    const std::string uuid = "uuid-session-pin";
    const std::string part = "all_1_1_0";
    const std::map<std::string, std::string> files{{"a.bin", "AAA"}, {"b.bin", "BBBB"}, {"columns.txt", "x y"}};

    // Build the part (finalize each content file) but do NOT commit yet: each write-buffer finalize
    // pins its blob in the session BEFORE the upload, rewriting the one session object per blob.
    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & [name, bytes] : files)
    {
        auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }

    // Exactly one session object exists under the sessions prefix (one per in-flight transaction).
    auto session_keys = listKeysUnder(os, sessionsPrefix(""));
    ASSERT_EQ(session_keys.size(), 1u) << "expected exactly one in-flight WriteSession object";

    // It deserializes to a session that names this part and lists this part's blob hashes. The hashes
    // are the same the part's content files resolve to: project them to full blob object keys and
    // compare against the blob keys the committed part will read back from.
    auto session = WriteSession::deserialize(readObject(os, session_keys[0]));
    EXPECT_EQ(session.part_id, PartId(part));
    EXPECT_EQ(session.pending.size(), files.size());

    std::set<std::string> pinned_blob_keys;
    for (const auto & h : session.pending)
        pinned_blob_keys.insert(blobKey("", h).string());

    // Now publish the ref: the session object must be GONE (the ref keeps the blobs reachable).
    tx.commit(DB::NoCommitOptions{});
    EXPECT_TRUE(listKeysUnder(os, sessionsPrefix("")).empty()) << "session object must be cleared on commit";

    // The part still reads back correctly, and the blobs the session pinned are exactly the part's blobs.
    std::set<std::string> committed_blob_keys;
    for (const auto & [name, bytes] : files)
    {
        auto objs = ms->getStorageObjects("uui/" + uuid + "/" + part + "/" + name);
        ASSERT_EQ(objs.size(), 1u);
        EXPECT_EQ(readObject(os, objs[0].remote_path), bytes);
        committed_blob_keys.insert(objs[0].remote_path);
    }
    EXPECT_EQ(pinned_blob_keys, committed_blob_keys);
}

// B57: LocalObjectStorage::removeObject prunes empty parent dirs, so a sibling transaction's
// releaseSession can rmdir the shared sessions/ dir while another transaction is still writing its pin.
// persistSession must survive a pruned sessions/ dir: the next write re-creates it. (This reproduces the
// pruned-directory state deterministically; the tight create_directories->open race itself is intrinsic
// to LocalObjectStorage and is covered end-to-end by 02434_cancel_insert_when_client_dies.)
TEST_F(ContentAddressedMetaTest, SessionPersistSurvivesSessionsDirPrune)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_session_prune");
    auto os = getObjectStorage("cas_session_prune");
    const std::string uuid = "uuid-session-prune";
    const std::string part = "all_1_1_0";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);

    // First blob opens the session, creating the sessions/ directory.
    {
        auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/a.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write("AAA", 3);
        buf->finalize();
    }
    auto keys = listKeysUnder(os, sessionsPrefix(""));
    ASSERT_EQ(keys.size(), 1u);

    // Simulate a concurrent sibling's releaseSession: removeObjectIfExists -> LocalObjectStorage's
    // removeObject unlinks the only session object AND prunes the now-empty sessions/ directory.
    os->removeObjectIfExists(DB::StoredObject(keys[0]));
    EXPECT_TRUE(listKeysUnder(os, sessionsPrefix("")).empty());

    // Second blob: persistSession must re-create the pruned directory and succeed (no CANNOT_OPEN_FILE).
    {
        auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/b.bin", 4096, DB::WriteMode::Rewrite, {});
        buf->write("BBBB", 4);
        buf->finalize();
    }

    // The session is durable again and pins BOTH blobs.
    auto keys2 = listKeysUnder(os, sessionsPrefix(""));
    ASSERT_EQ(keys2.size(), 1u);
    auto session = WriteSession::deserialize(readObject(os, keys2[0]));
    EXPECT_EQ(session.pending.size(), 2u);

    tx.commit(DB::NoCommitOptions{});
}

// CAS M8 Phase B: an ABORTED transaction (one that goes out of scope without commit) must not leave a
// lingering session object. Its lease would expire anyway, but the destructor removes it eagerly,
// mirroring the in-process pin release.
TEST_F(ContentAddressedMetaTest, AbortedTransactionLeavesNoSession)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_session_abort");
    auto os = getObjectStorage("cas_session_abort");
    const std::string uuid = "uuid-session-abort";
    const std::string part = "all_1_1_0";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{{"a.bin", "AAA"}, {"b.bin", "BBBB"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        // While live, the session object exists (the cross-mounter pin is active).
        ASSERT_EQ(listKeysUnder(os, sessionsPrefix("")).size(), 1u);
        // tx goes out of scope WITHOUT commit -> destructor must remove the session object.
    }
    EXPECT_TRUE(listKeysUnder(os, sessionsPrefix("")).empty()) << "aborted transaction must leave no session";
}

// Task 4: `_pool_meta` is on the shared codec now. Pin the on-object bytes (cross-arch determinism)
// and reject an unknown ENCODING version fail-closed (distinct from the body POOL-content version,
// which the caller gates — see PoolMetaUnknownVersionFailsClosed).
TEST_F(ContentAddressedMetaTest, PoolMetaGoldenBytesAndRejectsUnknownEncoding)
{
    PoolMeta meta;
    // CAS replication Phase 1.1 bumped the POOL-content version to 2 and appended `pool_uuid` LAST in
    // the body. The golden bytes are updated for the version-2 layout: the version u32 is now 2 and a
    // length-prefixed pool_uuid trails the (unchanged) version-1 fields. The encoding version byte is
    // unchanged (only the body grew), so an unknown ENCODING version still fails closed (below).
    meta.version = 2;
    meta.owner_server_id = "srv";
    meta.claimed_at_unix = 1;
    meta.pool_uuid = "pu";
    const std::string expected =
        std::string("CAPM\x01", 5)                              // magic(4) + encoding version(1)
        + std::string("\x02\x00\x00\x00", 4)                    // pool content version u32 LE = 2
        + std::string("\x03", 1) + "srv"                        // owner server id (length-prefixed)
        + std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8)    // claimed_at_unix i64 LE = 1
        + std::string("\x02", 1) + "pu";                        // pool_uuid (length-prefixed, version 2+)
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

// B51: the absent->claim step is now an atomic compare-and-set (condCreateIfAbsent), not a
// read-then-write. Against ONE shared object storage, two first-mounters race: exactly one creates
// the marker and wins; the other loses the CAS, reads the surviving marker, and (allow_shared=false)
// fails closed. The old read-then-write would let the second see "absent" too and co-claim/overwrite.
TEST_F(ContentAddressedMetaTest, PoolMetaConcurrentClaimResolvesViaCAS)
{
    const std::string prefix = "cas_pool_concurrent";
    auto os = getObjectStorage(prefix);

    // "server-a" claims first (creates the marker via CAS).
    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-a", /*allow_shared=*/false, getLogger("test"));
    // "server-b" loses the CAS, reads the existing marker, and fails closed.
    EXPECT_THROW(
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-b", /*allow_shared=*/false, getLogger("test")),
        DB::Exception);

    // The surviving marker names the first claimant — the second never overwrote/co-claimed it.
    EXPECT_EQ(PoolMeta::deserialize(readObject(os, poolMetaKey(prefix))).owner_server_id, "server-a");
}

// The per-mounter registry: every mounter registers itself under poolMountersPrefix, so the live set
// is listable from the bucket alone. With allow_shared=true, "server-a" and "server-b" both register;
// listing the mounters prefix returns both server ids (needed for the multi-mounter milestone).
TEST_F(ContentAddressedMetaTest, PoolMounterRegistryListsAllMounters)
{
    const std::string prefix = "cas_pool_registry";
    auto os = getObjectStorage(prefix);

    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-a", /*allow_shared=*/false, getLogger("test"));
    DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-b", /*allow_shared=*/true, getLogger("test"));

    DB::RelativePathsWithMetadata listed;
    os->listObjects(DB::ContentAddressed::poolMountersPrefix(prefix), listed, /*max_keys=*/0);

    std::set<std::string> ids;
    for (const auto & e : listed)
        ids.insert(e->relative_path.substr(e->relative_path.find_last_of('/') + 1));

    EXPECT_TRUE(ids.contains("server-a")) << "server-a not registered";
    EXPECT_TRUE(ids.contains("server-b")) << "server-b not registered";
}

// CAS replication Phase 1.1: the first claim MINTS a stable, non-empty `pool_uuid`. It is written into
// `_pool_meta`, returned by `claimPoolOwnership`, and round-trips through (de)serialize unchanged.
TEST_F(ContentAddressedMetaTest, PoolUUIDMintedOnFirstClaim)
{
    const std::string prefix = "cas_pool_uuid_mint";
    auto os = getObjectStorage(prefix);

    const std::string returned =
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test"));
    EXPECT_FALSE(returned.empty()) << "first claim must mint a non-empty pool_uuid";

    // The minted value is persisted in the marker and reads back identically (version is the new
    // CURRENT_VERSION = 2, the version that carries pool_uuid in the body).
    auto parsed = PoolMeta::deserialize(readObject(os, poolMetaKey(prefix)));
    EXPECT_EQ(parsed.version, PoolMeta::CURRENT_VERSION);
    EXPECT_EQ(parsed.pool_uuid, returned);
    EXPECT_FALSE(parsed.pool_uuid.empty());
}

// CAS replication Phase 1.1: the `pool_uuid` is STABLE — re-opening the SAME pool (same prefix, same
// server re-mounting, AND a shared mounter with a different server id) reads back the creator's
// pool_uuid and never re-mints it. Two DIFFERENT pool prefixes mint DIFFERENT pool_uuids (so two
// distinct pools can never be mistaken for one — the safety property fetch-by-relink relies on).
TEST_F(ContentAddressedMetaTest, PoolUUIDIsStableAcrossReopenAndDiffersPerPool)
{
    const std::string prefix_a = "cas_pool_uuid_a";
    const std::string prefix_b = "cas_pool_uuid_b";
    auto os_a = getObjectStorage(prefix_a);
    auto os_b = getObjectStorage(prefix_b);

    // First claim mints; the SAME server re-mounting the SAME pool reads back the SAME uuid (never re-mint).
    const std::string uuid_a1 =
        DB::ContentAddressed::claimPoolOwnership(os_a, prefix_a, "server-one", /*allow_shared=*/false, getLogger("test"));
    const std::string uuid_a2 =
        DB::ContentAddressed::claimPoolOwnership(os_a, prefix_a, "server-one", /*allow_shared=*/false, getLogger("test"));
    EXPECT_FALSE(uuid_a1.empty());
    EXPECT_EQ(uuid_a1, uuid_a2) << "re-mount must keep the creator's pool_uuid";

    // A DIFFERENT server mounting the SAME pool as a shared mounter also reads back the creator's uuid
    // (it must not re-mint — that would defeat the shared-pool identity match).
    const std::string uuid_a_shared =
        DB::ContentAddressed::claimPoolOwnership(os_a, prefix_a, "server-two", /*allow_shared=*/true, getLogger("test"));
    EXPECT_EQ(uuid_a1, uuid_a_shared) << "a shared mounter must observe the creator's pool_uuid";

    // The marker on disk still carries the creator's uuid (never rewritten by the shared mounter).
    EXPECT_EQ(PoolMeta::deserialize(readObject(os_a, poolMetaKey(prefix_a))).pool_uuid, uuid_a1);

    // A DIFFERENT pool (distinct prefix) mints a DIFFERENT uuid.
    const std::string uuid_b =
        DB::ContentAddressed::claimPoolOwnership(os_b, prefix_b, "server-one", /*allow_shared=*/false, getLogger("test"));
    EXPECT_FALSE(uuid_b.empty());
    EXPECT_NE(uuid_a1, uuid_b) << "two distinct pools must have distinct pool_uuids";
}

// CAS replication Phase 1.1 (storage getter): the metadata storage exposes the pool's stable identity
// via getPoolUUID after startup (minted on the fresh claim here). The value matches the marker on disk.
TEST_F(ContentAddressedMetaTest, MetadataStorageExposesPoolUUIDAfterStartup)
{
    const std::string prefix = "cas_pool_uuid_storage";
    fs::remove_all("./" + prefix);

    DB::LocalObjectStorageSettings settings("test", "./" + prefix, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    auto ms = std::make_shared<DB::ContentAddressedMetadataStorage>(
        os, /*storage_path_prefix=*/prefix, "server-one", kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/false);

    // Before startup the identity is unresolved.
    EXPECT_TRUE(ms->getPoolUUID().empty());

    ms->startup();
    EXPECT_FALSE(ms->getPoolUUID().empty()) << "startup must resolve the pool_uuid";
    EXPECT_EQ(ms->getPoolUUID(), PoolMeta::deserialize(readObject(os, poolMetaKey(prefix))).pool_uuid);

    ms->shutdown();
    os->shutdown();
    fs::remove_all("./" + prefix);
}

// CAS replication Phase 1.1 (migration / fail-closed): a pool written by the PRE-change code (POOL-content
// version 1, no pool_uuid in the body) must fail closed on this build rather than be silently
// reinterpreted. We synthesize a version-1 marker by serializing the legacy body shape directly. The
// claim path then refuses to mount (unknown POOL-content version), which is the accepted migration story:
// fresh test pools are created each run, so they get version 2; a real pre-existing version-1 pool fails
// closed (no in-place migration in Phase 1).
TEST_F(ContentAddressedMetaTest, PoolMetaLegacyVersionOneFailsClosed)
{
    using namespace DB::ContentAddressed;
    const std::string prefix = "cas_pool_uuid_legacy";
    auto os = getObjectStorage(prefix);

    // Hand-build the legacy version-1 body: magic(4)+enc(1) + version u32 LE = 1 + owner + claimed_at i64,
    // with NO trailing pool_uuid (exactly what the pre-change serializer wrote).
    const std::string legacy =
        std::string("CAPM\x01", 5)
        + std::string("\x01\x00\x00\x00", 4)                    // pool content version u32 LE = 1
        + std::string("\x03", 1) + "old"                        // owner server id (length-prefixed)
        + std::string("\x01\x00\x00\x00\x00\x00\x00\x00", 8);   // claimed_at_unix i64 LE = 1
    writeObject(os, poolMetaKey(prefix), legacy);

    // It parses (the deserializer reads pool_uuid only for version >= 2, so a version-1 body is valid),
    // but the claim path gates the POOL-content version and fails closed on the unknown (old) version.
    EXPECT_EQ(PoolMeta::deserialize(legacy).version, 1u);
    EXPECT_EQ(PoolMeta::deserialize(legacy).pool_uuid, "");
    EXPECT_THROW(
        DB::ContentAddressed::claimPoolOwnership(os, prefix, "server-one", /*allow_shared=*/false, getLogger("test")),
        DB::Exception);
}

// CAS replication Phase 1.2 (N concurrent mounters + union-of-refs): two ContentAddressedMetadataStorage
// instances with DISTINCT server ids share ONE pool (same object storage + prefix), both allow_shared.
// Both claim succeed (no throw); both register in the mounter registry; they observe the SAME pool_uuid;
// each publishes its OWN ref under a disjoint store/<server>/ subtree; and listLivePartIds — the GC live
// set — sees BOTH servers' refs as roots (the cross-replica reference tracking the design gets for free).
// Each also writes the SAME blob idempotently (the second condCreateIfAbsent is a no-op).
TEST_F(ContentAddressedMetaTest, TwoMountersShareOnePoolUnionOfRefsAndSamePoolUUID)
{
    using namespace DB::ContentAddressed;
    const std::string p = "cas_two_mounters_union";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));

    auto make_storage = [&](const std::string & sid)
    {
        return std::make_shared<DB::ContentAddressedMetadataStorage>(
            os, /*storage_path_prefix=*/p, sid, kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    };
    auto a = make_storage("serverA");
    auto b = make_storage("serverB");

    // Both mount succeed (B is NOT rejected — allow_shared is set), and both register in the registry.
    EXPECT_NO_THROW(a->startup());
    EXPECT_NO_THROW(b->startup());
    EXPECT_TRUE(objectExists(os, poolMounterKey(p, "serverA")));
    EXPECT_TRUE(objectExists(os, poolMounterKey(p, "serverB")));

    // Both observe the SAME stable pool_uuid (the creator's; the shared mounter never re-mints).
    EXPECT_FALSE(a->getPoolUUID().empty());
    EXPECT_EQ(a->getPoolUUID(), b->getPoolUUID()) << "two mounters of one pool must share one pool_uuid";

    // Each mounter COMMITS its own part: A writes blob "SHARED" + a private "A-ONLY"; B writes the SAME
    // "SHARED" blob (idempotent dedup) + a private "B-ONLY".
    const std::string uuid_a = "uuid-mount-a";
    const std::string uuid_b = "uuid-mount-b";
    auto commit_part = [&](DB::ContentAddressedMetadataStorage & ms, const std::string & uuid,
                           const std::map<std::string, std::string> & files)
    {
        DB::ContentAddressedTransaction tx(ms, /*key_prefix=*/p, kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    };
    commit_part(*a, uuid_a, {{"a.bin", "A-ONLY"}, {"s.bin", "SHARED"}});
    commit_part(*b, uuid_b, {{"b.bin", "B-ONLY"}, {"s.bin", "SHARED"}});

    // Each ref lives under a DISJOINT store/<server>/ subtree (the per-server ref namespace).
    auto a_refs = listKeysUnder(os, refsPrefix(p, "serverA", uuid_a));
    auto b_refs = listKeysUnder(os, refsPrefix(p, "serverB", uuid_b));
    ASSERT_FALSE(a_refs.empty());
    ASSERT_FALSE(b_refs.empty());
    for (const auto & k : a_refs)
        EXPECT_NE(k.find("store/serverA/"), std::string::npos) << k;
    for (const auto & k : b_refs)
        EXPECT_NE(k.find("store/serverB/"), std::string::npos) << k;

    // The shared "SHARED" column deduplicates to ONE blob object across the two mounters.
    EXPECT_EQ(a->getStorageObjects("uui/" + uuid_a + "/all_1_1_0/s.bin")[0].remote_path,
              b->getStorageObjects("uui/" + uuid_b + "/all_1_1_0/s.bin")[0].remote_path);

    // The GC live set (listLivePartIds) scans the union of ALL servers' refs as roots: it must contain
    // BOTH parts' ids. This is the cross-replica reference tracking the replication design gets for free.
    auto a_pid = a->getStorageObjects("uui/" + uuid_a + "/all_1_1_0/a.bin"); // resolve to ensure part exists
    ASSERT_EQ(a_pid.size(), 1u);
    std::set<PartId> live = listLivePartIds(os, p);
    EXPECT_EQ(live.size(), 2u) << "the union of both mounters' refs must be the live set";

    // A sweep from ONE mounter's perspective (grace huge so nothing is reclaimed) keeps BOTH parts'
    // blobs reachable — neither mounter's data is seen as orphaned by the other's GC.
    auto a_blob = a->getStorageObjects("uui/" + uuid_a + "/all_1_1_0/a.bin")[0].remote_path;
    auto b_blob = b->getStorageObjects("uui/" + uuid_b + "/all_1_1_0/b.bin")[0].remote_path;
    DB::ContentAddressed::ContentAddressedGC gc(os, p);
    gc.runSweepOnce(/*now=*/0, /*grace=*/1000000);
    EXPECT_TRUE(objectExists(os, a_blob)) << "A's blob must be reachable from the union-of-refs scan";
    EXPECT_TRUE(objectExists(os, b_blob)) << "B's blob must be reachable from the union-of-refs scan";

    a->shutdown();
    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
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

// B50: unlinking a table-level file (e.g. a pruned/finished `mutation_N.txt` entry or a stale
// `tmp_mutation_*.txt`) must reclaim its verbatim object immediately. The reachability sweep scans
// only blobs/+parts/, so before the fix unlinkFile's table-level branch was a no-op and the object
// leaked until DROP. After the fix the verbatim object is removed and the file no longer resolves.
TEST_F(ContentAddressedMetaTest, UnlinkTableLevelFileReclaimsVerbatimObject)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_unlink_tf");
    auto os = getObjectStorage("cas_unlink_tf");
    const std::string uuid = "uuid-unlink";
    const std::string path = "uui/" + uuid + "/mutation_9.txt";
    const std::string bytes = "format version: 1\n9\n";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    EXPECT_TRUE(ms->existsFile(path));

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.unlinkFile(path, /*if_exists=*/false, /*should_remove_objects=*/true);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(ms->existsFile(path));
}

// B5 (patch-part lifecycle): a lightweight-DELETE patch part is a normal CA part whose directory name
// carries the `patch-<32hex>-<base_part_name>` prefix (MergeTreePartInfo::PATCH_PART_PREFIX). Its
// blobs must be published under a ref in the table's `refs/` namespace, treated as a reachability root
// by the GC sweep while the ref lives, and reclaimed once the ref is removed — exactly like any other
// part. This test drives the real write/commit path end-to-end.
TEST_F(ContentAddressedMetaTest, PatchPartIsReachableThenReclaimedLikeAnyPart)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_patch_lifecycle");
    auto os = getObjectStorage("cas_patch_lifecycle");
    const std::string uuid = "uuid-patch";
    // Canonical patch-part directory name: patch-<32 hex chars>-<base_part_name>.
    const std::string patch_part = "patch-00000000000000000000000000000000-all_1_1_0";

    // Step 1: write two files through the real transaction and commit.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "PATCHDATA"}, {"_row_exists.bin", "X"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + patch_part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // Step 2: the part must be REACHABLE — its PartId appears in the live-parts set, and
    // getStorageObjects resolves to the backing blob.
    // Read the ref payload directly (refKey + readObject) to get the PartId without relying on the
    // private readRefPartId API; partIdFromRefPayload is the SAME parser the GC live-set scan uses.
    const std::string sid = ms->serverIdForTest();
    const std::string ref_key = refKey("", sid, uuid, patch_part).string();
    ASSERT_TRUE(objectExists(os, ref_key)) << "patch part ref was not written by the transaction";
    PartId pid = partIdFromRefPayload(readObject(os, ref_key));
    EXPECT_TRUE(listLivePartIds(os, "").count(pid)) << "patch part PartId not in live set";

    const std::string data_blob = ms->getStorageObjects("uui/" + uuid + "/" + patch_part + "/data.bin")[0].remote_path;
    ASSERT_TRUE(objectExists(os, data_blob));
    EXPECT_EQ(readObject(os, data_blob), "PATCHDATA");

    // Step 3: a sweep must NOT reclaim blobs while the ref is live.
    {
        DB::ContentAddressed::ContentAddressedGC gc(os, "");
        gc.runSweepOnce(/*now=*/0, /*grace=*/100);
        gc.runSweepOnce(/*now=*/200, /*grace=*/100);
    }
    EXPECT_TRUE(objectExists(os, data_blob))
        << "patch-part data blob was wrongly reclaimed while the ref is live";
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + patch_part + "/data.bin")[0].remote_path), "PATCHDATA");

    // Step 4: remove the patch-part ref, then a sweep past grace must reclaim its blobs.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.removeRecursive("uui/" + uuid + "/" + patch_part, /*should_remove_objects=*/nullptr);
        tx.commit(DB::NoCommitOptions{});
    }
    EXPECT_FALSE(listLivePartIds(os, "").count(pid))
        << "patch part PartId still in live set after ref removal";

    {
        DB::ContentAddressed::ContentAddressedGC gc2(os, "");
        gc2.runSweepOnce(/*now=*/0, /*grace=*/100);
        gc2.runSweepOnce(/*now=*/200, /*grace=*/100);
    }
    EXPECT_FALSE(objectExists(os, data_blob))
        << "patch-part data blob was NOT reclaimed after ref removal and sweep past grace";
}

// M9 W2 / B21 (whole-part clone contract): cloning an entire committed part to a NEW name — the shape
// of ATTACH/REPLACE/FREEZE — carries every source file forward by reference through ONE transaction.
// Because the cloned part has IDENTICAL content, computePartId yields the SAME part_id, so the commit
// publishes ONE ref pointing at the source's existing part_id (no new manifest, no new blob). The B21
// regression this pins: a previous clone path linked only ONE file (the last), corrupting the clone —
// here EVERY file must resolve under the clone and read back its original bytes, and every clone blob
// must be the SAME object as the source's (pure re-reference).
TEST_F(ContentAddressedMetaTest, WholePartClonePublishesOneRefToSourcePartId)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_wholeclone");
    auto os = getObjectStorage("cas_wholeclone");
    const std::string uuid = "uuid-clone";
    const std::string src = "all_1_1_0";
    const std::string dst = "all_2_2_0"; // clone target (ATTACH/REPLACE shape)

    const std::map<std::string, std::string> files{
        {"a.bin", "AAA"}, {"b.bin", "BBB"}, {"c.bin", "CCC"}, {"columns.txt", "a b c"}};

    // Step 1: build the SOURCE part through a transaction + commit.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + src + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // Read the source's part_id via the public ref payload (the same parser the GC live-set scan uses),
    // since readRefPartId is a private resolution helper.
    const std::string sid = ms->serverIdForTest();
    ASSERT_TRUE(objectExists(os, refKey("", sid, uuid, src).string()));
    const PartId src_pid = partIdFromRefPayload(readObject(os, refKey("", sid, uuid, src).string()));

    // Capture each source file's backing blob remote_path so we can prove pure re-reference below.
    std::map<std::string, std::string> src_blob;
    for (const auto & [name, bytes] : files)
    {
        auto objs = ms->getStorageObjects("uui/" + uuid + "/" + src + "/" + name);
        ASSERT_EQ(objs.size(), 1u) << name;
        src_blob[name] = objs[0].remote_path;
    }

    // Step 2: CLONE the whole part to a NEW name through ONE transaction — link EVERY source file
    // forward by reference (this is what DataPartStorageOnDiskBase::freeze does on a CA disk).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
            tx.createHardLinkFrom("uui/" + uuid + "/" + src + "/" + name, "uui/" + uuid + "/" + dst + "/" + name);
        tx.commit(DB::NoCommitOptions{});
    }

    // (a) the clone resolves a SINGLE ref whose part_id EQUALS the source's (identical content → same
    // manifest → same part_id).
    ASSERT_TRUE(objectExists(os, refKey("", sid, uuid, dst).string()));
    const PartId dst_pid = partIdFromRefPayload(readObject(os, refKey("", sid, uuid, dst).string()));
    EXPECT_EQ(dst_pid.string(), src_pid.string());

    // (b) ALL files resolve under the clone and read back their ORIGINAL bytes — not just the last file
    // (the B21 corruption mode was a one-file clone).
    for (const auto & [name, bytes] : files)
    {
        auto objs = ms->getStorageObjects("uui/" + uuid + "/" + dst + "/" + name);
        ASSERT_EQ(objs.size(), 1u) << name;
        EXPECT_EQ(readObject(os, objs[0].remote_path), bytes) << name;
        // (c) each clone file's blob remote_path EQUALS the source's — pure re-reference, no new blob.
        EXPECT_EQ(objs[0].remote_path, src_blob.at(name)) << name;
    }

    // The source part is untouched by the clone.
    EXPECT_EQ(readObject(os, ms->getStorageObjects("uui/" + uuid + "/" + src + "/a.bin")[0].remote_path), "AAA");
}

// CAS M9 W2: ATTACH PARTITION / ATTACH PART publishes an ACTIVE ref out of a DETACHED staging dir.
// ATTACH stages the detached part as detached/attaching_<part>, then renames it to the active part dir
// <uuid>/<active_part>. moveDirectory must publish an active ref whose manifest carries the COMPLETE
// BARE file set (no attaching_/all_X/ prefix) and strip the staging entry from the shared "detached"
// ref. Before the fix this fell through with no branch, so the attached part had no on-disk ref and
// reads threw "ContentAddressed: no ref for .../<active_part>/<file>" (data loss on restart).
TEST_F(ContentAddressedMetaTest, MoveDetachedStagingToActivePublishesActiveRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_attach");
    auto os = getObjectStorage("cas_attach");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-attach";

    const std::map<std::string, std::string> files{
        {"a.bin", "AAA"},
        {"primary.idx", "IDX"},
        {"data.cmrk4", "MRK"},
        {"columns.txt", "cols"},
        {"metadata_version.txt", "0"}};

    // Commit an active part, then move it into detached/<part> (mirror MoveCommittedPartIntoDetached).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : files)
        {
            auto buf = tx.writeFile("uui/" + uuid + "/all_1_2_1/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/all_1_2_1", "uui/" + uuid + "/detached/all_1_2_1");
        tx.commit(DB::NoCommitOptions{});
    }
    // ATTACH renames detached/all_1_2_1 -> detached/attaching_all_1_2_1 (the staging rekey).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/detached/all_1_2_1", "uui/" + uuid + "/detached/attaching_all_1_2_1");
        tx.commit(DB::NoCommitOptions{});
    }
    // The NEW branch: detached/attaching_all_1_2_1 -> active <uuid>/all_2_2_0.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.moveDirectory("uui/" + uuid + "/detached/attaching_all_1_2_1", "uui/" + uuid + "/all_2_2_0");
        tx.commit(DB::NoCommitOptions{});
    }

    // (a) the active ref resolves to a part_id whose manifest lists the COMPLETE BARE file set — every
    // original file (incl. primary.idx / data.cmrk4), with NO attaching_/all_X/ prefix. Resolve through
    // the same public parser the GC live-set scan uses (partIdFromRefPayload), since readRefPartId is a
    // private resolution helper.
    ASSERT_TRUE(objectExists(os, refKey("", sid, uuid, "all_2_2_0").string()));
    const PartId active_pid = partIdFromRefPayload(readObject(os, refKey("", sid, uuid, "all_2_2_0").string()));
    auto active_manifest = PartManifest::deserialize(readObject(os, partKey("", active_pid).string()));
    std::set<std::string> manifest_files;
    for (const auto & [file, entry] : active_manifest.blobs)
        manifest_files.insert(file);
    // metadata_version.txt is a mutable per-part file (sidecar, not the manifest), so the manifest holds
    // the content files only.
    EXPECT_EQ(manifest_files, (std::set<std::string>{"a.bin", "primary.idx", "data.cmrk4", "columns.txt"}));

    // (b) each file resolves via getStorageObjects("<uuid>/all_2_2_0/<file>") and reads back its bytes.
    for (const auto & [name, bytes] : files)
    {
        auto objs = ms->getStorageObjects("uui/" + uuid + "/all_2_2_0/" + name);
        ASSERT_EQ(objs.size(), 1u) << name;
        EXPECT_EQ(readObject(os, objs[0].remote_path), bytes) << name;
    }

    // (c) the shared "detached" ref no longer lists the staging entry (it held exactly one detached part,
    // so it is unlinked entirely).
    EXPECT_FALSE(objectExists(os, refKey("", sid, uuid, "detached").string()));
    auto container = ms->listDirectory("uui/" + uuid + "/detached");
    std::set<std::string> container_got(container.begin(), container.end());
    EXPECT_TRUE(container_got.empty()) << "detached still lists staging entries";
}

// Approach A: a projection's files are stored as nested keys <proj>.proj/<file> in the parent part's
// manifest. The CA metadata storage must recognize the projection subdirectory so loadProjections finds
// it. This seeds a part (top-level files + one projection's nested files) through a transaction and
// asserts the projection dir is discoverable.
TEST_F(ContentAddressedMetaTest, ProjectionSubdirIsDiscoverable)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_exists");
    const std::string uuid = "uuid-proj";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"), std::string("checksums.txt"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_sum.proj/checksums.txt")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "bytes-of-" + f;
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    EXPECT_TRUE(ms->existsDirectory(base + "p_sum.proj"));
    EXPECT_FALSE(ms->existsDirectory(base + "p_absent.proj"));
    EXPECT_FALSE(ms->existsFile(base + "p_sum.proj"));
    EXPECT_TRUE(ms->existsDirectory("uui/" + uuid + "/" + part));
}

// Projection MATERIALIZE / merge stages the projection part under a temporary subdirectory of the
// (not-yet-committed) parent part, <part>/<proj>_<n>.tmp_proj, and renames it to <part>/<proj>.proj
// before the parent part commits (MergeProjectionPartsTask). Content addressing keys the staged
// projection files by their in-part name (<proj>_<n>.tmp_proj/<inner>), so moveDirectory of that staged
// projection dir must re-key them to <proj>.proj/<inner> in the transaction's staged maps; otherwise the
// parent manifest is published with .tmp_proj/ keys and the read path (resolving <proj>.proj/<inner>)
// throws FILE_DOESNT_EXIST. This drives that staged same-part projection-dir rename and asserts the
// committed manifest carries the final <proj>.proj/ keys (and none of the .tmp_proj/ ones).
TEST_F(ContentAddressedMetaTest, StagedProjectionTmpDirRenameRekeysManifest)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_tmp_rename");
    const std::string uuid = "uuid-proj-tmp";
    const std::string tmp_part = "tmp_mut_all_1_2_1_3";
    const std::string final_part = "all_1_2_1_3";
    const std::string tmp_base = "uui/" + uuid + "/" + tmp_part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);

    // Write the parent part's top-level files and the projection files under the temporary
    // <proj>_<n>.tmp_proj subdirectory (mirrors MergeProjectionPartsTask's staging name).
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"), std::string("checksums.txt"),
                           std::string("p_sum_1.tmp_proj/columns.txt"), std::string("p_sum_1.tmp_proj/data.bin"),
                           std::string("p_sum_1.tmp_proj/checksums.txt")})
    {
        auto buf = tx.writeFile(tmp_base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "bytes-of-" + f;
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }

    // Rename the staged projection directory <proj>_<n>.tmp_proj -> <proj>.proj (the materialize merge's
    // final rename), then rename the parent part dir tmp_mut_<part> -> <part> (the part commit rename).
    tx.moveDirectory(tmp_base + "p_sum_1.tmp_proj", tmp_base + "p_sum.proj");
    tx.moveDirectory("uui/" + uuid + "/" + tmp_part, "uui/" + uuid + "/" + final_part);
    tx.commit(DB::NoCommitOptions{});

    const std::string base = "uui/" + uuid + "/" + final_part + "/";

    // The projection is discoverable under its final .proj name; the .tmp_proj name is gone.
    EXPECT_TRUE(ms->existsDirectory(base + "p_sum.proj"));
    EXPECT_FALSE(ms->existsDirectory(base + "p_sum_1.tmp_proj"));

    // Every projection inner file resolves under .proj/, and none remains under .tmp_proj/.
    EXPECT_NO_THROW(ms->getFileSize(base + "p_sum.proj/data.bin"));
    EXPECT_NO_THROW(ms->getFileSize(base + "p_sum.proj/columns.txt"));
    EXPECT_NO_THROW(ms->getFileSize(base + "p_sum.proj/checksums.txt"));
    EXPECT_FALSE(ms->existsFile(base + "p_sum_1.tmp_proj/data.bin"));

    // The inner listing of the projection dir is exactly its files (the .proj/ prefix stripped).
    auto inner = ms->listDirectory(base + "p_sum.proj");
    std::set<std::string> got(inner.begin(), inner.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin", "checksums.txt"}));
}

// listDirectory("<part>/<proj>.proj") returns the projection's INNER file names (the <proj>.proj/
// prefix stripped), so the projection's child DataPartStorage enumerates exactly its own files.
TEST_F(ContentAddressedMetaTest, ProjectionSubdirListsInnerFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_list");
    const std::string uuid = "uuid-proj-list";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_sum.proj/checksums.txt")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    auto inner = ms->listDirectory(base + "p_sum.proj");
    std::set<std::string> got(inner.begin(), inner.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin", "checksums.txt"}));
}

// isDirectoryEmpty reports a projection subdir <part>/<proj>.proj as empty so DiskObjectStorage::removeDirectory
// proceeds straight to the authoritative ref-unlink (the projection's blobs live in the parent part's manifest)
// instead of attempting rmdir on a manifest-derived non-empty listing and throwing CANNOT_RMDIR (B60). Mirrors the
// part-dir-empty branch (B45). A real table dir with live refs stays non-empty so the DROP non-empty-data guard holds.
TEST_F(ContentAddressedMetaTest, ProjectionSubdirReportsEmptyForRemoval)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_empty");
    const std::string uuid = "uuid-proj-empty";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    // The projection subdir reports empty (lets removeDirectory skip the failing rmdir); the part dir itself
    // also reports empty (B45); but the TABLE dir with a live ref stays non-empty (DROP guard intact).
    EXPECT_TRUE(ms->isDirectoryEmpty(base + "p_sum.proj"));
    EXPECT_TRUE(ms->isDirectoryEmpty("uui/" + uuid + "/" + part));
    EXPECT_FALSE(ms->isDirectoryEmpty("uui/" + uuid));
}

// listDirectory("<part>") collapses nested projection keys (<proj>.proj/<file>) to a SINGLE <proj>.proj
// directory entry, alongside top-level files emitted verbatim. This is what iterate()-based projection
// discovery expects and keeps the top-level column listing free of nested projection files.
TEST_F(ContentAddressedMetaTest, PartDirListingCollapsesProjectionToDirEntry)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_collapse");
    const std::string uuid = "uuid-proj-collapse";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_max.proj/data.bin")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    auto names = ms->listDirectory("uui/" + uuid + "/" + part);
    std::set<std::string> got(names.begin(), names.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin", "p_sum.proj", "p_max.proj"}));
}

// The content-addressed part_id hashes the part's full file set, INCLUDING nested projection keys, so a
// part with a projection and the same part WITHOUT it get distinct part_ids (no false dedup), and
// ADD/DROP/MATERIALIZE PROJECTION yields a new part version. (Spec §2.)
TEST(ContentAddressedPartId, IncludesProjectionFiles)
{
    using namespace DB::ContentAddressed;
    auto blob = [](const std::string & s) { return BlobEntry{BlobHash(s), s.size(), BlobHash(s).string()}; };

    std::map<std::string, BlobEntry> base{
        {"columns.txt", blob("c")}, {"data.bin", blob("d")}, {"checksums.txt", blob("k")}};

    std::map<std::string, BlobEntry> with_proj = base;
    with_proj["p_sum.proj/data.bin"] = blob("pd");
    with_proj["p_sum.proj/columns.txt"] = blob("pc");

    EXPECT_NE(computePartId(base), computePartId(with_proj));
    EXPECT_EQ(computePartId(with_proj), computePartId(with_proj));
}

// The projection-subdir gate also covers .tmp_proj (temp projections used by MATERIALIZE-merges, Phase
// 3): existsDirectory finds it and listDirectory lists its inner files, exactly as for .proj.
TEST_F(ContentAddressedMetaTest, TempProjectionSubdirIsRecognized)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_tmp_proj");
    const std::string uuid = "uuid-tmp-proj";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.tmp_proj/columns.txt"), std::string("p_sum.tmp_proj/data.bin")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    EXPECT_TRUE(ms->existsDirectory(base + "p_sum.tmp_proj"));
    auto inner = ms->listDirectory(base + "p_sum.tmp_proj");
    std::set<std::string> got(inner.begin(), inner.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin"}));
    // The parent listing collapses it to a single dir entry too.
    auto names = ms->listDirectory("uui/" + uuid + "/" + part);
    std::set<std::string> top(names.begin(), names.end());
    EXPECT_TRUE(top.count("p_sum.tmp_proj") == 1);
}

// ============================================================================================
// CAS replication Phase 2a: fetch-by-relink (pin-before-publish) at the metadata layer (spec §4).
// ============================================================================================

namespace
{
/// Count the objects directly under blobs/ (the content-blob store) for a pool prefix. Relink must add
/// ZERO new blobs — the proof that it published a ref to ALREADY-PRESENT shared content (re-link) rather
/// than downloading bytes.
size_t countBlobs(const std::shared_ptr<DB::IObjectStorage> & os, const std::string & prefix)
{
    return DB::ContentAddressed::listKeysUnder(os, DB::ContentAddressed::blobsPrefix(prefix)).size();
}

/// Resolve the part_id a published ref names, reading the ref object directly (the read-path/GC-scan
/// parser). Used by the relink tests to learn the part_id server A committed, which server B then relinks.
DB::ContentAddressed::PartId refPartId(
    const std::shared_ptr<DB::IObjectStorage> & os, const std::string & prefix,
    const std::string & sid, const std::string & uuid, const std::string & part)
{
    using namespace DB::ContentAddressed;
    return partIdFromRefPayload(ContentAddressedMetaTest::readObject(os, refKey(prefix, sid, uuid, part).string()));
}
}

// Happy path: server A commits a real part (blobs + parts/<part_id> manifest + its ref). Server B (a
// second metadata storage, distinct server id, SAME pool, allow_shared) relinks the same part_id. B's
// ref resolves, B reads the part's files back correctly, ZERO new blobs were created (relink not
// download), and B's sidecar carries the passed-in mutable values.
TEST_F(ContentAddressedMetaTest, RelinkHappyPathPublishesRefWithoutNewBlobs)
{
    using namespace DB::ContentAddressed;
    const std::string p = "cas_relink_happy";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    auto make_storage = [&](const std::string & sid)
    {
        return std::make_shared<DB::ContentAddressedMetadataStorage>(
            os, /*storage_path_prefix=*/p, sid, kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    };
    auto a = make_storage("serverA");
    auto b = make_storage("serverB");
    a->startup();
    b->startup();

    const std::string uuid = "uuid-relink";
    const std::string part = "all_1_1_0";

    // Server A commits a part (the source another replica wrote).
    {
        DB::ContentAddressedTransaction tx(*a, /*key_prefix=*/p, kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "PAYLOAD"}, {"columns.txt", "a b"}, {"metadata_version.txt", "7"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    const PartId part_id = refPartId(os, p, "serverA", uuid, part);
    const size_t blobs_before = countBlobs(os, p);
    ASSERT_GT(blobs_before, 0u);

    // Server B has NO ref yet for this part.
    EXPECT_FALSE(objectExists(os, refKey(p, "serverB", uuid, part).string()));

    // Server B relinks: publishes its own ref for the same part_id with a fresh mutable set.
    const std::map<std::string, std::string> sidecar{{"uuid.txt", "b-uuid"}, {"metadata_version.txt", "9"}};
    EXPECT_TRUE(b->relinkExistingPart(uuid, part, part_id, sidecar));

    // B's ref resolves to the same part_id and B reads the part's content back correctly.
    EXPECT_TRUE(objectExists(os, refKey(p, "serverB", uuid, part).string()));
    EXPECT_EQ(refPartId(os, p, "serverB", uuid, part), part_id);
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid + "/" + part + "/data.bin")[0].remote_path), "PAYLOAD");
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid + "/" + part + "/columns.txt")[0].remote_path), "a b");

    // ZERO new blobs — the relink published a ref to already-present content, it did NOT download bytes.
    EXPECT_EQ(countBlobs(os, p), blobs_before) << "relink must create no new blobs";

    // B's sidecar carries the passed-in mutable values (its OWN per-part state, distinct from A's).
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid + "/" + part + "/uuid.txt")[0].remote_path), "b-uuid");
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid + "/" + part + "/metadata_version.txt")[0].remote_path), "9");

    // The pin is released: no leftover write-session object after a successful relink.
    EXPECT_TRUE(listKeysUnder(os, sessionsPrefix(p)).empty()) << "relink must release its pin on success";

    a->shutdown();
    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
}

// THE RACE (the data-loss hole the adversarial review found). Server A commits the source part. Server B
// begins a relink: it PINS (step 1) and RE-VALIDATES (step 2). BETWEEN B's pin and B's ref publish we
// simulate the hazard: (i) drop server A's ref (the part_id is now named by NO ref), (ii) run a GC sweep
// with grace=0. The sweep MUST NOT reclaim the part's blobs — B's live WriteSession pins them. Then B
// publishes its ref and the part still reads. This proves the pin closes the window.
TEST_F(ContentAddressedMetaTest, RelinkPinSurvivesConcurrentSourceDropAndSweep)
{
    using namespace DB::ContentAddressed;
    const std::string p = "cas_relink_race";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    auto make_storage = [&](const std::string & sid)
    {
        return std::make_shared<DB::ContentAddressedMetadataStorage>(
            os, /*storage_path_prefix=*/p, sid, kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    };
    auto a = make_storage("serverA");
    auto b = make_storage("serverB");
    a->startup();
    b->startup();

    const std::string uuid = "uuid-race";
    const std::string part = "all_1_1_0";

    {
        DB::ContentAddressedTransaction tx(*a, /*key_prefix=*/p, kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{{"data.bin", "RACE-PAYLOAD"}, {"columns.txt", "c"}})
        {
            auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    const PartId part_id = refPartId(os, p, "serverA", uuid, part);
    const std::string data_blob = a->getStorageObjects("uui/" + uuid + "/" + part + "/data.bin")[0].remote_path;
    ASSERT_TRUE(objectExists(os, data_blob));

    // STEP 1 — B PINS the existing part's blob set (durable WriteSession), and RE-VALIDATES (step 2).
    auto pin = b->relinkPin(part_id);
    ASSERT_TRUE(pin.has_value());
    ASSERT_TRUE(b->relinkRevalidate(part_id));
    // Exactly one live write-session pins the part's blobs (B's relink pin).
    ASSERT_EQ(listKeysUnder(os, sessionsPrefix(p)).size(), 1u);

    // BETWEEN B's pin and B's publish, the hazard fires:
    //   (i) server A drops its ref -> the part_id is now named by NO ref.
    os->removeObjectIfExists(DB::StoredObject(refKey(p, "serverA", uuid, part).string()));
    EXPECT_TRUE(listLivePartIds(os, p).empty()) << "no ref names the part_id anymore";

    //   (ii) a GC sweep runs with grace=0 (any TRULY orphaned object would be reclaimed this round).
    {
        const int64_t now = 1; // below B's 300s lease so its session is LIVE
        auto held = tryAcquireGcLock(*os, p, "serverGc", /*lease_seconds=*/100, /*now_unix=*/static_cast<uint64_t>(now));
        ASSERT_TRUE(held.has_value());
        DB::ContentAddressed::ContentAddressedGC gc(os, p);
        gc.runSweepOnce(now, /*grace=*/0, /*held=*/held);
        releaseGcLock(*os, p, *held);
    }

    // The pin held: the part's blobs and manifest were NOT reclaimed despite no ref naming them.
    EXPECT_TRUE(objectExists(os, data_blob)) << "B's live relink pin must keep the blob across the sweep";
    EXPECT_TRUE(objectExists(os, partKey(p, part_id).string())) << "the manifest must survive too";

    // STEP 3 — B publishes its ref; STEP 4 — release the pin. The part now reads back from B.
    b->relinkPublishRef(uuid, part, part_id, /*sidecar_values=*/{});
    b->relinkReleasePin(*pin);
    EXPECT_TRUE(objectExists(os, refKey(p, "serverB", uuid, part).string()));
    EXPECT_EQ(readObject(os, b->getStorageObjects("uui/" + uuid + "/" + part + "/data.bin")[0].remote_path), "RACE-PAYLOAD");

    a->shutdown();
    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
}

// Reclaim: after a relink, dropping BOTH A's and B's refs and sweeping with grace=0 reclaims the part's
// blobs and manifest — no leftovers once no replica references the part_id (the union-of-refs invariant).
TEST_F(ContentAddressedMetaTest, RelinkReclaimedWhenBothRefsDropped)
{
    using namespace DB::ContentAddressed;
    const std::string p = "cas_relink_reclaim";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    auto make_storage = [&](const std::string & sid)
    {
        return std::make_shared<DB::ContentAddressedMetadataStorage>(
            os, /*storage_path_prefix=*/p, sid, kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    };
    auto a = make_storage("serverA");
    auto b = make_storage("serverB");
    a->startup();
    b->startup();

    const std::string uuid = "uuid-reclaim";
    const std::string part = "all_1_1_0";
    {
        DB::ContentAddressedTransaction tx(*a, /*key_prefix=*/p, kCasTestScratch);
        auto buf = tx.writeFile("uui/" + uuid + "/" + part + "/data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "RECLAIM-PAYLOAD";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const PartId part_id = refPartId(os, p, "serverA", uuid, part);
    const std::string data_blob = a->getStorageObjects("uui/" + uuid + "/" + part + "/data.bin")[0].remote_path;

    ASSERT_TRUE(b->relinkExistingPart(uuid, part, part_id, /*sidecar_values=*/{}));

    // Drop BOTH refs: no replica references the part_id anymore.
    os->removeObjectIfExists(DB::StoredObject(refKey(p, "serverA", uuid, part).string()));
    os->removeObjectIfExists(DB::StoredObject(refKey(p, "serverB", uuid, part).string()));
    EXPECT_TRUE(listLivePartIds(os, p).empty());

    // Sweep with grace=0 reclaims the now-unreferenced blob and manifest (no leftovers).
    {
        const int64_t now = 1000000; // past any session lease (there is none — relink released its pin)
        auto held = tryAcquireGcLock(*os, p, "serverGc", /*lease_seconds=*/100, /*now_unix=*/static_cast<uint64_t>(now));
        ASSERT_TRUE(held.has_value());
        DB::ContentAddressed::ContentAddressedGC gc(os, p);
        auto stats = gc.runSweepOnce(now, /*grace=*/0, /*held=*/held);
        releaseGcLock(*os, p, *held);
        EXPECT_GE(stats.deleted_blobs, 1u);
        EXPECT_GE(stats.deleted_parts, 1u);
    }
    EXPECT_FALSE(objectExists(os, data_blob)) << "the blob must be reclaimed once no ref references it";
    EXPECT_FALSE(objectExists(os, partKey(p, part_id).string())) << "the manifest must be reclaimed too";

    a->shutdown();
    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
}

// Missing blob -> no dangling ref. A parts/<part_id> manifest names a blob that is NOT present. A relink
// MUST NOT publish a ref (it returns "not possible" so the caller falls back to a byte fetch), and it
// leaves no ref behind and releases its pin. Never publish a ref to a missing blob.
TEST_F(ContentAddressedMetaTest, RelinkMissingBlobPublishesNoRef)
{
    using namespace DB::ContentAddressed;
    const std::string p = "cas_relink_missing";
    fs::remove_all("./" + p);

    DB::LocalObjectStorageSettings settings("test", "./" + p, /*read_only_=*/false);
    auto os = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
    auto b = std::make_shared<DB::ContentAddressedMetadataStorage>(
        os, /*storage_path_prefix=*/p, "serverB", kCasTestScratch, /*context=*/nullptr, /*allow_shared_pool=*/true);
    b->startup();

    const std::string uuid = "uuid-missing";
    const std::string part = "all_1_1_0";

    // Seed a manifest naming a blob that is NOT present in blobs/ (no put_blob for it).
    const std::string pid = "abcdef0000000000000000000000abcd";
    PartManifest f;
    f.blobs["data.bin"] = BlobEntry{BlobHash("deadbeefdead"), 5, "deadbeefdead"};
    writeObject(os, partKey(p, PartId(pid)).string(), f.serialize());

    // The relink must NOT publish a ref (it returns false — fall back to byte fetch).
    EXPECT_FALSE(b->relinkExistingPart(uuid, part, PartId(pid), /*sidecar_values=*/{}));

    // No ref was left behind, and the pin was released (no dangling ref, no leftover session).
    EXPECT_FALSE(objectExists(os, refKey(p, "serverB", uuid, part).string())) << "no ref to a missing blob";
    EXPECT_TRUE(listKeysUnder(os, sessionsPrefix(p)).empty()) << "the pin must be released on the not-possible path";

    b->shutdown();
    os->shutdown();
    fs::remove_all("./" + p);
}

// B59: a CA part-build transaction can read a part file it staged but has NOT committed (read-your-writes).
// The blob is uploaded as soon as the write buffer finalizes; only the ref/manifest commit is deferred.
TEST_F(ContentAddressedMetaTest, InFlightReadYourWritesBeforeCommit)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_inflight");
    const std::string uuid = "uuid-inflight";
    const std::string part = "all_1_1_0";
    const std::string col = "uui/" + uuid + "/" + part + "/data.bin";
    const std::string bytes = "INFLIGHT-COLUMN-BYTES";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    {
        auto buf = tx.writeFile(col, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    // NOT committed yet: the committed read path can't see it.
    EXPECT_FALSE(ms->existsFile(col));
    // But the transaction resolves its own staged file.
    auto objs = tx.tryGetInFlightStorageObjects(col);
    ASSERT_TRUE(objs.has_value());
    ASSERT_EQ(objs->size(), 1u);
    EXPECT_EQ((*objs)[0].remote_path.rfind("blobs/", 0), 0u); // a content-addressed blob key
    EXPECT_EQ(tx.tryGetInFlightFileSize(col), std::optional<uint64_t>(bytes.size()));
    auto rb = tx.tryReadFileInFlight(col, DB::getReadSettings(), std::nullopt);
    ASSERT_NE(rb, nullptr);
    String got; DB::readStringUntilEOF(got, *rb);
    EXPECT_EQ(got, bytes);
    // A file this transaction never wrote → nullopt.
    EXPECT_FALSE(tx.tryGetInFlightStorageObjects("uui/" + uuid + "/" + part + "/absent.bin").has_value());
    tx.commit(DB::NoCommitOptions{});
    EXPECT_TRUE(ms->existsFile(col)); // now committed
}

// FREEZE path: writing a FREEZE target (shadow/<backup>/…) must publish the ref at the shadow/ key,
// not at the live store/.../refs/ location. The live ref must be completely unchanged; the shadow ref
// and the live ref resolve to the SAME part_id (content-only id is identical for identical bytes) but
// they live at distinct keys so neither clobbers the other.
TEST_F(ContentAddressedMetaTest, FreezePublishesShadowRefWithoutClobberingLiveRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_freeze_shadow");
    auto os = getObjectStorage("cas_freeze_shadow");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-freeze";
    const std::string part = "all_1_1_0";
    const std::string backup = "b1";

    // Determine the uuid[:3] prefix used in disk-relative paths (standard ClickHouse layout).
    const std::string uuid3 = uuid.substr(0, 3);

    // === 1. Write the LIVE part. ===
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "COL-BYTES"}, {"columns.txt", "a b"}, {"metadata_version.txt", "1"}})
        {
            const std::string path = uuid3 + "/" + uuid + "/" + part + "/" + name;
            auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // Capture the live ref key and its part_id BEFORE the freeze.
    const std::string live_ref_key = refKey("", sid, uuid, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(live_ref_key, /*with_tags=*/false).has_value())
        << "live ref must exist after the first commit";
    const std::string live_ref_bytes_before = readObject(os, live_ref_key);
    const PartId live_pid = partIdFromRefPayload(live_ref_bytes_before);
    ASSERT_FALSE(live_pid.string().empty());

    // === 2. Write the FREEZE (shadow) copy of the SAME part with IDENTICAL content. ===
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "COL-BYTES"}, {"columns.txt", "a b"}, {"metadata_version.txt", "1"}})
        {
            // FREEZE writes to shadow/<backup>/<uuid[:3]>/<uuid>/<part>/<file>.
            const std::string path = "shadow/" + backup + "/" + uuid3 + "/" + uuid + "/" + part + "/" + name;
            auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // === 3. Assertions. ===

    // 3a. The live ref at refKey(...) is INTACT and UNCHANGED — not clobbered by the freeze.
    ASSERT_TRUE(os->tryGetObjectMetadata(live_ref_key, /*with_tags=*/false).has_value())
        << "live ref must still exist after the freeze commit";
    const std::string live_ref_bytes_after = readObject(os, live_ref_key);
    EXPECT_EQ(live_ref_bytes_before, live_ref_bytes_after)
        << "freeze must not alter the live ref bytes";

    // 3b. A shadow ref now exists at shadowRefKey(...) and resolves to the SAME part_id (identical
    // content means identical content-only part_id — deduplication via the blob hash chain).
    // The shadow ref-family keys mirror the physical store tree: the shadow table dir is everything
    // before the part component (shadow/<backup>/<uuid[:3]>/<uuid>).
    const std::string shadow_table_dir = "shadow/" + backup + "/" + uuid3 + "/" + uuid;
    const std::string shadow_ref_key = shadowRefKey("", shadow_table_dir, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(shadow_ref_key, /*with_tags=*/false).has_value())
        << "shadow ref must be published by the freeze commit";
    const PartId shadow_pid = partIdFromRefPayload(readObject(os, shadow_ref_key));
    EXPECT_EQ(live_pid, shadow_pid)
        << "identical content must produce the same content-only part_id in the shadow ref";

    // 3c. The shadow ref key and the live ref key are distinct (no aliasing).
    EXPECT_NE(live_ref_key, shadow_ref_key)
        << "live and shadow ref keys must be at distinct object-storage paths";
}

// GC reachability: shadow/ refs are GC roots — a frozen snapshot's blobs must remain reachable even
// after the live part is merged/dropped (its store/ ref removed). Without this, the GC would reclaim
// the blobs the moment the live ref disappeared, breaking the FREEZE guarantee.
//
// The test walks three states:
//   A. both refs present   → part_id is live (trivially)
//   B. only shadow ref     → part_id is STILL live (this is the key invariant)
//   C. no refs at all      → part_id is gone (reachability is correctly bounded)
TEST_F(ContentAddressedMetaTest, GcShadowRefIsAGcRoot)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_gc_shadow_root");
    auto os = getObjectStorage("cas_gc_shadow_root");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-gc-shadow";
    const std::string part = "all_1_1_0";
    const std::string backup = "b1";
    const std::string uuid3 = uuid.substr(0, 3);

    // === 1. Write the live part. ===
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "COL-BYTES"}, {"columns.txt", "a b"}, {"metadata_version.txt", "1"}})
        {
            auto buf = tx.writeFile(uuid3 + "/" + uuid + "/" + part + "/" + name, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    // Capture the live ref key and the part_id it names.
    const std::string live_ref_key = refKey("", sid, uuid, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(live_ref_key, /*with_tags=*/false).has_value());
    const PartId pid = partIdFromRefPayload(readObject(os, live_ref_key));

    // === 2. Freeze (publish a shadow ref for the same part). ===
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        for (const auto & [name, bytes] : std::map<std::string, std::string>{
                 {"data.bin", "COL-BYTES"}, {"columns.txt", "a b"}, {"metadata_version.txt", "1"}})
        {
            const std::string path = "shadow/" + backup + "/" + uuid3 + "/" + uuid + "/" + part + "/" + name;
            auto buf = tx.writeFile(path, 4096, DB::WriteMode::Rewrite, {});
            buf->write(bytes.data(), bytes.size());
            buf->finalize();
        }
        tx.commit(DB::NoCommitOptions{});
    }

    const std::string shadow_table_dir = "shadow/" + backup + "/" + uuid3 + "/" + uuid;
    const std::string shadow_key = shadowRefKey("", shadow_table_dir, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(shadow_key, /*with_tags=*/false).has_value())
        << "shadow ref must exist after the freeze commit";

    // === State A: both refs present — part_id appears in the live set. ===
    {
        std::set<PartId> live = listLivePartIds(os, "");
        EXPECT_TRUE(live.count(pid)) << "State A: part_id must be in live set when both refs exist";
    }

    // === State B: remove the live (store/) ref, keep only the shadow ref. ===
    // This simulates the live part being merged/dropped after the FREEZE snapshot was taken.
    os->removeObjectIfExists(DB::StoredObject(live_ref_key));
    ASSERT_FALSE(os->tryGetObjectMetadata(live_ref_key, /*with_tags=*/false).has_value())
        << "live ref must be gone";

    {
        std::set<PartId> live = listLivePartIds(os, "");
        EXPECT_TRUE(live.count(pid))
            << "State B: part_id must still be in live set when only the shadow ref exists "
               "(frozen blobs must not be reclaimed after the live part is gone)";
    }

    // === State C: remove the shadow ref too — the part_id must now be absent. ===
    os->removeObjectIfExists(DB::StoredObject(shadow_key));
    ASSERT_FALSE(os->tryGetObjectMetadata(shadow_key, /*with_tags=*/false).has_value())
        << "shadow ref must be gone";

    {
        std::set<PartId> live = listLivePartIds(os, "");
        EXPECT_FALSE(live.count(pid))
            << "State C: part_id must be gone from the live set once all refs are removed";
    }
}

// B59: the in-flight read of a MUTABLE per-part file (e.g. metadata_version.txt) — staged inline in
// recorded_mutable, NOT as a content blob — is served as the inline bytes. It has no StoredObject, so
// tryGetInFlightStorageObjects returns nullopt while tryReadFileInFlight returns the bytes.
TEST_F(ContentAddressedMetaTest, InFlightReadYourWritesMutableFileBeforeCommit)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_inflight_mutable");
    const std::string uuid = "uuid-inflight-mut";
    const std::string part = "all_1_1_0";
    const std::string mut = "uui/" + uuid + "/" + part + "/metadata_version.txt";
    const std::string bytes = "42"; // a small mutable per-part value

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    {
        auto buf = tx.writeFile(mut, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    // A mutable file has no blob object → no StoredObjects in-flight (must be read via tryReadFileInFlight).
    EXPECT_FALSE(tx.tryGetInFlightStorageObjects(mut).has_value());
    // But its size and inline bytes are resolvable before commit.
    EXPECT_EQ(tx.tryGetInFlightFileSize(mut), std::optional<uint64_t>(bytes.size()));
    auto rb = tx.tryReadFileInFlight(mut, DB::getReadSettings(), std::nullopt);
    ASSERT_NE(rb, nullptr);
    String got; DB::readStringUntilEOF(got, *rb);
    EXPECT_EQ(got, bytes);
}

// ============================================================================================
// TXN-T3: replaceFile + mutable-only sidecar update
// ============================================================================================

// Core invariant: writing txn_version.txt via the tmp+replaceFile sequence on a COMMITTED part
// updates only the per-ref sidecar (the mutable-only commit branch) and KEEPS the existing
// manifest, part_id and ref. The live content blobs are never affected, and the sidecar now
// holds exactly the written bytes under "txn_version.txt".
TEST_F(ContentAddressedMetaTest, TxnVersionRewriteKeepsManifestAndRef)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_txnrewrite");
    auto os = getObjectStorage("cas_txnrewrite");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-txnrewrite";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    // 1. Write and commit a live part with a content file.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data = "COLUMN-DATA";
        buf->write(data.data(), data.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }

    // Capture the part_id and the manifest object key BEFORE the mutable-only update.
    const std::string ref_key = refKey("", sid, uuid, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_key, /*with_tags=*/false).has_value())
        << "ref must exist after first commit";
    const PartId pid_before = partIdFromRefPayload(readObject(os, ref_key));
    const std::string manifest_bytes_before = readObject(os, partKey("", pid_before).string());
    const PartManifest manifest_before = PartManifest::deserialize(manifest_bytes_before);
    EXPECT_TRUE(manifest_before.blobs.contains("data.bin"))
        << "manifest must contain the content file";
    EXPECT_FALSE(manifest_before.blobs.contains("txn_version.txt"))
        << "manifest must NOT contain txn_version.txt before the mutable-only write";
    EXPECT_FALSE(manifest_before.blobs.contains("txn_version.txt.tmp"))
        << "manifest must NOT contain the tmp form either";

    // 2. Simulate VersionMetadataOnDisk::storeInfoToDataPartStorage: write .tmp then replaceFile.
    const std::string txn_bytes = "42";
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        buf->write(txn_bytes.data(), txn_bytes.size());
        buf->finalize();
        tx.replaceFile(base + "txn_version.txt.tmp", base + "txn_version.txt");
        tx.commit(DB::NoCommitOptions{});
    }

    // 3. The ref still resolves to the SAME part_id (mutable-only commit must not clobber the ref).
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_key, /*with_tags=*/false).has_value())
        << "ref must still exist after the mutable-only commit";
    const PartId pid_after = partIdFromRefPayload(readObject(os, ref_key));
    EXPECT_EQ(pid_before, pid_after)
        << "mutable-only commit must not change the part_id / ref";

    // 4. The manifest is UNCHANGED.
    const std::string manifest_bytes_after = readObject(os, partKey("", pid_after).string());
    EXPECT_EQ(manifest_bytes_before, manifest_bytes_after)
        << "manifest must be byte-for-byte unchanged after the mutable-only commit";
    const PartManifest manifest_after = PartManifest::deserialize(manifest_bytes_after);
    EXPECT_FALSE(manifest_after.blobs.contains("txn_version.txt"))
        << "txn_version.txt must NOT appear in the manifest after the mutable-only commit";
    EXPECT_FALSE(manifest_after.blobs.contains("txn_version.txt.tmp"))
        << "txn_version.txt.tmp must NOT appear in the manifest";

    // 5. The sidecar now carries txn_version.txt with exactly the written bytes.
    const std::string meta_key = refMetaKey("", sid, uuid, part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(meta_key, /*with_tags=*/false).has_value())
        << "sidecar bundle object must exist after the mutable-only commit";
    const RefSidecar sidecar = RefSidecar::deserialize(readObject(os, meta_key));
    ASSERT_TRUE(sidecar.files.contains("txn_version.txt"))
        << "sidecar must contain txn_version.txt";
    EXPECT_EQ(sidecar.files.at("txn_version.txt"), txn_bytes)
        << "sidecar must hold exactly the bytes written to txn_version.txt";
    EXPECT_FALSE(sidecar.files.contains("txn_version.txt.tmp"))
        << "the tmp form must not appear in the sidecar (it was the source of the replaceFile)";

    // 6. Reading back the mutable file through the metadata storage returns the same bytes.
    auto txn_objs = ms->getStorageObjects(base + "txn_version.txt");
    ASSERT_EQ(txn_objs.size(), 1u);
    EXPECT_EQ(readObject(os, txn_objs[0].remote_path), txn_bytes);

    // 7. The content file still resolves via the manifest -> blob path (unchanged).
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "data.bin")[0].remote_path), "COLUMN-DATA");
}

// A SECOND mutable-only commit with different txn_version.txt bytes overwrites the sidecar in
// place. The ref and part_id stay unchanged after both rewrites; only the sidecar bytes change.
TEST_F(ContentAddressedMetaTest, TxnVersionOverwrite)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_txnoverwrite");
    auto os = getObjectStorage("cas_txnoverwrite");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-txnoverwrite";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    // Write and commit the live part.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data = "ROWS";
        buf->write(data.data(), data.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string ref_key = refKey("", sid, uuid, part).string();
    const PartId pid_original = partIdFromRefPayload(readObject(os, ref_key));

    // First mutable-only commit: write txn_version.txt = "10".
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        const std::string v = "10";
        buf->write(v.data(), v.size());
        buf->finalize();
        tx.replaceFile(base + "txn_version.txt.tmp", base + "txn_version.txt");
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string meta_key = refMetaKey("", sid, uuid, part).string();
    // Sidecar carries "10"; part_id unchanged.
    {
        const PartId pid = partIdFromRefPayload(readObject(os, ref_key));
        EXPECT_EQ(pid, pid_original);
        ASSERT_TRUE(os->tryGetObjectMetadata(meta_key, /*with_tags=*/false).has_value());
        const RefSidecar sc1 = RefSidecar::deserialize(readObject(os, meta_key));
        EXPECT_EQ(sc1.files.at("txn_version.txt"), "10");
    }

    // Second mutable-only commit: overwrite with "99".
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        const std::string v = "99";
        buf->write(v.data(), v.size());
        buf->finalize();
        tx.replaceFile(base + "txn_version.txt.tmp", base + "txn_version.txt");
        tx.commit(DB::NoCommitOptions{});
    }
    // After the second update the sidecar holds "99" and the part_id is still unchanged.
    {
        const PartId pid = partIdFromRefPayload(readObject(os, ref_key));
        EXPECT_EQ(pid, pid_original)
            << "part_id must be unchanged after two mutable-only commits";
        ASSERT_TRUE(os->tryGetObjectMetadata(meta_key, /*with_tags=*/false).has_value());
        const RefSidecar sc2 = RefSidecar::deserialize(readObject(os, meta_key));
        EXPECT_EQ(sc2.files.at("txn_version.txt"), "99")
            << "second mutable-only commit must overwrite the first";
        // The content file still resolves correctly.
        EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "data.bin")[0].remote_path), "ROWS");
    }
}

// Fail-closed: a mutable-only commit (replaceFile of txn_version.txt) on a part that was NEVER
// committed (no ref) must throw LOGICAL_ERROR. Publishing a sidecar update to a non-existent ref
// would leave a dangling sidecar with no manifest; the commit detects the absence of an existing
// ref and throws before writing anything.
TEST_F(ContentAddressedMetaTest, MutableOnlyCommitWithoutRefThrows)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mut_nothrow");
    const std::string uuid = "uuid-mut-nothrow";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    // Build a transaction that stages only a mutable file (no content file -> no ref will exist).
    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    {
        auto buf = tx.writeFile(base + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        const std::string v = "77";
        buf->write(v.data(), v.size());
        buf->finalize();
    }
    tx.replaceFile(base + "txn_version.txt.tmp", base + "txn_version.txt");
    // No ref for this part exists — commit must throw.
    EXPECT_THROW(tx.commit(DB::NoCommitOptions{}), DB::Exception);
}

// RemoveTmpMutableFile: after a successful mutable-only commit, a transaction that removes the
// tmp mutable file (unlinkFile on txn_version.txt.tmp — MergeTree's removeTmpMetadataFile) on the
// same committed part commits cleanly. The sidecar loses the ".tmp" entry (if any) but keeps the
// non-tmp txn_version.txt entry written by the prior commit; the ref and part_id are unchanged.
TEST_F(ContentAddressedMetaTest, RemoveTmpMutableFile)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_rmtmp");
    auto os = getObjectStorage("cas_rmtmp");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-rmtmp";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    // Write a live part.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data = "PAYLOAD";
        buf->write(data.data(), data.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string ref_key = refKey("", sid, uuid, part).string();
    const PartId pid_original = partIdFromRefPayload(readObject(os, ref_key));

    // Write txn_version.txt via the tmp+replaceFile sequence.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        const std::string v = "5";
        buf->write(v.data(), v.size());
        buf->finalize();
        tx.replaceFile(base + "txn_version.txt.tmp", base + "txn_version.txt");
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string meta_key = refMetaKey("", sid, uuid, part).string();
    // Confirm the sidecar has txn_version.txt = "5".
    {
        ASSERT_TRUE(os->tryGetObjectMetadata(meta_key, /*with_tags=*/false).has_value());
        const RefSidecar sc = RefSidecar::deserialize(readObject(os, meta_key));
        EXPECT_EQ(sc.files.at("txn_version.txt"), "5");
    }

    // MergeTree calls removeTmpMetadataFile → unlinkFile(txn_version.txt.tmp) on the committed part.
    // This is a mutable-only removal (the .tmp is recognized by isMutablePerPartFile). Commit must
    // succeed cleanly and the sidecar must NOT gain a ".tmp" entry.
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        tx.unlinkFile(base + "txn_version.txt.tmp", /*if_exists=*/true, /*should_remove_objects=*/false);
        tx.commit(DB::NoCommitOptions{});
    }

    // The ref and part_id are unchanged.
    EXPECT_EQ(partIdFromRefPayload(readObject(os, ref_key)), pid_original);

    // The sidecar no longer has the ".tmp" entry; it does still have "txn_version.txt".
    ASSERT_TRUE(os->tryGetObjectMetadata(meta_key, /*with_tags=*/false).has_value());
    const RefSidecar sc_final = RefSidecar::deserialize(readObject(os, meta_key));
    EXPECT_FALSE(sc_final.files.contains("txn_version.txt.tmp"))
        << "sidecar must not contain the tmp form after the remove";
    EXPECT_EQ(sc_final.files.at("txn_version.txt"), "5")
        << "the non-tmp txn_version.txt must remain in the sidecar";

    // The content file is still resolvable.
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base + "data.bin")[0].remote_path), "PAYLOAD");
}

// MULTI-PART (B67): ONE ContentAddressedTransaction stages two distinct NEW content parts (A and B,
// each its own (uuid, part)) AND a mutable-only update (txn_version.txt) to a THIRD, already-committed
// part C. commit() iterates the per-part staging map under one gc_lock, calling commitOnePart per part.
// Assert: A and B each publish a fresh ref + manifest carrying their own content; C's ref / part_id /
// manifest are UNCHANGED (the mutable-only branch must not clobber the committed part) and C's sidecar
// now carries the new txn_version.txt bytes. This is the core of the single-transaction merge/mutation
// shape the per-part refactor exists to support.
TEST_F(ContentAddressedMetaTest, MultiPartCommitPublishesAllParts)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mpt_all");
    auto os = getObjectStorage("cas_mpt_all");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-mpt-all";
    const std::string part_a = "all_1_1_0";
    const std::string part_b = "all_2_2_0";
    const std::string part_c = "all_3_3_0";
    const std::string base_a = "uui/" + uuid + "/" + part_a + "/";
    const std::string base_b = "uui/" + uuid + "/" + part_b + "/";
    const std::string base_c = "uui/" + uuid + "/" + part_c + "/";

    // 1. Pre-commit part C (it must already have a ref for the mutable-only branch to update, not throw).
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
        auto buf = tx.writeFile(base_c + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string data = "C-DATA";
        buf->write(data.data(), data.size());
        buf->finalize();
        tx.commit(DB::NoCommitOptions{});
    }
    const std::string ref_c_key = refKey("", sid, uuid, part_c).string();
    const PartId pid_c_before = partIdFromRefPayload(readObject(os, ref_c_key));
    const std::string manifest_c_bytes_before = readObject(os, partKey("", pid_c_before).string());

    // 2. ONE transaction: write content for A and B, AND a mutable-only update to the committed C.
    const std::string txn_bytes = "777";
    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);

        auto a_data = tx.writeFile(base_a + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string adata = "A-DATA";
        a_data->write(adata.data(), adata.size());
        a_data->finalize();
        auto a_cols = tx.writeFile(base_a + "columns.txt", 4096, DB::WriteMode::Rewrite, {});
        const std::string acols = "a b";
        a_cols->write(acols.data(), acols.size());
        a_cols->finalize();

        auto b_data = tx.writeFile(base_b + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string bdata = "B-DATA";
        b_data->write(bdata.data(), bdata.size());
        b_data->finalize();

        // Mutable-only update to the already-committed C (the tmp + replaceFile dance).
        auto c_txn = tx.writeFile(base_c + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        c_txn->write(txn_bytes.data(), txn_bytes.size());
        c_txn->finalize();
        tx.replaceFile(base_c + "txn_version.txt.tmp", base_c + "txn_version.txt");

        tx.commit(DB::NoCommitOptions{});
    }

    // 3. A and B each resolve to a published ref + manifest carrying their own content.
    const std::string ref_a_key = refKey("", sid, uuid, part_a).string();
    const std::string ref_b_key = refKey("", sid, uuid, part_b).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_a_key, /*with_tags=*/false).has_value())
        << "part A ref must exist after the multi-part commit";
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_b_key, /*with_tags=*/false).has_value())
        << "part B ref must exist after the multi-part commit";

    const PartId pid_a = partIdFromRefPayload(readObject(os, ref_a_key));
    const PartId pid_b = partIdFromRefPayload(readObject(os, ref_b_key));
    const PartManifest manifest_a = PartManifest::deserialize(readObject(os, partKey("", pid_a).string()));
    const PartManifest manifest_b = PartManifest::deserialize(readObject(os, partKey("", pid_b).string()));
    EXPECT_TRUE(manifest_a.blobs.contains("data.bin"));
    EXPECT_TRUE(manifest_a.blobs.contains("columns.txt"));
    EXPECT_TRUE(manifest_b.blobs.contains("data.bin"));

    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_a + "data.bin")[0].remote_path), "A-DATA");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_a + "columns.txt")[0].remote_path), "a b");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_b + "data.bin")[0].remote_path), "B-DATA");

    // 4. C's ref / part_id / manifest are UNCHANGED (the mutable-only branch must not clobber the part).
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_c_key, /*with_tags=*/false).has_value())
        << "part C ref must still exist after the multi-part commit";
    const PartId pid_c_after = partIdFromRefPayload(readObject(os, ref_c_key));
    EXPECT_EQ(pid_c_before, pid_c_after)
        << "C's mutable-only update must not change its part_id / ref";
    EXPECT_EQ(manifest_c_bytes_before, readObject(os, partKey("", pid_c_after).string()))
        << "C's manifest must be byte-for-byte unchanged";
    const PartManifest manifest_c_after = PartManifest::deserialize(readObject(os, partKey("", pid_c_after).string()));
    EXPECT_FALSE(manifest_c_after.blobs.contains("txn_version.txt"))
        << "C's mutable file must never enter the manifest";

    // 5. C's sidecar now carries the mutable txn_version.txt bytes; reading it back returns them.
    const std::string meta_c_key = refMetaKey("", sid, uuid, part_c).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(meta_c_key, /*with_tags=*/false).has_value())
        << "C's sidecar must exist after the mutable-only update";
    const RefSidecar sidecar_c = RefSidecar::deserialize(readObject(os, meta_c_key));
    ASSERT_TRUE(sidecar_c.files.contains("txn_version.txt"));
    EXPECT_EQ(sidecar_c.files.at("txn_version.txt"), txn_bytes);
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_c + "txn_version.txt")[0].remote_path), txn_bytes);

    // 6. C's content still resolves unchanged.
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_c + "data.bin")[0].remote_path), "C-DATA");

    // 7. A and B are distinct parts, not deduped onto each other (different content).
    EXPECT_NE(pid_a, pid_b);
}

// MULTI-PART moveDirectory re-key (B67 deferred tmp_merge_X -> X window): in ONE transaction, write
// the merge-output content under the temporary tmp_merge_X part, then write a mutable file (txn_version.txt)
// under the FINAL part X (so the staging map holds BOTH a tmp_merge_X entry and an X entry), then
// moveDirectory(tmp_merge_X -> X). The re-key must MERGE the tmp source staging entry into the final
// destination entry: carry the content blobs over from tmp_merge_X while PRESERVING the txn_version.txt
// already staged under X. After commit, part X must resolve with BOTH the content blobs (from tmp_merge_X)
// AND the mutable txn_version.txt (already under X), and tmp_merge_X must have NO ref of its own.
TEST_F(ContentAddressedMetaTest, MoveDirectoryMergesStagingTmpToFinal)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_mpt_movedir");
    auto os = getObjectStorage("cas_mpt_movedir");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-mpt-movedir";
    const std::string final_part = "all_1_2_1";
    const std::string tmp_part = "tmp_merge_all_1_2_1";
    const std::string base_tmp = "uui/" + uuid + "/" + tmp_part + "/";
    const std::string base_final = "uui/" + uuid + "/" + final_part + "/";
    const std::string txn_bytes = "13";

    {
        DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);

        // Merge output content staged under the TEMPORARY part name.
        auto m_data = tx.writeFile(base_tmp + "data.bin", 4096, DB::WriteMode::Rewrite, {});
        const std::string mdata = "MERGED-DATA";
        m_data->write(mdata.data(), mdata.size());
        m_data->finalize();
        auto m_cols = tx.writeFile(base_tmp + "columns.txt", 4096, DB::WriteMode::Rewrite, {});
        const std::string mcols = "x y";
        m_cols->write(mcols.data(), mcols.size());
        m_cols->finalize();

        // A CSN write arrives under the FINAL part name BEFORE the rename re-keys (the deferred window),
        // so the staging map gains a separate entry for the final part holding only the mutable file.
        auto f_txn = tx.writeFile(base_final + "txn_version.txt.tmp", 4096, DB::WriteMode::Rewrite, {});
        f_txn->write(txn_bytes.data(), txn_bytes.size());
        f_txn->finalize();
        tx.replaceFile(base_final + "txn_version.txt.tmp", base_final + "txn_version.txt");

        // The deferred merge rename: re-key tmp_merge_X -> X, MERGING the two staging entries.
        tx.moveDirectory("uui/" + uuid + "/" + tmp_part, "uui/" + uuid + "/" + final_part);

        tx.commit(DB::NoCommitOptions{});
    }

    // The FINAL part resolves with BOTH the content blobs (re-keyed from tmp_merge_X) AND the mutable file.
    const std::string ref_final_key = refKey("", sid, uuid, final_part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(ref_final_key, /*with_tags=*/false).has_value())
        << "final part ref must exist after the merge rename + commit";
    const PartId pid_final = partIdFromRefPayload(readObject(os, ref_final_key));
    const PartManifest manifest_final = PartManifest::deserialize(readObject(os, partKey("", pid_final).string()));
    EXPECT_TRUE(manifest_final.blobs.contains("data.bin"))
        << "content blobs must have been re-keyed from tmp_merge_X into the final part manifest";
    EXPECT_TRUE(manifest_final.blobs.contains("columns.txt"));
    EXPECT_FALSE(manifest_final.blobs.contains("txn_version.txt"))
        << "the mutable file must never enter the manifest";

    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_final + "data.bin")[0].remote_path), "MERGED-DATA");
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_final + "columns.txt")[0].remote_path), "x y");

    // The mutable txn_version.txt (staged under X, preserved across the merge) is in the sidecar.
    const std::string meta_final_key = refMetaKey("", sid, uuid, final_part).string();
    ASSERT_TRUE(os->tryGetObjectMetadata(meta_final_key, /*with_tags=*/false).has_value())
        << "final part sidecar must exist (it carries the preserved txn_version.txt)";
    const RefSidecar sidecar_final = RefSidecar::deserialize(readObject(os, meta_final_key));
    ASSERT_TRUE(sidecar_final.files.contains("txn_version.txt"));
    EXPECT_EQ(sidecar_final.files.at("txn_version.txt"), txn_bytes)
        << "the txn_version.txt staged under the final part must survive the tmp->final merge re-key";
    EXPECT_EQ(readObject(os, ms->getStorageObjects(base_final + "txn_version.txt")[0].remote_path), txn_bytes);

    // tmp_merge_X has NO ref of its own (its staging entry was merged into the final part, not published).
    const std::string ref_tmp_key = refKey("", sid, uuid, tmp_part).string();
    EXPECT_FALSE(os->tryGetObjectMetadata(ref_tmp_key, /*with_tags=*/false).has_value())
        << "tmp_merge_X must not have a published ref of its own after the merge re-key";
}
