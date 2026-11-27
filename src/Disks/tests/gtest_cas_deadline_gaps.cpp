#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/tests/cas_time_taking_backend.h>
#include <IO/ReadBufferFromString.h>
#include <Common/Exception.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using namespace DB::Cas;
using DB::Cas::tests::OperationForTest;
using DB::Cas::tests::TimeTakingBackend;
using DB::Cas::tests::VirtualRetryClock;
using DB::Cas::tests::idOf;

namespace
{

constexpr uint64_t kTenMinutesMs = 10 * 60 * 1000;

using Verb = TimeTakingBackend::Verb;
using Ending = TimeTakingBackend::Ending;

struct OpenedPool
{
    PoolPtr store;
    std::shared_ptr<VirtualRetryClock> clock;
};

/// Every request plane of the pool and the backend's own delays share one virtual clock.
OpenedPool openPoolOn(const std::shared_ptr<TimeTakingBackend> & backend)
{
    /// A pool closed while a build still owes its cleanup leaves no clean farewell; the reopen must not
    /// spend a lease TTL of real time observing the slot.
    PoolPtr store = Pool::open(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .unsafe_remount_no_delay = true});
    auto clock = VirtualRetryClock::installOn(store);
    backend->useClock(clock);
    return {std::move(store), std::move(clock)};
}

PartWriteTxnPtr startBuildFor(const PoolPtr & store, const RootNamespace & ns, const String & ref)
{
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref;
    return store->beginPartWrite(info);
}

ManifestEntry blobManifestEntry(const String & path, const String & payload)
{
    ManifestEntry entry;
    entry.path = path;
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    return entry;
}

std::function<bool(const String &)> exactly(const String & wanted)
{
    return [wanted](const String & key) { return key == wanted; };
}

enum class BodyTransport : uint8_t { Streaming, ServerSideCopy };

const char * transportName(BodyTransport transport)
{
    return transport == BodyTransport::Streaming ? "Streaming" : "ServerSideCopy";
}

/// A re-readable source of `payload`. For a server-side copy it also stores the staging object the
/// copy publishes verbatim: the pool's fixed-length envelope followed by the payload.
BlobSource sourceOf(BodyTransport transport, TimeTakingBackend & backend, const PoolPtr & store, const String & payload)
{
    BlobSource source;
    source.size = payload.size();
    source.open = [payload]() -> std::unique_ptr<DB::ReadBuffer>
    {
        return std::make_unique<DB::ReadBufferFromOwnString>(payload);
    };
    if (transport == BodyTransport::ServerSideCopy)
    {
        const String staging_key = "p/staging/test/" + payload;
        OperationForTest seed(backend);
        const WriteResult seeded
            = (*seed).create(staging_key, String(store->poolMeta().blob_header_len, 'h') + payload, Retry::once());
        EXPECT_TRUE(std::holds_alternative<Committed>(seeded));
        source.server_side_copy_from = staging_key;
    }
    return source;
}

/// Runs `step` and returns the text of whatever it threw, or nothing.
std::optional<String> failureOf(const std::function<void()> & step)
{
    try
    {
        step();
    }
    catch (...)
    {
        return DB::getCurrentExceptionMessage(/*with_stacktrace=*/false);
    }
    return std::nullopt;
}

void publishOnePart(const PoolPtr & store, const RootNamespace & ns, const String & ref, const String & payload)
{
    auto build = startBuildFor(store, ns, ref);
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(ns, ref, build->buildId(), id);
}

/// A pool reopened over a table with two committed parts, so the table's ref state is not in memory.
struct ColdTable
{
    std::shared_ptr<TimeTakingBackend> backend = std::make_shared<TimeTakingBackend>();
    RootNamespace ns{"srv/tbl"};
    OpenedPool pool;

    ColdTable()
    {
        {
            const OpenedPool first = openPoolOn(backend);
            publishOnePart(first.store, ns, "part_a", "payload-a");
            publishOnePart(first.store, ns, "part_b", "payload-b");
        }
        pool = openPoolOn(backend);
    }

    /// The first read of the removal that is not the catalog read belongs to the table's recovery.
    void armSlowRecovery() const
    {
        backend->arm(Verb::Read, [catalog = pool.store->layout().refCatalogKey()](const String & key) { return key != catalog; },
                     kTenMinutesMs);
    }
};

}

/// ---------------------------------------------------------------------------------------------
/// A publication that takes longer than the request window.
/// ---------------------------------------------------------------------------------------------

class CASDeadlineGapsLongPublication : public ::testing::TestWithParam<BodyTransport>
{
};

INSTANTIATE_TEST_SUITE_P(CASTransport, CASDeadlineGapsLongPublication,
    ::testing::Values(BodyTransport::Streaming, BodyTransport::ServerSideCopy),
    [](const ::testing::TestParamInfo<BodyTransport> & param) { return String(transportName(param.param)); });

/// The transfer of a body is bounded by its own send and receive timeouts only, so a publication five
/// times the request window long must still end in a committed part: no request that follows it may be
/// measured against a deadline taken before it started.
TEST_P(CASDeadlineGapsLongPublication, Commits)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    const OpenedPool pool = openPoolOn(backend);
    const RootNamespace ns{"srv/tbl"};
    const String payload = "body-that-takes-ten-minutes";
    const String key = pool.store->layout().blobKey(idOf(payload));

    auto build = startBuildFor(pool.store, ns, "part");
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "part", id);

    backend->arm(Verb::Publish, exactly(key), kTenMinutesMs);
    const uint64_t started_ms = pool.clock->nowMs();
    const auto failure = failureOf([&]
    {
        build->putBlob(idOf(payload), sourceOf(GetParam(), *backend, pool.store, payload));
        build->promote(ns, "part", build->buildId(), id);
    });

    ASSERT_EQ(backend->slowedKeys(), std::vector<String>{key}) << "the publication never ran slowly";
    ASSERT_GT(pool.clock->nowMs() - started_ms, kDefaultRequestWindowMs);
    ASSERT_FALSE(failure.has_value()) << *failure;
    EXPECT_EQ(backend->publicationsOf(key), 1u);
    EXPECT_EQ(backend->serverSideCopiesOf(key), GetParam() == BodyTransport::ServerSideCopy ? 1u : 0u);
    EXPECT_TRUE(pool.store->resolveRef(ns, "part").has_value());
}

namespace
{

struct LostPublication
{
    BodyTransport transport;
    bool landed;
};

class CASDeadlineGapsLostLongPublication : public ::testing::TestWithParam<LostPublication>
{
};

}

INSTANTIATE_TEST_SUITE_P(CASTransport, CASDeadlineGapsLostLongPublication,
    ::testing::Values(
        LostPublication{BodyTransport::Streaming, false}, LostPublication{BodyTransport::Streaming, true},
        LostPublication{BodyTransport::ServerSideCopy, false}, LostPublication{BodyTransport::ServerSideCopy, true}),
    [](const ::testing::TestParamInfo<LostPublication> & param)
    {
        return String(transportName(param.param.transport)) + (param.param.landed ? "Landed" : "NotLanded");
    });

/// The publication loop allows eight attempts. A first attempt that ran for ten minutes before its
/// response was lost has used none of the time a reissue needs: the next observation of the key, and the
/// publication after it when the body did not land, must still be sent.
TEST_P(CASDeadlineGapsLostLongPublication, IsReissuedInPlaceAndCommits)
{
    const LostPublication lost = GetParam();
    auto backend = std::make_shared<TimeTakingBackend>();
    const OpenedPool pool = openPoolOn(backend);
    const RootNamespace ns{"srv/tbl"};
    const String payload = "body-whose-first-response-is-lost";
    const String key = pool.store->layout().blobKey(idOf(payload));

    auto build = startBuildFor(pool.store, ns, "part");
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "part", id);

    backend->arm(Verb::Publish, exactly(key), kTenMinutesMs, /*times=*/1,
                 lost.landed ? Ending::LostAfterLanding : Ending::LostBeforeLanding);
    const auto failure = failureOf([&]
    {
        build->putBlob(idOf(payload), sourceOf(lost.transport, *backend, pool.store, payload));
        build->promote(ns, "part", build->buildId(), id);
    });

    ASSERT_EQ(backend->slowedKeys(), std::vector<String>{key}) << "the publication never ran slowly";
    ASSERT_FALSE(failure.has_value())
        << "after " << backend->publicationsOf(key) << " publication(s) the build failed: " << *failure;
    /// A stored body has no deletion intent, so the reissue adopts it; an absent one is published again.
    EXPECT_EQ(backend->publicationsOf(key), lost.landed ? 1u : 2u);
    EXPECT_TRUE(pool.store->resolveRef(ns, "part").has_value());
}

/// A part of sixteen bodies whose last upload takes ten minutes. The fifteen dependency proofs taken
/// before it are consumed by the promote ten minutes later, and nothing may re-measure them against
/// the time they were taken at.
TEST(CASDeadlineGaps, BodiesPublishedBeforeASlowOneAreStillAcceptedAtPromote)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    const OpenedPool pool = openPoolOn(backend);
    const RootNamespace ns{"srv/tbl"};
    constexpr size_t bodies = 16;

    std::vector<String> payloads;
    std::vector<ManifestEntry> entries;
    for (size_t i = 0; i < bodies; ++i)
    {
        payloads.push_back(fmt::format("body-{}-of-a-wide-part", i));
        entries.push_back(blobManifestEntry(fmt::format("column_{}.bin", i), payloads.back()));
    }
    const String slow_key = pool.store->layout().blobKey(idOf(payloads.back()));

    auto build = startBuildFor(pool.store, ns, "part");
    const ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, "part", id);

    backend->arm(Verb::Publish, exactly(slow_key), kTenMinutesMs);
    std::vector<BlobUploadResult> results;
    const auto failure = failureOf([&]
    {
        /// The order of the upload fan-out's join: every result is kept until the slowest task ends.
        for (const String & payload : payloads)
            results.push_back(build->uploadBlobDetached(
                BlobUploadRequest{idOf(payload), BlobSource::fromString(payload), payload.size()}));
        build->mergeBlobUploadResults(results);
        build->promote(ns, "part", build->buildId(), id);
    });

    ASSERT_EQ(backend->slowedKeys(), std::vector<String>{slow_key}) << "the last publication never ran slowly";
    ASSERT_FALSE(failure.has_value()) << *failure;
    for (const String & payload : payloads)
        EXPECT_EQ(backend->publicationsOf(pool.store->layout().blobKey(idOf(payload))), 1u);
    EXPECT_TRUE(pool.store->resolveRef(ns, "part").has_value());
}

/// GC rounds run while a body uploads for ten minutes. The build's precommit was durable before the
/// upload started, so neither the manifest it staged nor the sibling body it already published may be
/// reclaimed, and the promote that follows must find its precommit.
TEST(CASDeadlineGaps, GcRoundsDuringALongPublicationSpareTheBuild)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    const OpenedPool pool = openPoolOn(backend);
    const RootNamespace ns{"srv/tbl"};
    const String payload_a = "body-a-published-before-the-long-upload";
    const String payload_b = "body-b-uploads-for-ten-minutes";
    const String key_a = pool.store->layout().blobKey(idOf(payload_a));
    const String key_b = pool.store->layout().blobKey(idOf(payload_b));

    auto build = startBuildFor(pool.store, ns, "part");
    const ManifestId id = build->stageManifest({blobManifestEntry("a.bin", payload_a), blobManifestEntry("b.bin", payload_b)});
    build->precommitAdd(ns, "part", id);
    build->putBlob(idOf(payload_a), BlobSource::fromString(payload_a));

    Gc gc(pool.store, hexToU128("00000000000000000000000000000001"));
    size_t rounds_during_upload = 0;
    backend->armWith(Verb::Publish, exactly(key_b), [&]
    {
        for (size_t i = 0; i < 3; ++i)
        {
            pool.clock->advance(kTenMinutesMs / 3);
            /// What the renewal thread does every period while the upload runs.
            pool.store->renewWatermarkOnce();
            (void)DB::Cas::tests::runRegularRoundReclaiming(gc);
            ++rounds_during_upload;
        }
    });
    const auto failure = failureOf([&]
    {
        build->putBlob(idOf(payload_b), BlobSource::fromString(payload_b));
        build->promote(ns, "part", build->buildId(), id);
    });

    ASSERT_EQ(rounds_during_upload, 3u) << "no GC round ran during the publication";
    ASSERT_FALSE(failure.has_value()) << *failure;
    for (size_t i = 0; i < 3; ++i)
    {
        pool.store->renewWatermarkOnce();
        (void)DB::Cas::tests::runRegularRoundReclaiming(gc);
    }
    EXPECT_EQ(backend->publicationsOf(key_a), 1u);
    EXPECT_EQ(backend->publicationsOf(key_b), 1u);
    EXPECT_TRUE(DB::Cas::tests::objectPresent(*backend, key_a));
    EXPECT_TRUE(DB::Cas::tests::objectPresent(*backend, key_b));
    ASSERT_TRUE(pool.store->resolveRef(ns, "part").has_value());
    EXPECT_EQ(pool.store->readManifest(id).entries.size(), 2u);
}

/// One upload outlives twenty mount lease TTLs. The lease is renewed every period by its own thread
/// and a renewal does not move the fence generation, so the build admitted before the upload is still
/// admitted after it and commits.
TEST(CASDeadlineGaps, ABuildSurvivesAnUploadLongerThanManyLeaseTtls)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    /// One clock for the lease and for every request window, as on a server.
    auto boot_ms = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    const PoolPtr store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test",
        .boot_ms_fn = [boot_ms] { return boot_ms->load(); }});
    store->setCasRetrySleepForTest([boot_ms](uint64_t ms) { boot_ms->fetch_add(ms + 1); });
    const uint64_t ttl_ms = static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count());
    const uint64_t period_ms = static_cast<uint64_t>(store->poolConfig().mount_renew_period.count());

    const RootNamespace ns{"srv/tbl"};
    const String payload = "body-that-outlives-the-lease";
    const String key = store->layout().blobKey(idOf(payload));
    auto build = startBuildFor(store, ns, "part");
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "part", id);

    const uint64_t generation_before = store->fenceGeneration();
    const uint64_t started_ms = boot_ms->load();
    backend->armWith(Verb::Publish, exactly(key), [&]
    {
        for (uint64_t elapsed_ms = 0; elapsed_ms < kTenMinutesMs; elapsed_ms += period_ms)
        {
            boot_ms->fetch_add(period_ms);
            store->renewWatermarkOnce();
        }
    });
    const auto failure = failureOf([&]
    {
        build->putBlob(idOf(payload), BlobSource::fromString(payload));
        build->promote(ns, "part", build->buildId(), id);
    });

    ASSERT_EQ(backend->slowedKeys(), std::vector<String>{key}) << "the publication never ran slowly";
    ASSERT_GE(boot_ms->load() - started_ms, 20 * ttl_ms);
    ASSERT_FALSE(failure.has_value()) << *failure;
    EXPECT_EQ(store->fenceGeneration(), generation_before);
    EXPECT_EQ(backend->publicationsOf(key), 1u);
    EXPECT_TRUE(store->resolveRef(ns, "part").has_value());
}

/// The eighth publication is the last the loop makes. When its response is lost after the body landed
/// the loop does not observe the key again: the write fails and the caller's retry adopts the body.
TEST(CASDeadlineGaps, ABodyThatLandsOnTheEighthLostPublicationStillFails)
{
    auto backend = std::make_shared<TimeTakingBackend>();
    const OpenedPool pool = openPoolOn(backend);
    const RootNamespace ns{"srv/tbl"};
    const String payload = "body-landing-on-the-last-attempt";
    const String key = pool.store->layout().blobKey(idOf(payload));

    auto build = startBuildFor(pool.store, ns, "part");
    const ManifestId id = build->stageManifest({blobManifestEntry("data.bin", payload)});
    build->precommitAdd(ns, "part", id);

    size_t journal_at_eighth = 0;
    backend->arm(Verb::Publish, exactly(key), /*takes_ms=*/0, /*times=*/7, Ending::LostBeforeLanding);
    backend->armWith(Verb::Publish, exactly(key), [&] { journal_at_eighth = backend->seenCount(); }, /*times=*/1,
                     Ending::LostAfterLanding);
    const auto failure = failureOf([&] { build->putBlob(idOf(payload), BlobSource::fromString(payload)); });

    ASSERT_TRUE(failure.has_value()) << "the loop must not commit a body whose last publication was lost";
    EXPECT_NE(failure->find("remained ambiguous"), String::npos) << *failure;
    EXPECT_EQ(backend->publicationsOf(key), 8u);
    EXPECT_TRUE(DB::Cas::tests::objectPresent(*backend, key));
    EXPECT_EQ(backend->seenSince(journal_at_eighth, Verb::Head, key), 0u) << "the loop observed the key after its last publication";
}

/// ---------------------------------------------------------------------------------------------
/// A table removal whose ref-table recovery takes longer than the request window.
/// ---------------------------------------------------------------------------------------------

/// Recovery of a table reads its checkpoint, snapshot and log tail, and its length grows with the
/// table. The removal's catalog read and its `Live -> Removing` write come after it and are single
/// requests, so they need a window of their own.
TEST(CASDeadlineGaps, RemovalOfATableWhoseRecoveryOutlastsTheWindowCompletes)
{
    const ColdTable table;
    const PoolPtr & store = table.pool.store;
    ASSERT_FALSE(store->refTableRecoveredForTest(table.ns)) << "the table must be recovered by the removal itself";

    table.armSlowRecovery();
    const uint64_t started_ms = table.pool.clock->nowMs();
    std::optional<uint64_t> recovery_took_ms;
    store->setRemovalAttemptHookForTest([&] { recovery_took_ms = table.pool.clock->nowMs() - started_ms; });
    const auto failure = failureOf([&] { store->dropNamespace(table.ns); });
    store->setRemovalAttemptHookForTest({});

    ASSERT_EQ(table.backend->slowedKeys().size(), 1u) << "no read of the recovery ran slowly";
    ASSERT_TRUE(recovery_took_ms.has_value()) << "the removal never reached its first request";
    ASSERT_GT(*recovery_took_ms, kDefaultRequestWindowMs);
    ASSERT_FALSE(failure.has_value()) << *failure;
    EXPECT_FALSE(store->resolveRef(table.ns, "part_a").has_value());
    EXPECT_FALSE(store->resolveRef(table.ns, "part_b").has_value());
}

/// The recovery a failed removal paid for stays in memory, so the next removal of the same table has
/// nothing long in front of its requests. The first removal fails at its `gc/state` read, after the
/// recovery and before any catalog write.
TEST(CASDeadlineGaps, RepeatedRemovalAfterASlowRecoveryCompletes)
{
    const ColdTable table;
    const PoolPtr & store = table.pool.store;
    const String gc_state_key = store->layout().gcStateKey();
    const String catalog_key = store->layout().refCatalogKey();

    table.armSlowRecovery();
    auto gc_state_fails = std::make_shared<std::atomic<bool>>(false);
    table.backend->arm(Verb::Read, [gc_state_fails, gc_state_key](const String & key)
    {
        return gc_state_fails->load() && key == gc_state_key;
    }, /*takes_ms=*/0, std::numeric_limits<size_t>::max(), TimeTakingBackend::Ending::LostBeforeLanding);
    store->setRemovalAttemptHookForTest([gc_state_fails] { gc_state_fails->store(true); });
    const size_t requests_before = table.backend->seenCount();
    const auto first_failure = failureOf([&] { store->dropNamespace(table.ns); });
    store->setRemovalAttemptHookForTest({});
    gc_state_fails->store(false);

    ASSERT_TRUE(first_failure.has_value()) << "the first removal must fail";
    EXPECT_GT(table.backend->seenSince(requests_before, Verb::Read, gc_state_key), 0u) << "the gc/state read was never sent";
    ASSERT_EQ(table.backend->seenSince(requests_before, Verb::Write, catalog_key), 0u) << "a catalog write was sent";
    const std::vector<String> slowed = table.backend->slowedKeys();
    ASSERT_EQ(std::count_if(slowed.begin(), slowed.end(), [&](const String & key) { return key != gc_state_key; }), 1)
        << "no read of the recovery ran slowly";

    ASSERT_TRUE(store->refTableRecoveredForTest(table.ns)) << "the first removal left the table unrecovered";
    const uint64_t recoveries_before = store->recoveryInstallCountForTest();
    const auto failure = failureOf([&] { store->dropNamespace(table.ns); });
    ASSERT_FALSE(failure.has_value()) << *failure;
    EXPECT_EQ(store->recoveryInstallCountForTest(), recoveries_before) << "the repeated removal recovered the table again";
    EXPECT_FALSE(store->resolveRef(table.ns, "part_a").has_value());
    EXPECT_FALSE(store->resolveRef(table.ns, "part_b").has_value());
}
