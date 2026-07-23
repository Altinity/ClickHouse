#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

/// Task 10 (rev.7 spec §5): `SYSTEM CONTENT ADDRESSED FORGET` — the operator force-Vanish. FORGET drives a
/// content-addressed pool to `Vanished(forgotten)` with the fence-first protocol: (1) publish terminal
/// intent, (2) trip the local fence, (3+4) stop the GC scheduler, (5) join keeper/remount, drain, retire
/// the keeper WITHOUT an unearned clean farewell, (6) publish `Vanished(forgotten)` with the [D5] message
/// carrying the decommission timestamp. These tests exercise the Pool-level protocol body (`Pool::forgetDisk`)
/// and the end-to-end verb through a real `ContentAddressedMetadataStorage` (the six-class gate wired to the
/// new state). Harness patterns follow gtest_cas_lifecycle_condition.cpp and gtest_cas_operation_gate.cpp.

namespace DB::ErrorCodes
{
extern const int INVALID_STATE;
}

using namespace DB;
using DB::Cas::PoolLifecycle;
using DB::Cas::tests::CountingBackend;

namespace
{

const String kSrid = "test";

/// A test-authored [D5] reason with a RECOGNIZABLE timestamp — the Pool-level tests assert this exact
/// string flows through `enterVanished` into the `throwIfLifecycleTerminal` message (the timestamp
/// threading the metadata storage does in production). It keeps the two [D5] substrings the gate relies on.
const String kForgetReason =
    "decommissioned by SYSTEM CONTENT ADDRESSED FORGET at 2099-01-02 03:04:05 UTC — erasure was NOT "
    "verified; if this was a mistake the data may be intact (restart re-registers the name)";

/// Delete an existing key exactly (its current token comes from the same GET). Mirrors
/// gtest_cas_lifecycle_condition.cpp — used to drive a live pool into `IdentityLost`.
void deleteKeyExact(DB::Cas::Backend & backend, const String & key)
{
    const auto got = backend.get(key);
    ASSERT_TRUE(got.has_value()) << "expected '" << key << "' to exist before deletion";
    if (got)
        backend.deleteExact(key, got->token);
}

/// A Backend decorator whose head/get/list throw an untyped transport error while `fail` is armed — so a
/// self-remount attempt verdicts `StayTransient` (fast, no lease-expiry wait) and the remount loop keeps
/// spinning. Starts DISARMED so `Pool::open` succeeds. Mirrors gtest_cas_lifecycle_condition.cpp's decorator.
class ToggleableTransportFaultBackend final : public DB::Cas::InMemoryBackend
{
public:
    using Backend::get;
    using Backend::getStream;
    using Backend::putIfAbsent;
    using Backend::putIfAbsentStream;
    using Backend::putOverwrite;
    using Backend::casPut;

    DB::Cas::HeadResult head(const String & key) override
    {
        if (fail.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::head(key);
    }
    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range) override
    {
        if (fail.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::get(key, range);
    }
    DB::Cas::ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (fail.load())
            throw std::runtime_error("injected fault: transport error");
        return InMemoryBackend::list(prefix, cursor, limit);
    }

    std::atomic<bool> fail{false};
};

/// The message thrown by `fn`, or a failure if it did not throw a `DB::Exception`.
std::string messageOf(const std::function<void()> & fn)
{
    try
    {
        fn();
    }
    catch (const Exception & e)
    {
        return std::string(e.message());
    }
    ADD_FAILURE() << "expected a DB::Exception";
    return {};
}

/// A live table dir + committed part reused by the end-to-end gate test (the shape
/// gtest_cas_operation_gate.cpp uses).
const std::string kTableDir = "gg0/gg0gg0g0-0808-4808-8808-080808080808";
const std::string kPartDir = kTableDir + "/all_1_1_0";
const std::string kPartFile = kPartDir + "/data.bin";

std::shared_ptr<ContentAddressedMetadataStorage> openForgetStorage()
{
    auto settings = Cas::tests::makeSettingsForTest(
        "test", std::filesystem::temp_directory_path() / "ca_forget_scratch");
    auto storage = std::make_shared<ContentAddressedMetadataStorage>(
        Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

void commitOnePart(ContentAddressedMetadataStorage & storage)
{
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<ContentAddressedTransaction &>(*tx);
    auto buf = ca_tx.writeFile(kTableDir + "/tmp_insert_all_1_1_0/data.bin", 65536, WriteMode::Rewrite, {});
    const std::string bytes = "content-of-the-part";
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
    tx->moveDirectory(kTableDir + "/tmp_insert_all_1_1_0", kPartDir);
    tx->commit(NoCommitOptions{});
}

}

/// (a) FORGET on a LIVE pool: the local fence is tripped, the injected GC-stop step runs, the pool settles
/// `Vanished(forgotten)`, and store-class access fails loud with the timestamped [D5] message.
TEST(CasForget, ForgetOnLivePoolTripsFenceAndVanishes)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);
    ASSERT_EQ(store->lifecycle(), PoolLifecycle::Live);
    ASSERT_TRUE(store->mayMutate());

    bool gc_stopped = false;
    store->forgetDisk([&] { gc_stopped = true; }, kForgetReason);

    /// Step 3/4 ran (the GC-stop callback was invoked from inside the protocol).
    EXPECT_TRUE(gc_stopped);
    /// Terminal truth, fence tripped.
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_TRUE(store->isVanished());
    EXPECT_FALSE(store->mayMutate());

    /// The [D5] message carries the operator's FORGET timestamp (threaded through the reason) and still
    /// names the sub-state ("erasure was NOT verified").
    const std::string msg = messageOf([&] { store->throwIfLifecycleTerminal(); });
    EXPECT_NE(msg.find("SYSTEM CONTENT ADDRESSED FORGET at "), std::string::npos) << msg;
    EXPECT_NE(msg.find("2099-01-02 03:04:05 UTC"), std::string::npos) << msg;
    EXPECT_NE(msg.find("erasure was NOT verified"), std::string::npos) << msg;
}

/// (a') FORGET stops AND joins a real `CasGcScheduler`'s worker + heartbeat threads (the injected GC-stop
/// step). A long interval keeps any round from firing during the test window, so this isolates the
/// thread-lifecycle: `start()` spawns the two workers, FORGET's callback `stop()`s + joins them, and the
/// test completing (no hang) plus a clean `isQuiescent()` proves the join.
TEST(CasForget, ForgetStopsAndJoinsRealGcScheduler)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);

    Cas::CasGcScheduler sched(store, std::chrono::seconds(3600), "CasForgetTest", "forget-disk");
    sched.start();

    bool gc_joined = false;
    store->forgetDisk([&] { sched.stop(); gc_joined = true; }, kForgetReason);

    EXPECT_TRUE(gc_joined);
    EXPECT_TRUE(sched.isQuiescent()) << "no GC round may be in flight after FORGET joined the scheduler";
    EXPECT_FALSE(sched.gcHealth().is_leader);
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
}

/// (c) Double FORGET is idempotent: the second call is a no-op (the pool is already `Vanished(forgotten)`),
/// so it never re-runs the protocol — the GC-stop callback is NOT invoked again, and the first reason wins.
TEST(CasForget, DoubleForgetIsIdempotent)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);

    int gc_stops = 0;
    store->forgetDisk([&] { ++gc_stops; }, kForgetReason);
    ASSERT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    ASSERT_EQ(gc_stops, 1);

    /// A second FORGET with a DIFFERENT reason must change nothing (first terminal transition wins) and
    /// must NOT re-enter the teardown (idempotent short-circuit on `isVanished()`).
    store->forgetDisk([&] { ++gc_stops; }, "a different reason that must be ignored");
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_EQ(gc_stops, 1) << "the idempotent second FORGET must not re-run the protocol";

    const std::string msg = messageOf([&] { store->throwIfLifecycleTerminal(); });
    EXPECT_NE(msg.find("2099-01-02 03:04:05 UTC"), std::string::npos)
        << "the first FORGET's reason must win: " << msg;
}

/// (d) FORGET on an `IdentityLost` pool → `Vanished(forgotten)` — the escape hatch. `IdentityLost` is
/// non-absorbing and has no benign answer, so FORGET is the operator's way out.
TEST(CasForget, ForgetOnIdentityLostPoolVanishesForgotten)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);

    /// Delete both pool sentinels while other objects remain, then drive the identity gate: the pool enters
    /// `IdentityLost` (never `Vanished`) — exactly gtest_cas_lifecycle_condition.cpp scenario (a).
    deleteKeyExact(*backend, store->layout().poolMetaKey());
    deleteKeyExact(*backend, store->layout().ownerKey(kSrid));
    EXPECT_FALSE(store->tryRemountOnce());
    ASSERT_EQ(store->lifecycle(), PoolLifecycle::IdentityLost);
    ASSERT_FALSE(store->isVanished());

    bool gc_stopped = false;
    store->forgetDisk([&] { gc_stopped = true; }, kForgetReason);

    EXPECT_TRUE(gc_stopped);
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_TRUE(store->isVanished());
}

/// (a'') The clean-farewell is EARNED, never unconditional: on a drained pool FORGET stamps the mount lease
/// with the terminated sentinel (`min_active == UINT64_MAX`) so a same-server restart reclaims immediately,
/// but with an UNSETTLED (wedged) ref lane it must NOT — the lease is left to expire by observation.
TEST(CasForget, ForgetCleanFarewellGatedOnDrain)
{
    using DB::Cas::decodeMountLease;
    constexpr uint64_t kTerminated = std::numeric_limits<uint64_t>::max();

    /// Drained pool → clean farewell written (lease stamped terminated).
    {
        auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
        auto store = DB::Cas::tests::openPoolForTest(backend);
        const String mount_key = store->layout().mountKey(kSrid);
        ASSERT_NE(decodeMountLease(backend->get(mount_key)->bytes).min_active, kTerminated);   /// baseline

        store->forgetDisk([] {}, kForgetReason);
        ASSERT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);

        const auto got = backend->get(mount_key);
        ASSERT_TRUE(got.has_value());
        EXPECT_EQ(decodeMountLease(got->bytes).min_active, kTerminated)
            << "a drained FORGET earns the clean-release farewell";
    }

    /// Unsettled (wedged) ref lane → NO clean farewell (the drain cannot certify a clean death).
    {
        auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
        auto store = DB::Cas::tests::openPoolForTest(backend);
        const String mount_key = store->layout().mountKey(kSrid);

        const DB::Cas::RootNamespace ns{"test/forget_wedge"};
        store->forceWedgeForTest(ns, /*writer_epoch*/ 1, /*ref_sequence*/ 1, "bogus/_log/key", "bogus-bytes");
        ASSERT_TRUE(store->refLaneWedgedForTest(ns));

        store->forgetDisk([] {}, kForgetReason);
        ASSERT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);

        const auto got = backend->get(mount_key);
        ASSERT_TRUE(got.has_value()) << "the lease object must still be present (expiry by observation)";
        EXPECT_NE(decodeMountLease(got->bytes).min_active, kTerminated)
            << "an unearned clean farewell must NOT be written when the ref lanes did not drain";
    }
}

/// (b) FORGET racing an ACTIVE in-flight self-remount completes without deadlock, and lands the terminal
/// state with the fence latched (`mayMutate() == false`) even though a raced reclaim may have briefly
/// re-armed it. A real remount thread spins against a faulting backend; FORGET runs on another thread and
/// must join it in bounded time. Uses a `std::future` timeout wait (never a sleep) — the timeout only
/// fires on a genuine deadlock regression.
TEST(CasForget, ForgetRacingActiveRemountThreadCompletesBounded)
{
    auto backend = std::make_shared<ToggleableTransportFaultBackend>();
    /// `background_watermark = true` so `scheduleRemount` actually spawns a recovery thread (mirrors
    /// gtest_cas_pool.cpp's ShutdownGuardRefusesToArmRemount setup).
    auto store = DB::Cas::Pool::open(backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .background_watermark = true});

    /// Arm the fault so every remount attempt verdicts `StayTransient` fast (no lease-expiry wait), then
    /// trip the fence and spawn the recovery thread — it now loops `tryRemountOnce` against the fault.
    backend->fail.store(true);
    store->tripMountLost();
    ASSERT_TRUE(store->scheduleRemountForTest()) << "the recovery thread must be armed and running";

    /// FORGET from ANOTHER thread must join the active remount thread and finish in bounded time.
    std::promise<void> done;
    auto fut = done.get_future();
    std::thread forgetter([&]
    {
        store->forgetDisk([] {}, kForgetReason);
        done.set_value();
    });
    EXPECT_EQ(fut.wait_for(std::chrono::seconds(30)), std::future_status::ready)
        << "FORGET must not deadlock against an in-flight self-remount";
    forgetter.join();

    /// Disarm before ~Pool so its residual teardown is not fighting the injected fault.
    backend->fail.store(false);

    EXPECT_EQ(store->lifecycle(), PoolLifecycle::VanishedForgotten);
    EXPECT_FALSE(store->mayMutate()) << "the fence must stay latched even if a raced reclaim re-armed it";
}

/// (e) End-to-end through the verb entry `ContentAddressedMetadataStorage::forgetDisk` and the six-class
/// gate: after FORGET, a Probe answers truth-absent, a Remove no-ops, and a content read throws the [D5]
/// message with the REAL decommission timestamp produced by the handler.
TEST(CasForget, ForgetEndToEndGatesTruthWithTimestampedMessage)
{
    auto storage = openForgetStorage();
    commitOnePart(*storage);
    ASSERT_TRUE(storage->existsFile(kPartFile));   /// Live baseline

    storage->forgetDisk();

    /// Probe → truth-absent (no throw): the committed part reads absent on a forgotten disk.
    EXPECT_FALSE(storage->existsFile(kPartFile));
    EXPECT_FALSE(storage->existsDirectory(kPartDir));

    /// Remove → no-op success (this is what lets a forgotten-disk table's DROP complete).
    EXPECT_NO_THROW({
        auto tx = storage->createTransaction();
        tx->removeRecursive(kTableDir, /*should_remove_objects=*/nullptr);
        tx->commit(NoCommitOptions{});
    });

    /// Content read → the typed [D5] message, with the handler's real UTC timestamp.
    const std::string msg = messageOf([&] { storage->getFileSize(kPartFile); });
    EXPECT_NE(msg.find("SYSTEM CONTENT ADDRESSED FORGET at "), std::string::npos) << msg;
    EXPECT_NE(msg.find(" UTC"), std::string::npos) << msg;
    EXPECT_NE(msg.find("erasure was NOT verified"), std::string::npos) << msg;
}
