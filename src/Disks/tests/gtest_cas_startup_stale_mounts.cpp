#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Core/ServerUUID.h>
#include <Disks/DiskFactory.h>
#include <Disks/DiskFromAST.h>
#include <Disks/registerDisks.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Interpreters/Context.h>
#include <Parsers/ExpressionListParsers.h>
#include <Parsers/parseQuery.h>

#include <Poco/AutoPtr.h>
#include <Poco/Channel.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>
#include <Poco/Util/XMLConfiguration.h>
#include <base/scope_guard.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

/// Server start re-creates the `disk = disk(...)` custom disks of every table it loads. A CAS disk
/// whose previous process died without a farewell waits out the stale mount lease inside that
/// creation. These tests pin how such waits compose with the rest of startup.

namespace
{

namespace fs = std::filesystem;
using namespace DB::Cas;

constexpr uint64_t lease_ttl_ms = 3000;
constexpr uint64_t renew_period_ms = 600;
/// `mountObservationThresholdMs`: TTL + TTL/20 + renew_period/2.
constexpr uint64_t observation_ms = lease_ttl_ms + lease_ttl_ms / 20 + renew_period_ms / 2;

uint64_t elapsedMs(std::chrono::steady_clock::time_point since)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

uint64_t wallNowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

struct StaleCasDisk
{
    String name;
    String server_root_id;
    fs::path pool_dir;
    fs::path scratch_dir;

    std::map<String, String> settings() const
    {
        return {
            {"type", "object_storage"},
            {"object_storage_type", "local"},
            {"metadata_type", "cas"},
            {"path", pool_dir.string() + "/"},
            {"cas_server_root_id", server_root_id},
            {"cas_scratch_path", scratch_dir.string() + "/"},
            {"cas_mount_lease_ttl_ms", std::to_string(lease_ttl_ms)},
            {"cas_mount_renew_period_ms", std::to_string(renew_period_ms)},
            {"cas_attempt_timeout_ms", "200"},
            {"cas_lease_safety_margin_ms", "200"},
            {"cas_gc_enabled", "0"},
        };
    }

    /// The table-metadata form: what server start parses from `SETTINGS disk = disk(...)`.
    DB::ASTPtr diskFunctionAst() const
    {
        String text = "disk(name = '" + name + "'";
        for (const auto & [key, value] : settings())
            text += ", " + key + " = '" + value + "'";
        text += ")";
        DB::ParserFunction parser;
        return DB::parseQuery(parser, text, "", 0, 0, 0);
    }

    Poco::AutoPtr<Poco::Util::XMLConfiguration> xmlConfig() const
    {
        std::ostringstream xml;
        xml << "<clickhouse><disk>";
        for (const auto & [key, value] : settings())
            xml << "<" << key << ">" << value << "</" << key << ">";
        xml << "</disk></clickhouse>";
        std::istringstream in(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(in));
    }
};

/// Leaves `disk` as a process killed after it minted a writer epoch and claimed the mount: a live-
/// looking lease with no farewell, held by this server's own uuid under an older epoch.
void seedStaleMount(const StaleCasDisk & disk)
{
    {
        auto config = disk.xmlConfig();
        auto seeded = DB::DiskFactory::instance().create(
            disk.name + "_seed", *config, "disk", getContext().context, {}, /*attach=*/true, /*custom_disk=*/true);
        seeded->shutdown();
    }

    auto object_storage = std::make_shared<DB::LocalObjectStorage>(
        DB::LocalObjectStorageSettings(disk.name + "_seed_backend", disk.pool_dir.string() + "/", /*read_only_=*/false));
    auto backend = std::make_shared<ObjectStorageBackend>(object_storage, ObjectStorageBackend::Mode::EmulatedSingleProcess);
    DB::Cas::tests::OperationForTest op(backend);
    const Layout layout{"ca"};

    const auto mount = (*op).read(layout.mountKey(disk.server_root_id), Retry::standard());
    ASSERT_TRUE(mount.has_value());
    const MountLease farewell = decodeMountLease(mount->bytes);
    ASSERT_EQ(farewell.min_active_build_sequence, std::numeric_limits<uint64_t>::max())
        << "the seeding open must end in a clean farewell";

    const auto epoch_object = (*op).read(layout.epochKey(disk.server_root_id), Retry::standard());
    ASSERT_TRUE(epoch_object.has_value());
    const uint64_t crashed_epoch = decodeServerEpoch(epoch_object->bytes).next_writer_epoch;
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(
        layout.epochKey(disk.server_root_id), encodeServerEpoch(ServerEpoch{.next_writer_epoch = crashed_epoch + 1}),
        epoch_object->etag, Retry::standard())));

    const MountClaimResult claim = claimMount(
        *op, layout, disk.server_root_id, farewell.server_uuid, crashed_epoch, wallNowMs(), lease_ttl_ms);
    ASSERT_EQ(claim.kind, MountClaimResult::Claimed);
}

/// Signals when a CAS mount starts waiting out a stale lease.
class StaleMountWaitChannel : public Poco::Channel
{
public:
    void log(const Poco::Message & msg) override
    {
        if (msg.getText().find("stale-looking mount lease") == std::string::npos)
            return;
        std::lock_guard lock(mutex);
        ++waits_started;
        cv.notify_all();
    }

    bool waitForWaitStarted(size_t count)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(30), [&] { return waits_started >= count; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    size_t waits_started = 0;
};

class CASStartupStaleMounts : public ::testing::Test
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
            / ("cas_startup_stale_mounts_" + std::to_string(::getpid()) + "_"
               + ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(root);
        fs::create_directories(root);
    }

    void TearDown() override
    {
        for (const auto & disk : disks)
            if (auto created = getContext().context->getDisksMap(); created.contains(disk.name))
                created.at(disk.name)->shutdown();
        fs::remove_all(root);
    }

    StaleCasDisk & addStaleDisk(const String & suffix)
    {
        const String test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        StaleCasDisk disk{
            .name = "cas_stale_" + test_name + "_" + suffix,
            .server_root_id = "stale_" + suffix,
            .pool_dir = root / ("pool_" + suffix),
            .scratch_dir = root / ("scratch_" + suffix),
        };
        fs::create_directories(disk.scratch_dir);
        seedStaleMount(disk);
        return disks.emplace_back(std::move(disk));
    }

    fs::path root;
    std::vector<StaleCasDisk> disks;
};

}

/// Each stale mount must be waited out once, but the waits of different disks are independent.
/// Waiting them one after another makes server start N × the observation window, which is what
/// missed the start timeout with three stale mounts.
TEST_F(CASStartupStaleMounts, StaleMountsOfDifferentDisksAreWaitedOutConcurrently)
{
    constexpr size_t disk_count = 3;
    for (size_t i = 0; i < disk_count; ++i)
        addStaleDisk(std::to_string(i));
    ASSERT_FALSE(HasFatalFailure());

    std::vector<uint64_t> per_disk_ms(disk_count, 0);
    std::vector<std::exception_ptr> errors(disk_count);
    const auto start = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> loaders;
        for (size_t i = 0; i < disk_count; ++i)
            loaders.emplace_back([&, i]
            {
                try
                {
                    DB::DiskFromAST::createCustomDisk(disks[i].diskFunctionAst(), getContext().context, /*attach=*/true);
                }
                catch (...)
                {
                    errors[i] = std::current_exception();
                }
                per_disk_ms[i] = elapsedMs(start);
            });
        for (auto & loader : loaders)
            loader.join();
    }
    const uint64_t total_ms = elapsedMs(start);

    for (size_t i = 0; i < disk_count; ++i)
    {
        ASSERT_FALSE(errors[i]) << DB::getExceptionMessage(errors[i], false);
        EXPECT_GE(per_disk_ms[i], observation_ms) << "disk " << i << " had no stale mount to wait out";
    }
    EXPECT_LT(total_ms, 2 * observation_ms)
        << disk_count << " stale mounts took " << total_ms << " ms to open; one observation window is "
        << observation_ms << " ms, so the waits ran one after another";
}

/// A disk lookup by an unrelated table load must not queue behind a stale-mount wait: the wait is
/// up to a full lease TTL, and every table load and system log flush resolves disks.
TEST_F(CASStartupStaleMounts, DiskLookupDoesNotWaitForAStaleMountObservation)
{
    const StaleCasDisk & stale = addStaleDisk("0");
    ASSERT_FALSE(HasFatalFailure());

    Poco::Logger & pool_logger = Poco::Logger::get("CasPool");
    Poco::AutoPtr<Poco::Channel> previous_channel(pool_logger.getChannel(), /*shared=*/true);
    const int previous_level = pool_logger.getLevel();
    Poco::AutoPtr<StaleMountWaitChannel> channel(new StaleMountWaitChannel);
    pool_logger.setChannel(channel);
    pool_logger.setLevel(Poco::Message::PRIO_INFORMATION);
    SCOPE_EXIT({
        pool_logger.setChannel(previous_channel);
        pool_logger.setLevel(previous_level);
    });

    std::exception_ptr error;
    std::thread loader([&]
    {
        try
        {
            DB::DiskFromAST::createCustomDisk(stale.diskFunctionAst(), getContext().context, /*attach=*/true);
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });

    ASSERT_TRUE(channel->waitForWaitStarted(1)) << "the stale mount was never observed";
    const auto lookup_start = std::chrono::steady_clock::now();
    (void)getContext().context->getDisksMap();
    const uint64_t lookup_ms = elapsedMs(lookup_start);
    loader.join();

    ASSERT_FALSE(error) << DB::getExceptionMessage(error, false);
    EXPECT_LT(lookup_ms, observation_ms / 3)
        << "a disk lookup took " << lookup_ms << " ms while a stale mount was being observed for up to "
        << observation_ms << " ms";
}
