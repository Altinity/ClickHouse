#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasUploadGuard.h>
#include <Disks/tests/cas_killable_query.h>

#include <Common/Exception.h>
#include <Common/ThreadStatus.h>

#include <atomic>
#include <limits>
#include <memory>
#include <vector>

#include "config.h"
#if USE_AWS_S3
#include <Disks/DiskCommitTransactionOptions.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_fake_generation_object_storage.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/WriteSettings.h>
#include <Interpreters/Context.h>
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <unistd.h>
#endif

namespace DB::ErrorCodes
{
    extern const int QUERY_WAS_CANCELLED;
}

using namespace DB;
using namespace DB::Cas;
using DB::Cas::tests::CasKillableQuery;
using DB::Cas::tests::onKillableQueryThread;
using DB::Cas::tests::onThread;

namespace
{

/// Records what reaches it and reports fixed limits.
class RecordingThrottler final : public IThrottler
{
public:
    bool throttle(size_t amount_, size_t max_block_ns_) override
    {
        amount += amount_;
        max_block_ns = max_block_ns_;
        ++calls;
        return true;
    }

    bool isThrottling() const override { return true; }
    Int64 getAvailable() override { return 123; }
    UInt64 getMaxSpeed() const override { return 456; }
    UInt64 getMaxBurst() const override { return 789; }

    size_t amount = 0;
    size_t max_block_ns = 0;
    size_t calls = 0;
};

int codeOf(const std::function<void()> & call)
{
    try
    {
        call();
    }
    catch (const Exception & e)
    {
        return e.code();
    }
    return 0;
}

}

TEST(CASUploadGuard, GuardForwardsAndStops)
{
    auto limiter = std::make_shared<RecordingThrottler>();

    EXPECT_EQ(makeUploadGuard(limiter, {}), limiter);
    EXPECT_EQ(makeUploadGuard(nullptr, {}), nullptr);

    std::vector<size_t> inner_calls_at_stop;
    auto guard = makeUploadGuard(limiter, [&] { inner_calls_at_stop.push_back(limiter->calls); });
    ASSERT_NE(guard, limiter);
    EXPECT_TRUE(guard->throttle(32768, 20'000'000'000));
    EXPECT_EQ(limiter->amount, 32768u);
    EXPECT_EQ(limiter->max_block_ns, 20'000'000'000u);
    EXPECT_EQ(inner_calls_at_stop, (std::vector<size_t>{0, 1}));
    EXPECT_TRUE(guard->isThrottling());
    EXPECT_EQ(guard->getAvailable(), 123);
    EXPECT_EQ(guard->getMaxSpeed(), 456u);
    EXPECT_EQ(guard->getMaxBurst(), 789u);

    std::atomic<bool> stopped{false};
    auto stopping = makeUploadGuard(limiter, [&]
    {
        if (stopped)
            throw Exception(ErrorCodes::QUERY_WAS_CANCELLED, "stopped");
    });
    stopped = true;
    const size_t calls_before = limiter->calls;
    EXPECT_EQ(codeOf([&] { stopping->throttle(1, 1); }), ErrorCodes::QUERY_WAS_CANCELLED);
    EXPECT_EQ(limiter->calls, calls_before) << "the stop before the inner call must keep the bytes from the limiter";

    stopped = false;
    auto stop_after = makeUploadGuard(limiter, [&, first = true]() mutable
    {
        if (!std::exchange(first, false))
            throw Exception(ErrorCodes::QUERY_WAS_CANCELLED, "stopped");
    });
    EXPECT_EQ(codeOf([&] { stop_after->throttle(1, 1); }), ErrorCodes::QUERY_WAS_CANCELLED);
    EXPECT_EQ(limiter->calls, calls_before + 1) << "the stop after the inner call runs once the limiter was charged";
}

TEST(CASUploadGuard, AStopOnlyGuardStopsAndReportsNoLimit)
{
    onKillableQueryThread([](CasKillableQuery & query)
    {
        auto guard = makeUploadGuard(nullptr, uploadStopCheck());
        ASSERT_NE(guard, nullptr);
        EXPECT_FALSE(guard->throttle(32768, 1));
        EXPECT_FALSE(guard->isThrottling());
        EXPECT_EQ(guard->getAvailable(), std::numeric_limits<Int64>::max());
        EXPECT_EQ(guard->getMaxSpeed(), 0u);
        EXPECT_EQ(guard->getMaxBurst(), 0u);

        query.kill();
        EXPECT_EQ(codeOf([&] { guard->throttle(32768, 1); }), ErrorCodes::QUERY_WAS_CANCELLED);
    });
}

TEST(CASUploadGuard, StopCheckIsEmptyWhereWorkMustFinish)
{
    onKillableQueryThread([](CasKillableQuery &)
    {
        EXPECT_TRUE(uploadStopCheck());
        {
            auto owed = OwnerScope::owed();
            EXPECT_FALSE(uploadStopCheck());
        }
        {
            auto shared = OwnerScope::shared();
            EXPECT_FALSE(uploadStopCheck());
        }
        EXPECT_TRUE(uploadStopCheck());
    });

    onThread(true, [] { EXPECT_FALSE(uploadStopCheck()) << "a thread with no group has no query to stop"; });
    onThread(false, [] { EXPECT_FALSE(uploadStopCheck()); });
}

TEST(CASUploadGuard, StopCheckCarriesTheQueryVerdictAndKeepsTheGroup)
{
    std::function<void()> check;
    std::weak_ptr<ThreadGroup> group;

    onKillableQueryThread([&](CasKillableQuery & query)
    {
        check = uploadStopCheck();
        ASSERT_TRUE(check);
        group = query.group;

        onThread(false, [&] { EXPECT_EQ(codeOf(check), 0); });
        query.kill();
        onThread(false, [&] { EXPECT_EQ(codeOf(check), ErrorCodes::QUERY_WAS_CANCELLED); });
    });

    EXPECT_FALSE(group.expired()) << "the check keeps the group after the fixture released its references";
    check = nullptr;
    EXPECT_TRUE(group.expired());
}

TEST(CASUploadGuard, AMergeThreadGroupGetsAStopThatNeverThrows)
{
    onThread(true, []
    {
        CurrentThread::attachToGroup(ThreadGroup::createForMergeMutate(getContext().context));
        const std::function<void()> stop = uploadStopCheck();
        if (stop)
            EXPECT_NO_THROW(stop());

        auto limiter = std::make_shared<RecordingThrottler>();
        auto guard = makeUploadGuard(limiter, stop);
        EXPECT_NO_THROW(guard->throttle(32768, 0));
        EXPECT_EQ(limiter->amount, 32768u);
    });
}

TEST(CASUploadGuard, BodyGuardScopeMatchesTheExactKeyOnly)
{
    auto outer = std::make_shared<RecordingThrottler>();
    auto inner = std::make_shared<RecordingThrottler>();

    EXPECT_EQ(BodyGuardScope::forKey("a"), nullptr);
    {
        BodyGuardScope scope("a", outer);
        EXPECT_EQ(BodyGuardScope::forKey("a"), outer);
        EXPECT_EQ(BodyGuardScope::forKey("b"), nullptr);
        EXPECT_EQ(BodyGuardScope::forKey("a/"), nullptr);
        EXPECT_EQ(BodyGuardScope::forKey(""), nullptr);
        {
            BodyGuardScope nested("b", inner);
            EXPECT_EQ(BodyGuardScope::forKey("b"), inner);
            EXPECT_EQ(BodyGuardScope::forKey("a"), nullptr);
        }
        EXPECT_EQ(BodyGuardScope::forKey("a"), outer);
        EXPECT_EQ(BodyGuardScope::forKey("b"), nullptr);
    }
    EXPECT_EQ(BodyGuardScope::forKey("a"), nullptr);

    onThread(false, [&]
    {
        BodyGuardScope scope("a", outer);
    });
    EXPECT_EQ(BodyGuardScope::forKey("a"), nullptr);
}

TEST(CASUploadGuard, UploadStopScopeRestoresItsPredecessor)
{
    EXPECT_FALSE(UploadStopScope::current());
    int outer_calls = 0;
    int inner_calls = 0;
    {
        UploadStopScope outer([&] { ++outer_calls; });
        UploadStopScope::current()();
        {
            UploadStopScope inner([&] { ++inner_calls; });
            UploadStopScope::current()();
        }
        UploadStopScope::current()();
        {
            UploadStopScope empty({});
            EXPECT_FALSE(UploadStopScope::current());
        }
        EXPECT_TRUE(UploadStopScope::current());
    }
    EXPECT_FALSE(UploadStopScope::current());
    EXPECT_EQ(outer_calls, 2);
    EXPECT_EQ(inner_calls, 1);

    UploadStopScope here([] {});
    onThread(false, [] { EXPECT_FALSE(UploadStopScope::current()) << "the marker is thread-scoped"; });
}

#if USE_AWS_S3

namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsString staging_backend;
}

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace
{

using DB::Cas::tests::FakeGenerationObjectStorage;
using DB::Cas::tests::makeFakeGenerationObjectStorageForTest;
using WriteRecords = std::vector<std::shared_ptr<const FakeGenerationObjectStorage::WriteRecord>>;

const String kTable = "a11/a11a11a1-1111-4111-8111-111111111111/";

/// A mount of `object_storage` that stages blob bodies in local files, so each body is streamed through `writeObject`
/// at publication; with `s3_staging` it stages them in the store and publishes them by a server-side copy.
std::shared_ptr<ContentAddressedMetadataStorage> mountOver(
    const ObjectStoragePtr & object_storage, const String & server_root_id, bool s3_staging = false)
{
    static std::atomic<uint64_t> counter{0};
    const auto scratch = std::filesystem::temp_directory_path()
        / ("cas_upload_guard_" + server_root_id + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)));
    auto settings = DB::Cas::tests::makeSettingsForTest(server_root_id, scratch);
    if (s3_staging)
    {
        settings[ContentAddressedSetting::staging_backend] = "s3";
        settings.validate();
    }
    auto mount = DB::Cas::tests::ownedByDirectory(
        std::make_shared<ContentAddressedMetadataStorage>(object_storage, "pool", "srv1", /*disk_name_=*/"", /*context_=*/nullptr, settings),
        scratch);
    mount->startup();
    return mount;
}

void writeWith(IMetadataTransaction & tx, const String & path, const String & bytes, ThrottlerPtr limiter)
{
    WriteSettings settings;
    settings.remote_throttler = std::move(limiter);
    auto buf = dynamic_cast<ContentAddressedTransaction &>(tx).writeFile(path, 65536, WriteMode::Rewrite, settings);
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

String bodyKeyOf(const PoolPtr & pool, const String & payload)
{
    return pool->layout().blobKey(poolContentHash(pool->writeAlgo(), payload));
}

/// The streamed writes of `key`, copies excluded.
WriteRecords writesOf(const FakeGenerationObjectStorage & storage, const String & key)
{
    WriteRecords result;
    for (const auto & record : storage.records())
        if (record->key == key && !record->copy)
            result.push_back(record);
    return result;
}

/// The throttler of the one streamed body of `payload`.
ThrottlerPtr bodyThrottlerOf(const FakeGenerationObjectStorage & storage, const PoolPtr & pool, const String & payload)
{
    const auto writes = writesOf(storage, bodyKeyOf(pool, payload));
    if (writes.size() != 1)
        throw std::runtime_error(fmt::format("expected one body write of {}, got {}", bodyKeyOf(pool, payload), writes.size()));
    return writes.front()->settings.remote_throttler;
}

/// Calls `throttler` as a socket does at a refill and checks that `target` alone among `limiters` was charged.
void expectReachesOnly(const ThrottlerPtr & throttler, const RecordingThrottler & target, const std::vector<const RecordingThrottler *> & limiters)
{
    ASSERT_NE(throttler, nullptr) << "the body carries no throttler";
    std::vector<size_t> before;
    for (const auto * limiter : limiters)
        before.push_back(limiter->amount);
    EXPECT_EQ(codeOf([&] { throttler->throttle(32768, 0); }), 0);
    for (size_t i = 0; i < limiters.size(); ++i)
        EXPECT_EQ(limiters[i]->amount - before[i], limiters[i] == &target ? 32768u : 0u) << "limiter #" << i;
}

/// Sets the query's `max_remote_write_network_bandwidth` to one byte per second and returns the limiter that the
/// calling thread's writes get from it.
ThrottlerPtr limitQueryRemoteWrites(CasKillableQuery & query)
{
    query.context->setSetting("max_remote_write_network_bandwidth", Field(UInt64{1}));
    return DB::getWriteSettings().remote_throttler;
}

bool isManifestKey(const PoolPtr & pool, const String & key)
{
    return key.starts_with(pool->layout().casManifestsPrefix());
}

struct BoundedEvent
{
    std::mutex mutex;
    std::condition_variable cv;
    bool fired = false;

    void fire()
    {
        {
            std::lock_guard lock(mutex);
            fired = true;
        }
        cv.notify_all();
    }

    bool wait(std::chrono::seconds bound)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, bound, [&] { return fired; });
    }
};

/// A precommitted build of `part_path`'s part, which every publication of `uploadBlobDetached` requires.
PartWriteTxnPtr precommittedBuild(ContentAddressedMetadataStorage & mount, const String & part_path)
{
    const auto route = mount.route(*parsePartFilePath(part_path + "data.bin"));
    PartWriteInfo info;
    info.intended_ref = route->ns.string() + "/" + route->ref;
    PartWriteTxnPtr build = mount.poolForTest()->beginPartWrite(std::move(info));
    const String seed = "seed-manifest-" + part_path;
    const ManifestId id = build->stageManifest({DB::Cas::tests::blobEntryFor("seed.bin", DB::Cas::tests::u128Of(seed), seed.size())});
    build->precommitAdd(route->ns, route->ref, id);
    return build;
}

BlobUploadRequest requestFor(const PoolPtr & pool, const String & payload, ThrottlerPtr limiter)
{
    BlobUploadRequest request{poolContentHash(pool->writeAlgo(), payload), BlobSource::fromString(payload), payload.size()};
    request.source.remote_throttler = std::move(limiter);
    return request;
}

std::unique_ptr<ThreadPool> makeWorkers(size_t size)
{
    return std::make_unique<ThreadPool>(
        CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled, size);
}

}

TEST(CASUploadGuard, EachBodyCarriesItsOwnLimiterAndTheStopOnlyInANewBuild)
{
    enum class Arm : uint8_t { NewBuild, CommittedPart, Owed, Shared };
    for (const Arm arm : {Arm::NewBuild, Arm::CommittedPart, Arm::Owed, Arm::Shared})
    {
        const char * name = arm == Arm::NewBuild ? "new build"
            : arm == Arm::CommittedPart          ? "write onto a committed part"
            : arm == Arm::Owed                   ? "inside owed"
                                                 : "inside shared";
        SCOPED_TRACE(name);
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, fmt::format("mountGuardBodies{}", static_cast<int>(arm)));
        const String part = kTable + "all_1_1_0/";
        if (arm == Arm::CommittedPart)
        {
            auto tx = mount->createTransaction();
            writeWith(*tx, part + "count.txt", "1", nullptr);
            tx->commit(NoCommitOptions{});
        }

        const String payload_a = fmt::format("guard bodies {}: the first file", name);
        const String payload_b = fmt::format("guard bodies {}: the second file", name);
        auto limiter_a = std::make_shared<RecordingThrottler>();
        auto limiter_b = std::make_shared<RecordingThrottler>();
        const std::vector<const RecordingThrottler *> limiters{limiter_a.get(), limiter_b.get()};
        const size_t records_before_query = storage->records().size();
        std::set<const FakeGenerationObjectStorage::WriteRecord *> manifests;

        onKillableQueryThread([&](CasKillableQuery & query)
        {
            SCOPED_TRACE(name);
            ASSERT_NE(limitQueryRemoteWrites(query), nullptr) << "the query has no remote write limiter";
            {
                std::optional<OwnerScope> owner;
                if (arm == Arm::Owed)
                    owner.emplace(OwnerScope::owed());
                if (arm == Arm::Shared)
                    owner.emplace(OwnerScope::shared());
                auto tx = mount->createTransaction();
                writeWith(*tx, part + "a.bin", payload_a, limiter_a);
                writeWith(*tx, part + "b.bin", payload_b, limiter_b);
                tx->commit(NoCommitOptions{});
            }

            const ThrottlerPtr body_a = bodyThrottlerOf(*storage, mount->poolForTest(), payload_a);
            const ThrottlerPtr body_b = bodyThrottlerOf(*storage, mount->poolForTest(), payload_b);
            expectReachesOnly(body_a, *limiter_a, limiters);
            expectReachesOnly(body_b, *limiter_b, limiters);

            const auto records = storage->records();
            for (size_t i = records_before_query; i < records.size(); ++i)
                if (!records[i]->copy && isManifestKey(mount->poolForTest(), records[i]->key))
                    manifests.insert(records[i].get());
            ASSERT_FALSE(manifests.empty()) << "the commit wrote no manifest";
            for (const auto * manifest : manifests)
                EXPECT_EQ(manifest->settings.remote_throttler, nullptr) << "the manifest " << manifest->key << " carries a throttler";

            query.kill();
            const int expected = arm == Arm::NewBuild ? ErrorCodes::QUERY_WAS_CANCELLED : 0;
            ASSERT_NE(body_a, nullptr);
            ASSERT_NE(body_b, nullptr);
            EXPECT_EQ(codeOf([&] { body_a->throttle(32768, 0); }), expected);
            EXPECT_EQ(codeOf([&] { body_b->throttle(32768, 0); }), expected);
        });

        const auto pool = mount->poolForTest();
        const std::set<String> bodies{bodyKeyOf(pool, payload_a), bodyKeyOf(pool, payload_b)};
        size_t others = 0;
        for (const auto & record : storage->records())
        {
            if (record->copy || bodies.contains(record->key) || manifests.contains(record.get()))
                continue;
            ++others;
            EXPECT_EQ(record->settings.remote_throttler, nullptr) << "a write of " << record->key << " carries a throttler";
        }
        EXPECT_GT(others, 0u) << "the commit's other writes were not recorded";
    }
}

TEST(CASUploadGuard, AServerSideCopyOfAStagedBodyCarriesNoThrottler)
{
    auto storage = makeFakeGenerationObjectStorageForTest();
    auto mount = mountOver(storage, "mountGuardCopy", /*s3_staging=*/true);
    const String payload = "guard copy: a staged body";
    auto limiter = std::make_shared<RecordingThrottler>();

    onKillableQueryThread([&](CasKillableQuery &)
    {
        auto tx = mount->createTransaction();
        writeWith(*tx, kTable + "all_1_1_0/data.bin", payload, limiter);
        tx->commit(NoCommitOptions{});
    });

    const String body = bodyKeyOf(mount->poolForTest(), payload);
    size_t copies = 0;
    for (const auto & record : storage->records())
    {
        if (!record->copy || record->key != body)
            continue;
        ++copies;
        EXPECT_EQ(record->settings.remote_throttler, nullptr);
    }
    EXPECT_EQ(copies, 1u) << "the publication of a staged body was not a server-side copy";
}

TEST(CASUploadGuard, TheFanOutCarriesTheStopToItsWorkers)
{
    constexpr auto watchdog = std::chrono::seconds(30);
    {
        SCOPED_TRACE("a query killed while a worker sends a body");
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, "mountGuardFanoutStop");
        const auto pool = mount->poolForTest();
        const std::vector<String> payloads{"guard fan-out stop: one", "guard fan-out stop: two", "guard fan-out stop: three"};
        const String stopped = bodyKeyOf(pool, payloads.front());

        /// The hooks live in the storage, which the mount's teardown still writes through after this scope's locals
        /// are gone, so they hold what they use.
        auto in_body = std::make_shared<BoundedEvent>();
        auto killed = std::make_shared<BoundedEvent>();
        storage->throttle_while_writing = [stopped](const String & key) { return key == stopped; };
        storage->before_throttle = [in_body, killed, watchdog](const String &)
        {
            in_body->fire();
            EXPECT_TRUE(killed->wait(watchdog));
        };

        std::atomic<CasKillableQuery *> query_ptr{nullptr};
        int code = 0;
        std::exception_ptr setup_error;
        std::thread publisher([&]
        {
            try
            {
                ThreadStatus thread_status;
                CasKillableQuery query(getContext().context);
                query_ptr = &query;
                auto build = precommittedBuild(*mount, kTable + "all_2_2_0/");
                std::vector<BlobUploadRequest> requests;
                for (const auto & payload : payloads)
                    requests.push_back(requestFor(pool, payload, std::make_shared<RecordingThrottler>()));
                auto workers = makeWorkers(requests.size());
                UploadStopScope scope(uploadStopCheck());
                code = codeOf([&] { fanOutBlobUploads(*build, requests, *workers); });
            }
            catch (...)
            {
                setup_error = std::current_exception();
            }
        });

        const bool entered = in_body->wait(watchdog);
        EXPECT_TRUE(entered) << "no worker sent the body of the stopped blob";
        if (entered)
            query_ptr.load()->kill();
        killed->fire();
        publisher.join();
        ASSERT_FALSE(setup_error) << "the publisher failed: " << getExceptionMessage(setup_error, false);

        const auto writes = writesOf(*storage, stopped);
        ASSERT_EQ(writes.size(), 1u) << "the stopped blob was published again";
        ASSERT_TRUE(writes.front()->throttled) << "the stopped body carries no throttler";
        const std::exception_ptr throttle_error = writes.front()->throttle_error;
        EXPECT_EQ(codeOf([&] { if (throttle_error) std::rethrow_exception(throttle_error); }), ErrorCodes::QUERY_WAS_CANCELLED)
            << "the worker's body did not carry the query's stop";
        EXPECT_EQ(code, ErrorCodes::QUERY_WAS_CANCELLED);
    }
}

TEST(CASUploadGuard, TheLimiterFollowsTheFile)
{
    const String source = kTable + "all_1_1_0/";
    const String destination = kTable + "all_2_2_0/";
    enum class Arm : uint8_t { Hardlink, Move };
    for (const Arm arm : {Arm::Hardlink, Arm::Move})
    {
        const char * name = arm == Arm::Hardlink ? "hardlink" : "move";
        SCOPED_TRACE(name);
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, fmt::format("mountGuardFollows{}", static_cast<int>(arm)));
        const String payload = fmt::format("guard follows the file: {}", name);
        auto limiter = std::make_shared<RecordingThrottler>();

        onKillableQueryThread([&](CasKillableQuery &)
        {
            auto tx = mount->createTransaction();
            writeWith(*tx, source + "data.bin", payload, limiter);
            writeWith(*tx, source + "count.txt", "1", nullptr);
            if (arm == Arm::Hardlink)
            {
                tx->createHardLink(source + "data.bin", destination + "data.bin");
                tx->unlinkFile(source + "data.bin", /*if_exists=*/false, /*should_remove_objects=*/false);
            }
            else
                tx->moveFile(source + "data.bin", destination + "data.bin");
            tx->commit(NoCommitOptions{});
        });

        ASSERT_FALSE(mount->existsFile(source + "data.bin")) << "the source part still names the body";
        ASSERT_TRUE(mount->existsFile(destination + "data.bin"));
        expectReachesOnly(bodyThrottlerOf(*storage, mount->poolForTest(), payload), *limiter, {limiter.get()});
    }

    {
        SCOPED_TRACE("equal bytes written twice into one part");
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, "mountGuardFollowsTwice");
        const String payload = "guard follows the file: equal bytes";
        auto limiter_a = std::make_shared<RecordingThrottler>();
        auto limiter_b = std::make_shared<RecordingThrottler>();

        onKillableQueryThread([&](CasKillableQuery &)
        {
            auto tx = mount->createTransaction();
            writeWith(*tx, source + "a.bin", payload, limiter_a);
            writeWith(*tx, source + "b.bin", payload, limiter_b);
            tx->commit(NoCommitOptions{});
        });

        expectReachesOnly(bodyThrottlerOf(*storage, mount->poolForTest(), payload), *limiter_a, {limiter_a.get(), limiter_b.get()});
    }

    {
        SCOPED_TRACE("an inline candidate above the inline cap");
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, "mountGuardFollowsOversizedInline");
        /// One byte above `INLINE_CAP` of `ContentAddressedTransaction.cpp`, so the file spills to a blob.
        const String payload(1024 * 1024 + 1, 'c');
        auto limiter = std::make_shared<RecordingThrottler>();

        onKillableQueryThread([&](CasKillableQuery &)
        {
            auto tx = mount->createTransaction();
            writeWith(*tx, source + "columns.txt", payload, limiter);
            tx->commit(NoCommitOptions{});
        });

        expectReachesOnly(bodyThrottlerOf(*storage, mount->poolForTest(), payload), *limiter, {limiter.get()});
    }
}

TEST(CASUploadGuard, TheStagingSinkCarriesTheGuardAndKeepsItsDefaults)
{
    enum class Arm : uint8_t { NewBuild, CommittedPart };
    for (const Arm arm : {Arm::NewBuild, Arm::CommittedPart})
    {
        const char * name = arm == Arm::NewBuild ? "new build" : "data file written into a committed part";
        SCOPED_TRACE(name);
        auto storage = makeFakeGenerationObjectStorageForTest();
        auto mount = mountOver(storage, fmt::format("mountGuardSink{}", static_cast<int>(arm)), /*s3_staging=*/true);
        const String part = kTable + "all_1_1_0/";
        if (arm == Arm::CommittedPart)
        {
            auto tx = mount->createTransaction();
            writeWith(*tx, part + "count.txt", "1", nullptr);
            tx->commit(NoCommitOptions{});
        }
        const String sink_prefix = mount->stagingKeyPrefix();
        const auto sinksSince = [&](size_t first)
        {
            WriteRecords sinks;
            const auto records = storage->records();
            for (size_t i = first; i < records.size(); ++i)
                if (!records[i]->copy && records[i]->key.starts_with(sink_prefix))
                    sinks.push_back(records[i]);
            return sinks;
        };

        auto limiter = std::make_shared<RecordingThrottler>();
        onKillableQueryThread([&](CasKillableQuery & query)
        {
            SCOPED_TRACE(name);
            auto tx = mount->createTransaction();

            const size_t before_txn_version = storage->records().size();
            writeWith(*tx, part + "txn_version.txt", "version: 1\n", limiter);
            EXPECT_TRUE(sinksSince(before_txn_version).empty()) << "txn_version.txt opened a staging sink";

            WriteSettings settings;
            settings.remote_throttler = limiter;
            settings.enable_filesystem_cache_log = true;
            const size_t before_data = storage->records().size();
            {
                auto buf = dynamic_cast<ContentAddressedTransaction &>(*tx).writeFile(
                    part + "data.bin", 65536, WriteMode::Rewrite, settings);
                const String payload = fmt::format("guard sink: {}", name);
                buf->write(payload.data(), payload.size());
                buf->finalize();
            }
            const auto sinks = sinksSince(before_data);
            ASSERT_EQ(sinks.size(), 1u) << "the data file did not go through one staging sink";

            WriteSettings stripped = sinks.front()->settings;
            stripped.remote_throttler = nullptr;
            EXPECT_TRUE(stripped == WriteSettings{}) << "the sink's settings differ from the defaults beyond the throttler";
            const ThrottlerPtr sink_throttler = sinks.front()->settings.remote_throttler;
            expectReachesOnly(sink_throttler, *limiter, {limiter.get()});
            tx->commit(NoCommitOptions{});

            query.kill();
            ASSERT_NE(sink_throttler, nullptr);
            EXPECT_EQ(codeOf([&] { sink_throttler->throttle(32768, 0); }), ErrorCodes::QUERY_WAS_CANCELLED);
        });
    }
}

#endif
