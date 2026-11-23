#include <gtest/gtest.h>

#include <base/scope_guard.h>

#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Core/ServerUUID.h>
#include <Disks/DiskFactory.h>
#include <Disks/registerDisks.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Interpreters/Context.h>

#include <DisksApp.h>
#include <DisksClient.h>
#include <ICommand.h>
#include <Utils.h>

#include <boost/algorithm/string/replace.hpp>
#include <boost/program_options/parsers.hpp>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

/// `clickhouse-disks cas-migrate-pool` through the client the binary builds: the disk is created from a config
/// with its pool open deferred, then the command runs on it. Calling `migratePool` directly skips the client.

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int INVALID_STATE;
extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace fs = std::filesystem;

namespace
{

constexpr auto kDisk = "disk_cas_migrate";
constexpr auto kServerRoot = "upgrade_node";
/// A second CAS disk of the same config, created only when a command names it.
constexpr auto kOtherDisk = "disk_cas_other";

/// Path and bytes of every file under `root`.
std::map<String, String> snapshotFiles(const fs::path & root)
{
    std::map<String, String> files;
    for (const auto & entry : fs::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::ostringstream bytes;
        bytes << in.rdbuf();
        files[entry.path().string()] = bytes.str();
    }
    return files;
}

class CASMigratePoolCli : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        DB::ServerUUID::setRandomForUnitTests();
        DB::clearDiskRegistry();
        DB::registerDisks(/*global_skip_access_check=*/true);
    }

    static void TearDownTestSuite() { DB::clearDiskRegistry(); }

protected:
    void SetUp() override
    {
        root = fs::temp_directory_path()
            / ("cas_migrate_cli_" + std::to_string(::getpid()) + "_"
               + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        fs::create_directories(pool_dir());
        fs::create_directories(other_pool_dir());
        fs::create_directories(root / "scratch");
        fs::create_directories(root / "scratch_other");
    }

    void TearDown() override
    {
        DB::setDeferPoolOpenForMigration(false);
        fs::remove_all(root);
    }

    fs::path pool_dir() const { return root / "pool"; }
    fs::path other_pool_dir() const { return root / "pool_other"; }

    /// `gc_enabled` gives a writable open its GC scheduler, so a test can show the tool's run starts none.
    Poco::AutoPtr<Poco::Util::XMLConfiguration> config(bool read_only, bool gc_enabled = false) const
    {
        const auto disk = [&](const String & name, const fs::path & dir, const String & server_root, const fs::path & scratch)
        {
            std::ostringstream xml;
            xml << "<" << name << ">"
                << "<type>object_storage</type><object_storage_type>local</object_storage_type><metadata_type>cas</metadata_type>"
                << "<path>" << dir.string() << "/</path>"
                << "<cas_server_root_id>" << server_root << "</cas_server_root_id>"
                << "<cas_scratch_path>" << scratch.string() << "/</cas_scratch_path>"
                << "<readonly>" << (read_only ? "true" : "false") << "</readonly>"
                << "<cas_gc_enabled>" << (gc_enabled ? 1 : 0) << "</cas_gc_enabled>"
                << "</" << name << ">";
            return xml.str();
        };
        std::ostringstream xml;
        xml << "<clickhouse><disk>" << kDisk << "</disk><storage_configuration><disks>"
            << disk(kDisk, pool_dir(), kServerRoot, root / "scratch")
            << disk(kOtherDisk, other_pool_dir(), "other_node", root / "scratch_other")
            << "</disks></storage_configuration></clickhouse>";
        std::istringstream in(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(in));
    }

    /// The store as a second process sees it: what the tool left behind, read without the tool.
    std::shared_ptr<ObjectStorageBackend> store() const
    {
        auto object_storage = std::make_shared<DB::LocalObjectStorage>(
            DB::LocalObjectStorageSettings("cli_seed", pool_dir().string() + "/", /*read_only_=*/false));
        return std::make_shared<ObjectStorageBackend>(object_storage, ObjectStorageBackend::Mode::EmulatedSingleProcess);
    }

    /// A pool at reader generation 3 with one body under a `Condemned` marker and one under a `Clean` marker.
    void seedGenerationThreePool() const
    {
        auto backend = store();
        seedOlderPoolForTest(*backend, G_REF_CHAIN, layout.poolPrefix());
        writeBlobBody(*backend, layout, numberedBlobHash(1));
        writeMetaClean(*backend, layout, numberedBlobHash(1), 1);
        condemnMeta(*backend, layout, numberedBlobHash(1), 1);
        writeBlobBody(*backend, layout, numberedBlobHash(2));
        writeMetaClean(*backend, layout, numberedBlobHash(2), 1);
    }

    /// A pool created by a writable mount of this build, so at generation 4, with one marker left over.
    void seedGenerationFourPool() const
    {
        {
            auto writable = config(/*read_only=*/false);
            auto disk = DB::DiskFactory::instance().create(
                kDisk, *writable, String("storage_configuration.disks.") + kDisk, getContext().context, {},
                /*attach=*/false, /*custom_disk=*/false);
            disk->shutdown();
        }
        writeMetaClean(*store(), layout, numberedBlobHash(1), 1);
    }

    PoolMeta poolMeta() const
    {
        OperationForTest op(store());
        const auto got = (*op).read(layout.poolMetaKey(), Retry::standard());
        if (!got)
            throw std::runtime_error("no _pool_meta");
        return decodePoolMeta(got->bytes);
    }

    size_t keysUnder(const String & prefix) const
    {
        OperationForTest op(store());
        size_t keys = 0;
        (*op).forEachListedKey(prefix, [&](const ListedKey &)
        {
            ++keys;
            return true;
        }, Retry::standard());
        return keys;
    }

    size_t markers() const
    {
        OperationForTest op(store());
        size_t keys = 0;
        (*op).forEachListedKey(layout.blobsPrefix(), [&](const ListedKey & listed)
        {
            keys += listed.key.ends_with(".meta");
            return true;
        }, Retry::standard());
        return keys;
    }

    /// A file path under `pool_dir()` as the key the backend names it by.
    String relativeKey(const String & path) const { return fs::relative(path, pool_dir()).string(); }

    /// Files whose key starts with `prefix`, read from the directory rather than through a listing.
    size_t filesUnder(const String & prefix) const
    {
        size_t files = 0;
        for (const auto & [path, bytes] : snapshotFiles(pool_dir()))
            files += relativeKey(path).starts_with(prefix);
        return files;
    }

    GcState gcState() const
    {
        OperationForTest op(store());
        const auto got = (*op).read(layout.gcStateKey(), Retry::standard());
        if (!got)
            throw std::runtime_error("no gc/state");
        return decodeGcState(got->bytes);
    }

    /// `clickhouse-disks --disk <disk> --query '<command> <arguments>'`: the client is built as `DisksApp::main`
    /// builds it, the pool open deferred when the query runs `cas-migrate-pool`. Returns the command's standard
    /// output.
    /// `after` runs on the same client once the command returned.
    String runTool(const DB::CommandPtr & command, const DB::Strings & arguments, bool defer = true, bool read_only = true,
                   bool gc_enabled = false, const std::function<void(DB::DisksClient &)> & after = {}) const
    {
        DB::setDeferPoolOpenForMigration(defer);
        SCOPE_EXIT({ DB::setDeferPoolOpenForMigration(false); });
        auto tool_config = config(read_only, gc_enabled);
        DB::DisksClient client(*tool_config, getContext().context);
        ::testing::internal::CaptureStdout();
        try
        {
            command->execute(arguments, client);
        }
        catch (...)
        {
            ::testing::internal::GetCapturedStdout();
            throw;
        }
        String out = ::testing::internal::GetCapturedStdout();

        /// The tool opened its own pool and published none on the disk, so no mount, GC or lease ran.
        auto * ca = DB::ContentAddressedMetadataStorage::tryFromDisk(client.getCurrentDisk());
        EXPECT_NE(ca, nullptr);
        if (ca && defer)
            EXPECT_EQ(ca->poolForTest(), nullptr);
        if (after)
            after(client);
        return out;
    }

    static DB::Exception refusalOf(const std::function<void()> & action)
    {
        try
        {
            action();
        }
        catch (const DB::Exception & e)
        {
            return e;
        }
        throw std::runtime_error("the action was not refused");
    }

    fs::path root;
    const Layout layout{"ca"};
};

}

/// The tool runs on the disk as a server configures it: writable, GC on. Its deferred open keeps the disk from
/// mounting the pool, so the only writes are the migration's own, and neither this disk nor a second CAS disk
/// of the config gets a pool, a mount or a GC scheduler.
TEST_F(CASMigratePoolCli, MigratesAGenerationThreePool)
{
    seedGenerationThreePool();
    ASSERT_EQ(poolMeta().min_reader_generation, G_REF_CHAIN);
    ASSERT_EQ(markers(), 2u);
    const auto before = snapshotFiles(pool_dir());

    DB::ContentAddressedMetadataStorage * other = nullptr;
    const String out = runTool(DB::makeCommandCaMigratePool(), {"--grace-seconds", "0"}, /*defer=*/true, /*read_only=*/false,
        /*gc_enabled=*/true, [&](DB::DisksClient & client)
        {
            other = DB::ContentAddressedMetadataStorage::tryFromDisk(client.getDiskWithPathLazyInitialization(kOtherDisk).getDisk());
            ASSERT_NE(other, nullptr);
            EXPECT_EQ(other->poolForTest(), nullptr) << "the second CAS disk opened its pool";
        });

    EXPECT_NE(out.find("M3: 0 condemned row(s)"), String::npos) << out;
    EXPECT_NE(out.find("M4: 2 marker(s), 1 not Clean"), String::npos) << out;
    EXPECT_NE(out.find("migration complete"), String::npos) << out;
    EXPECT_EQ(poolMeta().min_reader_generation, G_BLOB_INTENTS);
    EXPECT_EQ(keysUnder(layout.blobIntentsPrefix()), 2u) << "every body has an intent";
    EXPECT_EQ(markers(), 0u);

    /// Every file the run left changed is a write of the migration: the floor, `gc/state` (M6), the intent (M5) and
    /// the removed markers (M9). No slot, epoch, owner, heartbeat or lease of a mount or a GC.
    const auto after = snapshotFiles(pool_dir());
    std::set<String> touched;
    for (const auto & [path, bytes] : after)
        if (auto it = before.find(path); it == before.end() || it->second != bytes)
            touched.insert(relativeKey(path));
    for (const auto & [path, bytes] : before)
        if (!after.contains(path))
            touched.insert(relativeKey(path));
    std::set<String> expected{layout.poolMetaKey(), layout.gcStateKey(), layout.blobMetaKey(blobRefOf(1)), layout.blobMetaKey(blobRefOf(2))};
    for (const auto & [path, bytes] : after)
        if (relativeKey(path).starts_with(layout.blobIntentsPrefix()))
            expected.insert(relativeKey(path));
    EXPECT_EQ(touched, expected);
    EXPECT_EQ(filesUnder(layout.serverRootsPrefix()), 0u);
    EXPECT_EQ(filesUnder(layout.gcHbKey()), 0u);
    const GcState state = gcState();
    EXPECT_EQ(state.header_generation, G_BLOB_INTENTS);
    EXPECT_EQ(state.lease.owner, DB::UInt128(0)) << "a GC lease was taken";
    EXPECT_TRUE(snapshotFiles(other_pool_dir()).empty()) << "the second CAS disk wrote to its store";

    /// Positive control: the same config opened without the deferral mounts both pools.
    runTool(DB::makeCommandList(), {}, /*defer=*/false, /*read_only=*/false, /*gc_enabled=*/true, [&](DB::DisksClient & client)
    {
        EXPECT_NE(DB::ContentAddressedMetadataStorage::tryFromDisk(client.getCurrentDisk())->poolForTest(), nullptr);
        auto * second = DB::ContentAddressedMetadataStorage::tryFromDisk(client.getDiskWithPathLazyInitialization(kOtherDisk).getDisk());
        EXPECT_NE(second->poolForTest(), nullptr);
    });
    EXPECT_GT(filesUnder(layout.mountKey(kServerRoot)), 0u);
    EXPECT_FALSE(snapshotFiles(other_pool_dir()).empty());
}

/// Without `--query` the disk opens the pool itself, so the command refuses to run.
TEST_F(CASMigratePoolCli, InteractiveRunIsRefused)
{
    seedGenerationFourPool();
    const DB::Exception refusal = refusalOf([&]
    {
        runTool(DB::makeCommandCaMigratePool(), {"--dry-run"}, /*defer=*/false, /*read_only=*/false);
    });
    EXPECT_NE(refusal.message().find("run it as `clickhouse-disks --query 'cas-migrate-pool ...'`"), String::npos)
        << refusal.message();
}

TEST_F(CASMigratePoolCli, OnlyAQueryRunningTheToolDefersTheOpen)
{
    for (const String query : {"cas-migrate-pool", "cas-migrate-pool --dry-run", "  cas-migrate-pool\t--grace-seconds 1"})
        EXPECT_TRUE(queryRunsCasMigratePool(query)) << query;
    for (const String query : {"ls", "cas-fsck", "cas-gc-dryrun; ls", "write cas-migrate-pool", ""})
        EXPECT_FALSE(queryRunsCasMigratePool(query)) << query;
}

TEST_F(CASMigratePoolCli, DryRunOnAMigratedPoolWritesNothing)
{
    seedGenerationFourPool();
    ASSERT_EQ(poolMeta().min_reader_generation, G_BLOB_INTENTS);
    const auto before = snapshotFiles(pool_dir());
    ASSERT_FALSE(before.empty());

    const String out = runTool(DB::makeCommandCaMigratePool(), {"--grace-seconds", "0", "--dry-run"});

    EXPECT_NE(out.find("the pool is at reader generation 4"), String::npos) << out;
    EXPECT_NE(out.find("dry run: 1 marker(s) would be removed; nothing was written"), String::npos) << out;
    EXPECT_EQ(out.find("migration complete"), String::npos) << out;
    EXPECT_EQ(snapshotFiles(pool_dir()), before);
}

/// A read-only local object storage refuses every delete, so the marker repair is refused before the scan, and the
/// refusal names the command that removes the markers on this store.
TEST_F(CASMigratePoolCli, MarkerRepairOnAReadOnlyLocalStoreIsRefusedUpFront)
{
    seedGenerationFourPool();
    ASSERT_EQ(markers(), 1u);
    const auto before = snapshotFiles(pool_dir());

    const DB::Exception refusal = refusalOf([&] { runTool(DB::makeCommandFsck(), {"--fix", "legacy-markers"}, /*defer=*/false); });
    EXPECT_EQ(refusal.code(), DB::ErrorCodes::BAD_ARGUMENTS) << refusal.message();
    EXPECT_NE(refusal.message().find(String(kMigratePoolCommandLine)), String::npos) << refusal.message();
    EXPECT_EQ(snapshotFiles(pool_dir()), before);

    /// Positive control: the scan alone runs on the same disk and counts the marker.
    const String scan = runTool(DB::makeCommandFsck(), {}, /*defer=*/false);
    EXPECT_NE(scan.find("legacy_marker=1"), String::npos) << scan;

    runTool(DB::makeCommandCaMigratePool(), {"--grace-seconds", "0"}, /*defer=*/true, /*read_only=*/false);
    EXPECT_EQ(markers(), 0u) << "the command the refusal names did not remove the marker";
}

TEST_F(CASMigratePoolCli, OtherCommandsOnAnOlderPoolAreRefused)
{
    seedGenerationThreePool();
    const auto before = snapshotFiles(pool_dir());

    /// Without `cas-migrate-pool` in the query the disk opens the pool, and the open refuses it.
    for (const auto & make : {DB::makeCommandList, DB::makeCommandFsck, DB::makeCommandCaGcDryRun})
    {
        const DB::Exception refusal = refusalOf([&] { runTool(make(), {}, /*defer=*/false); });
        EXPECT_EQ(refusal.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION) << refusal.message();
        EXPECT_NE(refusal.message().find("pool is at reader generation 3"), String::npos) << refusal.message();
        EXPECT_NE(refusal.message().find("stop every server of the pool, then run `" + String(kMigratePoolCommandLine) + "`"),
                  String::npos)
            << refusal.message();
    }

    /// With the open deferred, another command meets a storage that has no pool.
    for (const auto & make : {DB::makeCommandList, DB::makeCommandFsck, DB::makeCommandCaGcDryRun})
    {
        const DB::Exception refusal = refusalOf([&] { runTool(make(), {}); });
        EXPECT_EQ(refusal.code(), DB::ErrorCodes::INVALID_STATE) << refusal.message();
        EXPECT_NE(refusal.message().find("is not started"), String::npos) << refusal.message();
    }

    EXPECT_EQ(snapshotFiles(pool_dir()), before);
    EXPECT_EQ(poolMeta().min_reader_generation, G_REF_CHAIN);
}

TEST_F(CASMigratePoolCli, ToolRefusesToShareAQuery)
{
    for (const String query : {"cas-migrate-pool; ls", "ls; cas-migrate-pool --dry-run", "cas-migrate-pool; cas-migrate-pool"})
    {
        const DB::Exception refusal = refusalOf([&] { queryRunsCasMigratePool(query); });
        EXPECT_EQ(refusal.code(), DB::ErrorCodes::BAD_ARGUMENTS) << query;
        EXPECT_NE(refusal.message().find("cas-migrate-pool must be the only command of --query"), String::npos)
            << refusal.message();
    }
    /// Positive controls: the tool alone, with a trailing separator, and queries without it.
    for (const String query : {"cas-migrate-pool --dry-run", "cas-migrate-pool;", " ; cas-migrate-pool", "ls; cas-fsck"})
        EXPECT_NO_THROW(queryRunsCasMigratePool(query)) << query;
}

/// The command line a refusal prints is what the disks app parses as the tool's run on the named disk.
TEST(CASMigratePoolRefusal, PrintedCommandLineParsesAsTheTool)
{
    auto backend = std::make_shared<CountingBackend>();
    seedOlderPoolForTest(*backend, G_REF_CHAIN);
    for (const bool read_only : {false, true})
    {
        SCOPED_TRACE(read_only);
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
        config.read_only = read_only;
        String message;
        try
        {
            Pool::open(backend, config);
        }
        catch (const DB::Exception & e)
        {
            message = e.message();
        }
        const size_t begin = message.find("`clickhouse-disks ");
        ASSERT_NE(begin, String::npos) << message;
        const size_t end = message.find('`', begin + 1);
        ASSERT_NE(end, String::npos) << message;
        String line = message.substr(begin + 1, end - begin - 1);
        boost::replace_all(line, "SERVER_CONFIG_FILE", "/etc/clickhouse-server/config.xml");
        boost::replace_all(line, "DISK_NAME", "s3_cas");

        std::vector<String> words = boost::program_options::split_unix(line);
        ASSERT_FALSE(words.empty());
        EXPECT_EQ(words.front(), "clickhouse-disks");
        words.erase(words.begin());
        const auto options = DB::DisksApp::parseCommonArguments(words);
        ASSERT_TRUE(options.contains("disk") && options.contains("query") && options.contains("config-file")) << line;
        EXPECT_EQ(options["disk"].as<String>(), "s3_cas");
        EXPECT_EQ(options["config-file"].as<String>(), "/etc/clickhouse-server/config.xml");
        EXPECT_EQ(options["query"].as<String>(), "cas-migrate-pool");
        EXPECT_TRUE(queryRunsCasMigratePool(options["query"].as<String>()));
    }

    /// Control: the spelling the refusal used to print selects a disk named after the command and runs nothing.
    const auto old = DB::DisksApp::parseCommonArguments({"cas-migrate-pool"});
    EXPECT_EQ(old["disk"].as<String>(), "cas-migrate-pool");
    EXPECT_FALSE(old.contains("query"));
}
