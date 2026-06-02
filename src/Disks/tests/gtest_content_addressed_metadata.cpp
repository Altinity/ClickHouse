#include <gtest/gtest.h>
#include <optional>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
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
