#include <gtest/gtest.h>
#include <optional>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartId.h>
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

#include <filesystem>
#include <mutex>
#include <system_error>
#include <unordered_map>

using namespace DB::ContentAddressed;

TEST(ContentAddressedPoolPaths, ContentKeysFanOut)
{
    EXPECT_EQ(blobKey("abcdef0123"), "blobs/ab/cd/abcdef0123");
    EXPECT_EQ(partKey("0011223344"), "parts/00/11/0011223344");
    EXPECT_EQ(blobKey("ab"), "blobs/ab"); // too short to fan out (test-only)
}

TEST(ContentAddressedPoolPaths, RefKeys)
{
    EXPECT_EQ(refsPrefix("srvA", "uuid-1"), "store/srvA/uuid-1/refs/");
    EXPECT_EQ(refKey("srvA", "uuid-1", "all_1_1_0"), "store/srvA/uuid-1/refs/all_1_1_0");
}

TEST(ContentAddressedPoolPaths, ParsePartFilePath)
{
    auto file = parsePartFilePath("123/uuid-1/all_1_1_0/columns.txt");
    ASSERT_TRUE(file.has_value());
    EXPECT_EQ(file->table_uuid, "uuid-1");
    EXPECT_EQ(file->part_name, "all_1_1_0");
    EXPECT_EQ(file->file, "columns.txt");

    auto part_dir = parsePartFilePath("123/uuid-1/all_1_1_0/"); // trailing slash, no file
    ASSERT_TRUE(part_dir.has_value());
    EXPECT_EQ(part_dir->part_name, "all_1_1_0");
    EXPECT_EQ(part_dir->file, "");

    EXPECT_FALSE(parsePartFilePath("123/uuid-1").has_value());   // table dir, not a part
    EXPECT_FALSE(parsePartFilePath("123").has_value());          // shallower
}

TEST(ContentAddressedPoolPaths, ParseTableUuid)
{
    EXPECT_EQ(parseTableUuid("123/uuid-1/"), std::optional<std::string>("uuid-1"));
    EXPECT_EQ(parseTableUuid("123/uuid-1"), std::optional<std::string>("uuid-1"));
    EXPECT_FALSE(parseTableUuid("123/uuid-1/all_1_1_0").has_value()); // part dir, not table dir
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

        // The tests seed BARE keys (common-key-prefix handling is deferred to Phase 3),
        // so the content-pool roots are created at CWD and not covered by the per-storage
        // cleanup above. Remove them so CI runs leave no stray dirs in the repo root.
        // The std::error_code overload makes a missing dir a no-op.
        std::error_code ec;
        fs::remove_all("blobs", ec);
        fs::remove_all("parts", ec);
        fs::remove_all("store", ec);
        fs::remove_all("cas_wbuf_tmp", ec);
    }

private:
    std::shared_ptr<DB::IMetadataStorage> createMetadataStorage(const std::string & key_prefix)
    {
        fs::remove_all("./" + key_prefix);
        DB::LocalObjectStorageSettings settings("test", "./" + key_prefix, /*read_only_=*/false);
        auto object_storage = std::make_shared<DB::LocalObjectStorage>(std::move(settings));
        auto metadata_storage = std::make_shared<DB::ContentAddressedMetadataStorage>(object_storage, "", "test-server");

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

    writeObject(os, blobKey(blob_csum), blob_data);
    Footer f;
    f.blobs[file] = BlobEntry{blob_csum, blob_data.size(), blob_csum};
    writeObject(os, partKey(part_id), f.serialize());
    writeObject(os, refKey(sid, uuid, part), part_id);

    const std::string logical = "uui/" + uuid + "/" + part + "/" + file; // <uuid[:3]>/<uuid>/<part>/<file>
    EXPECT_TRUE(ms->existsFile(logical));
    EXPECT_EQ(ms->getFileSize(logical), blob_data.size());
    auto objs = ms->getStorageObjects(logical);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, blobKey(blob_csum));
    EXPECT_EQ(readObject(os, objs[0].remote_path), blob_data);
    EXPECT_FALSE(ms->existsFile("uui/" + uuid + "/" + part + "/absent.bin")); // file not in footer
}

TEST_F(ContentAddressedMetaTest, FailsClosedOnMissingFooter)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_failclose");
    auto os = getObjectStorage("cas_failclose");
    // ref present, but parts/<part_id> footer absent → must THROW (B18), not return empty
    writeObject(os, refKey(ms->serverIdForTest(), "uuid-3", "all_1_1_0"), "missingpid");
    EXPECT_THROW(ms->getStorageObjects("uui/uuid-3/all_1_1_0/data.bin"), DB::Exception);
}

TEST_F(ContentAddressedMetaTest, ListsPartsAndPartFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_list");
    auto os = getObjectStorage("cas_list");
    const std::string sid = ms->serverIdForTest();
    const std::string uuid = "uuid-4";

    auto seed = [&](const std::string & part, const std::string & pid, const Footer & f)
    {
        writeObject(os, partKey(pid), f.serialize());
        writeObject(os, refKey(sid, uuid, part), pid);
    };
    Footer fa; fa.blobs["data.bin"] = {"k1", 3, "k1"}; fa.blobs["columns.txt"] = {"k2", 2, "k2"};
    seed("all_1_1_0", "pidA", fa);
    Footer fb; fb.blobs["data.bin"] = {"k3", 4, "k3"};
    seed("all_2_2_0", "pidB", fb);

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
        ContentAddressedWriteBuffer buf(os, "./cas_wbuf_tmp");
        buf.write("HELLO-COLUMN", 12);
        buf.finalize();
        hash1 = buf.getBlobHash();
        EXPECT_EQ(buf.getSize(), 12u);
    }
    EXPECT_FALSE(hash1.empty());
    // bytes landed at blobs/<hash> and read back:
    EXPECT_EQ(readObject(os, blobKey(hash1)), "HELLO-COLUMN");

    // idempotent: identical content → same hash, no second upload (object already present)
    {
        ContentAddressedWriteBuffer buf2(os, "./cas_wbuf_tmp");
        buf2.write("HELLO-COLUMN", 12);
        buf2.finalize();
        EXPECT_EQ(buf2.getBlobHash(), hash1);
    }
    // different content → different hash + its own object
    std::string hash2;
    {
        ContentAddressedWriteBuffer buf3(os, "./cas_wbuf_tmp");
        buf3.write("OTHER", 5);
        buf3.finalize();
        hash2 = buf3.getBlobHash();
    }
    EXPECT_NE(hash2, hash1);
    EXPECT_EQ(readObject(os, blobKey(hash2)), "OTHER");
}

TEST(ContentAddressedPartId, DeterministicAndExcludesMutableFiles)
{
    using namespace DB::ContentAddressed;
    std::map<std::string, BlobEntry> a;
    a["a.bin"] = {"h1", 3, "h1"};
    a["b.bin"] = {"h2", 6, "h2"};

    // Order of insertion does not matter (std::map sorts), so an equal logical map => equal id.
    std::map<std::string, BlobEntry> a2;
    a2["b.bin"] = {"h2", 6, "h2"};
    a2["a.bin"] = {"h1", 3, "h1"};
    EXPECT_EQ(computePartId(a), computePartId(a2));

    // Mutable files do not contribute to the identity.
    std::map<std::string, BlobEntry> with_mutable = a;
    with_mutable["uuid.txt"] = {"u", 1, "u"};
    with_mutable["txn_version.txt"] = {"t", 1, "t"};
    with_mutable["metadata_version.txt"] = {"m", 1, "m"};
    EXPECT_EQ(computePartId(a), computePartId(with_mutable));

    // A different column checksum changes the identity.
    std::map<std::string, BlobEntry> b = a;
    b["a.bin"] = {"h1x", 3, "h1x"};
    EXPECT_NE(computePartId(a), computePartId(b));

    // Lowercase hex of a 128-bit value.
    const std::string id = computePartId(a);
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
        DB::ContentAddressedTransaction tx(*ms);
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

TEST_F(ContentAddressedMetaTest, MutationCarryForwardReusesBlob)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_cf");
    auto os = getObjectStorage("cas_cf");
    const std::string uuid = "uuid-10";
    // source part
    {
        DB::ContentAddressedTransaction tx(*ms);
        for (auto & [n,b] : std::map<std::string,std::string>{{"a.bin","A0"},{"b.bin","B0"},{"columns.txt","a b"}})
        { auto buf = tx.writeFile("uui/" + uuid + "/all_1_1_0/" + n, 4096, DB::WriteMode::Rewrite, {}); buf->write(b.data(), b.size()); buf->finalize(); }
        tx.commit(DB::NoCommitOptions{});
    }
    // mutation: rewrite a.bin, carry-forward b.bin + columns.txt
    {
        DB::ContentAddressedTransaction tx(*ms);
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
// (footer + ref), and read-back via the Phase-2 resolution path.
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

    // The footer + ref published by commit make the part files resolvable, and the bytes read back.
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
