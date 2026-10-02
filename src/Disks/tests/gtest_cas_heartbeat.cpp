#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <base/scope_guard.h>

#include "config.h"
#include <IO/S3Common.h>

#include <atomic>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}

using namespace DB::Cas;


/// MountLeaseRenewer behavior: the per-server mount lease and the merged build-watermark floor ride the
/// SAME slot, renewed by one beat. The renewer anchors durably before return, adopts a slot already
/// written by `claimMount` (same uuid+epoch), re-reads the callback on each renew and bumps `seq`,
/// stamps the farewell sentinel (`min_active_build_sequence = UINT64_MAX`, `expires_at_ms <= now`) on `release`, and
/// returns typed terminal results on any foreign touch.

namespace
{
/// The request planes this file's renewers run on. All are open-fence -- the exclusivity these tests
/// exercise is the mount protocol's own, not a fence's -- on the same injected boot clock the renewer's
/// lease deadline is expressed on, so they never disagree about how much budget is left.
/// `tests::OperationForTest` covers a fixture needing one operation, but neither the planes a renewer
/// takes nor this clock, which is why this stays local.
class Ops
{
public:
    Ops(std::shared_ptr<Backend> backend, uint64_t * boot_ms)
        : mount(openRequestsForTest(backend))
        , farewell(openRequestsForTest(backend))
        , lease(openRequestsForTest(std::move(backend)))
        , op(mount.admit())
    {
        for (CasRequests * requests : {&mount, &farewell, &lease})
        {
            requests->setNowFnForTest([boot_ms] { return *boot_ms; });
            requests->setSleepFnForTest([boot_ms](uint64_t ms) { *boot_ms += ms; });
        }
    }

    Ops(const Ops &) = delete;
    Ops & operator=(const Ops &) = delete;

    CasRequests mount;
    CasRequests farewell;
    CasRequests lease;
    CasOperation op;
};

/// A fixture write that must land, so a mis-seeded fixture fails where it is written rather than in
/// the assertion it silently invalidated.
void mustCommit(WriteResult && result, const String & what)
{
    if (!std::holds_alternative<Committed>(result))
        throw DB::Exception(DB::ErrorCodes::ABORTED, "test fixture write '{}' did not commit", what);
}

/// The normal steady-state flow: `claimMount` writes the live (uuid, epoch) mount, THEN the renewer
/// adopts it. Seed that claim so `start` adopts instead of self-tripping the double-start guard.
void seedOwnClaim(CasOperation & op, const Layout & l, const String & srid, UInt128 uuid, uint64_t epoch,
                  uint64_t now_ms, uint64_t ttl_ms)
{
    ASSERT_EQ(claimMount(op, l, srid, uuid, epoch, now_ms, ttl_ms).kind, MountClaimResult::Claimed);
}

class RenewalScriptBackend final : public InMemoryBackend
{
public:
    enum class Action : uint8_t
    {
        Delegate,
        ThrowBefore,
        LandThenThrow,
        ReturnThenCancel,
        ThrowBeforeThenLandAfterResolve,
        ThrowConnectHint,
        ThrowFirstAttemptFuse,
        ThrowStoreRefusal,
    };

    struct Attempt
    {
        String key;
        String bytes;
        std::optional<String> expected;
    };

    std::deque<Action> actions;
    std::vector<Attempt> attempts;
    std::function<void()> cancel_after_write;
    uint64_t read_calls = 0;
    /// Consulted when `actions` is empty: TRUE fails the guarded write with `outage_action`.
    std::function<bool()> outage;
    Action outage_action = Action::ThrowBefore;
    /// Scripted answers for reads of a mount slot: `ThrowBefore` and `ThrowFirstAttemptFuse` fail the
    /// read, `Delegate` serves it. Consulted before `read_outage`.
    std::deque<Action> read_actions;
    /// TRUE fails a read of a mount slot with a transport timeout.
    std::function<bool()> read_outage;
    /// Called on every scripted write and on every read of a mount slot, before it is answered.
    std::function<void()> on_attempt;
    std::function<void()> on_read;

    /// Only a GUARDED write of a mount slot is scripted; the fixture's own seeding and every other
    /// key reach the store untouched.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (!expected_value || !key.ends_with("/mount"))
            return InMemoryBackend::write(key, bytes, expected_value, access);

        attempts.push_back({key, bytes, expected_value});
        if (on_attempt)
            on_attempt();
        Action action = Action::Delegate;
        if (!actions.empty())
        {
            action = actions.front();
            actions.pop_front();
        }
        else if (outage && outage())
        {
            action = outage_action;
        }

        if (action == Action::ThrowConnectHint)
        {
#if USE_AWS_S3
            throw DB::S3Exception("Poco::Exception. Code: 1000, e.code() = 99, Cannot assign requested address: 10.0.0.1:9000",
                                  Aws::S3::S3Errors::NETWORK_CONNECTION);
#else
            throw Poco::TimeoutException("connect timed out");
#endif
        }
        if (action == Action::ThrowFirstAttemptFuse)
            throwFirstAttemptFuse();
        if (action == Action::ThrowStoreRefusal)
        {
#if USE_AWS_S3
            throw DB::S3Exception("the store answered MalformedXML", Aws::S3::S3Errors::UNKNOWN, "MalformedXML");
#else
            throw DB::Exception(DB::ErrorCodes::ABORTED, "a store refusal needs USE_AWS_S3");
#endif
        }

        if (action == Action::ThrowBefore || action == Action::ThrowBeforeThenLandAfterResolve)
        {
            if (action == Action::ThrowBeforeThenLandAfterResolve)
                pending = Attempt{key, bytes, expected_value};
            throw Poco::TimeoutException("injected renewal response uncertainty before a result");
        }

        auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (action == Action::LandThenThrow)
        {
            if (cancel_after_write)
                cancel_after_write();
            throw Poco::TimeoutException("injected renewal response loss after commit");
        }
        if (action == Action::ReturnThenCancel && cancel_after_write)
            cancel_after_write();
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        ++read_calls;
        if (key.ends_with("/mount"))
        {
            if (on_read)
                on_read();
            if (!read_actions.empty())
            {
                const Action action = read_actions.front();
                read_actions.pop_front();
                if (action == Action::ThrowFirstAttemptFuse)
                    throwFirstAttemptFuse();
                if (action == Action::ThrowBefore)
                    throw Poco::TimeoutException("injected renewal read failure");
            }
            else if (read_outage && read_outage())
            {
                throw Poco::TimeoutException("injected renewal read outage");
            }
        }
        std::optional<Raw> result = InMemoryBackend::read(key, access);
        if (pending && pending->key == key)
        {
            const Attempt delayed = *pending;
            pending.reset();
            const auto landed = InMemoryBackend::write(delayed.key, delayed.bytes, delayed.expected, access);
            if (!landed.has_value())
                throw DB::Exception(DB::ErrorCodes::ABORTED, "injected delayed renewal did not land");
        }
        return result;
    }

private:
    /// The adaptive first-attempt timeout the engine reissues at once.
    [[noreturn]] static void throwFirstAttemptFuse()
    {
#if USE_AWS_S3
        throw DB::S3Exception("Timeout", Aws::S3::S3Errors::NETWORK_CONNECTION);
#else
        throw Poco::TimeoutException("first-attempt fuse");
#endif
    }

    std::optional<Attempt> pending;
};

MountRenewOperationEnvironment renewalEnvironment(
    const std::function<bool()> & live = {},
    const std::function<bool()> & cancelled = {})
{
    return MountRenewOperationEnvironment{
        .live = live,
        .cancelled = cancelled,
        .on_request = {},
    };
}

DB::Exception terminalException(const MountRenewResult & result)
{
    EXPECT_EQ(result.outcome, MountRenewOutcome::Terminal);
    EXPECT_NE(result.failure, nullptr);
    try
    {
        std::rethrow_exception(result.failure);
    }
    catch (const DB::Exception & e)
    {
        return e;
    }
    catch (...)
    {
        ADD_FAILURE() << "terminal renewer failure was not a typed DB::Exception";
    }
    return DB::Exception(DB::ErrorCodes::ABORTED, "missing terminal exception");
}

void renewOrThrow(MountLeaseRenewer & renewer)
{
    const MountRenewResult result = renewer.renew(MountRenewOperationEnvironment{});
    if (result.outcome == MountRenewOutcome::Terminal)
        std::rethrow_exception(result.failure);
    if (result.outcome != MountRenewOutcome::Committed)
        throw DB::Exception(DB::ErrorCodes::ABORTED, "renewer renewal was not attempted");
}
}

TEST(CASHeartbeat, AnchorCarriesFloor)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t min_active_build_sequence_now = 5;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [&] { return min_active_build_sequence_now; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    ASSERT_TRUE(ops.op.head(layout.mountKey(srid), Retry::standard()).has_value());
    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_EQ(m.writer_epoch, 9u);
    EXPECT_EQ(m.min_active_build_sequence, 5u);
    EXPECT_EQ(m.seq, 1u);
    EXPECT_FALSE(m.gc_fenced);
}

TEST(CASHeartbeat, RenewRereadsCallbackAndBumpsSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t min_active_build_sequence_now = 5;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [&] { return min_active_build_sequence_now; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    /// The dynamic field moves; the renewal re-reads it off the callback and bumps seq.
    now_ms = 1500;
    min_active_build_sequence_now = 8;
    renewOrThrow(renewer);

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_EQ(m.min_active_build_sequence, 8u);
    EXPECT_EQ(m.seq, 2u);
    EXPECT_EQ(m.expires_at_ms, 1500u + 100u);
}

TEST(CASHeartbeat, StopStampsExpiredAndFarewellSentinel)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    now_ms = 2000;
    renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    /// Terminal body stamps the lease already-expired (so a same-server reopen reclaims immediately)
    /// AND folds the watermark farewell into it (min_active_build_sequence = UINT64_MAX).
    EXPECT_LE(m.expires_at_ms, now_ms);
    EXPECT_EQ(m.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
}

namespace
{
/// Reports the SHIPPED PRODUCTION defaults (`attempt_timeout_ms=5000`, two `connect_timeout_cap_ms=1000`
/// caps -> `attemptEnvelopeMs()=7000`, `CasRequestBudget.cpp`'s own defaults) while landing every attempt
/// immediately: the write's own success is not what is under test here, only whether the farewell's
/// policy window is wide enough to admit one attempt in the first place.
struct DefaultEnvelopeBackend : InMemoryBackend
{
    uint64_t attemptTimeoutMs() const override { return 5000; }
    uint64_t attemptEnvelopeMs() const override { return 7000; }
};

/// A DIFFERENT envelope from `DefaultEnvelopeBackend`'s, for
/// `FarewellIsAdmittedUnderADifferentEnvelope` below: that test exists to pin the window's
/// ARITHMETIC, not just that some window admits the write, so it needs a reservation the
/// shipped-default window (16000 ms) could not have admitted by coincidence.
struct WiderEnvelopeBackend : InMemoryBackend
{
    uint64_t attemptTimeoutMs() const override { return 5000; }
    uint64_t attemptEnvelopeMs() const override { return 9000; }
};
}

/// A write reserves two attempt envelopes before it starts (`CasOperation::writeLoop`'s
/// `reservedFor(0, 2)`), so at the shipped defaults the farewell needs a policy window that admits
/// 2 * 7000 = 14000 ms. A fixed window that predates that reservation (`kFarewellBudgetMs` alone is
/// 10000 ms) refuses the write before its first attempt on every graceful shutdown: no farewell is
/// published, and the next start pays a full incarnation-stability observation instead of reclaiming
/// the slot instantly.
TEST(CASHeartbeat, FarewellIsAdmittedUnderTheDefaultBudget)
{
    auto backend = std::make_shared<DefaultEnvelopeBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(30000), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                            [&] { return boot_ms; });
    renewer.start();

    now_ms = 2000;
    EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 30000))
        << "the farewell's policy window must admit the write's own two-envelope reservation "
           "(2 * 7000 ms with the shipped defaults) -- otherwise a clean shutdown never hands the "
           "mount slot back and every restart pays a full incarnation-stability observation";

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_LE(m.expires_at_ms, now_ms);
    EXPECT_EQ(m.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
}

/// Pins the window's ARITHMETIC, not just that some fixed window happens to be wide enough: a
/// regression that hardcoded the shipped-default window (16000 ms) instead of deriving it from
/// `attemptReservationMs()` would still pass `FarewellIsAdmittedUnderTheDefaultBudget` above (16000
/// happens to equal what a 7000 ms envelope needs) but would refuse THIS write, whose reservation is
/// 2 * 9000 = 18000 ms -- strictly more than the shipped-default window.
TEST(CASHeartbeat, FarewellIsAdmittedUnderADifferentEnvelope)
{
    auto backend = std::make_shared<WiderEnvelopeBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/40000);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(40000), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                            [&] { return boot_ms; });
    renewer.start();

    now_ms = 2000;
    EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 40000))
        << "the farewell's policy window must be DERIVED from this backend's own envelope "
           "(2 * 9000 ms), not hardcoded to the shipped-default window -- a window fixed at "
           "16000 ms would refuse this write's 18000 ms reservation";

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_LE(m.expires_at_ms, now_ms);
    EXPECT_EQ(m.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
}

/// The derived window alone is not the whole story: mount-control activity must also never run past
/// the point this node's own fence may already be gone. A 5000 ms TTL with a 2000 ms safety margin
/// leaves only 3000 ms of lease-safe remaining time at release -- far short of the 7000 ms envelope's
/// own 16000 ms derived window (2 * 7000 + 2000 slack) -- so the LEASE bound, not the derived window,
/// must be what refuses this write, and it must refuse it before any physical attempt: a write that
/// cannot land inside the lease-safe remainder gains nothing by being sent anyway.
TEST(CASHeartbeat, FarewellIsRefusedWhenTheLeaseExpiresBeforeItsDerivedWindow)
{
    auto backend = std::make_shared<DefaultEnvelopeBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/5000);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(5000), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                            [&] { return boot_ms; });
    renewer.start();

    now_ms = 2000;
    String message;
    int code = 0;
    bool threw = false;
    try
    {
        renewer.release(renewer.lastCommittedAttemptStartBootMs() + 5000);
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        message = e.message();
        code = e.code();
    }
    EXPECT_TRUE(threw) << "a farewell whose reservation cannot fit inside the lease-safe remaining "
                           "time must be refused, not admitted past the point this node's fence may "
                           "already be gone";
    EXPECT_EQ(code, DB::ErrorCodes::NETWORK_ERROR) << message;
    EXPECT_NE(message.find("gave up at the lease deadline after zero attempt(s)"), String::npos) << message;

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_NE(m.min_active_build_sequence, std::numeric_limits<uint64_t>::max())
        << "the refused write must not have landed";
}

/// The farewell is bounded by the deadline its caller passes. A near deadline refuses it although the
/// renewer's own lease would admit it, and a far one admits it although that lease would refuse it.
TEST(CASHeartbeat, FarewellIsBoundByTheDeadlineItIsGiven)
{
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);

    {
        auto backend = std::make_shared<DefaultEnvelopeBackend>();
        uint64_t now_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);
        MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                                  std::chrono::milliseconds(30000), [&] { return now_ms; },
                                  [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                                  [&] { return boot_ms; });
        renewer.start();

        now_ms = 2000;
        String message;
        int code = 0;
        try
        {
            /// A TTL of 30000 admits this farewell (`FarewellIsAdmittedUnderTheDefaultBudget`); the
            /// deadline passed here leaves too little past the margin for the write's reservation.
            renewer.release(boot_ms + 5000);
            ADD_FAILURE() << "a farewell must be refused when the deadline it is given cannot admit it";
        }
        catch (const DB::Exception & e)
        {
            message = e.message();
            code = e.code();
        }
        EXPECT_EQ(code, DB::ErrorCodes::NETWORK_ERROR) << message;
        EXPECT_NE(message.find("gave up at the lease deadline after zero attempt(s)"), String::npos) << message;
        EXPECT_NE(decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes).min_active_build_sequence,
                  std::numeric_limits<uint64_t>::max())
            << "the refused farewell must not have landed";
    }

    {
        auto backend = std::make_shared<DefaultEnvelopeBackend>();
        uint64_t now_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/5000);
        MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                                  std::chrono::milliseconds(5000), [&] { return now_ms; },
                                  [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                                  [&] { return boot_ms; });
        renewer.start();

        now_ms = 2000;
        /// A TTL of 5000 refuses this farewell (`FarewellIsRefusedWhenTheLeaseExpiresBeforeItsDerivedWindow`).
        EXPECT_NO_THROW(renewer.release(boot_ms + 30000));
        const MountLease farewell = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
        EXPECT_LE(farewell.expires_at_ms, now_ms);
        EXPECT_EQ(farewell.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
    }
}

/// The lease bound added above must not change what an ordinary Conflict outcome does: a successor
/// that took the slot (a different, unfenced incarnation) before this node's own shutdown could
/// publish its farewell must be left untouched, and the release must report the conflict rather than
/// silently succeeding or overwriting the successor's incarnation.
TEST(CASHeartbeat, ForeignIncarnationDuringFarewellLeavesTheSuccessorUntouchedAndReportsTheConflict)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    /// A successor (a different uuid/epoch, NOT gc_fenced) took the slot before this node's own
    /// clean shutdown could publish its farewell -- the exact shape a live double-start reclaim
    /// leaves behind.
    const auto observed = ops.op.read(layout.mountKey(srid), Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease successor;
    successor.server_uuid = UInt128(0x9999);
    successor.writer_epoch = 1;
    successor.seq = 1;
    successor.write_attempt_id = UInt128{1};
    mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(successor), observed->etag,
                              Retry::standard()), "successor slot");

    now_ms = 2000;
    String message;
    int code = 0;
    try
    {
        renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);
        FAIL() << "a farewell that finds a foreign, unfenced incarnation must report the conflict, "
                  "not silently succeed or clobber the successor";
    }
    catch (const DB::Exception & e)
    {
        message = e.message();
        code = e.code();
    }
    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("found a foreign incarnation"), String::npos) << message;

    auto m = decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes);
    EXPECT_EQ(m.server_uuid, successor.server_uuid)
        << "the successor's own incarnation must be untouched by the refused farewell";
    EXPECT_EQ(m.writer_epoch, successor.writer_epoch);
}

/// Phase A (spec rev.4 2026-07-24): a confirmed renewal mismatch whose re-read shows OUR OWN
/// (uuid, epoch), unfenced, is state UNCERTAINTY (an ambiguous landed renewal of ours, or a
/// same-pair twin after epoch-state loss) — fail closed via fence + self-remount, never an
/// exception that aborts debug/ASan builds at construction.
TEST(CASHeartbeat, SameEpochUnfencedTouchIsUncertainNotFatal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    /// The slot advances past the incarnation we hold, under our own pair (the ambiguous-landed-renewal shape).
    const auto observed = ops.op.read(layout.mountKey(srid), Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease advanced;
    advanced.server_uuid = uuid;
    advanced.writer_epoch = 9;
    advanced.seq = 99;
    advanced.write_attempt_id = UInt128{99};
    mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(advanced), observed->etag,
                              Retry::standard()), "advanced slot");

    try
    {
        renewOrThrow(renewer);
        FAIL() << "renew must return a terminal conflict";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED) << e.message();
        EXPECT_NE(e.message().find("state uncertain"), String::npos) << e.message();
        /// Forensics must ride in the message: the observed seq and our local seq.
        EXPECT_NE(e.message().find("seq=99"), String::npos) << e.message();
        /// The local-seq fragment specifically -- not just any "seq=99" substring (which the
        /// OBSERVED holder's own describeMountHolder text could also satisfy on its own).
        EXPECT_NE(e.message().find("vs our seq="), String::npos) << e.message();
    }
}

/// A body under our own uuid but a NEWER writer_epoch is proven supersession — a normal fencing
/// outcome (the TLA model's localLost), fail closed but never an abort.
TEST(CASHeartbeat, SupersededTouchIsFailClosedNotFatal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    const auto observed = ops.op.read(layout.mountKey(srid), Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease successor;
    successor.server_uuid = uuid;
    successor.writer_epoch = 10;
    successor.seq = 1;
    successor.write_attempt_id = UInt128{1};
    mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(successor), observed->etag,
                              Retry::standard()), "successor slot");

    try
    {
        renewOrThrow(renewer);
        FAIL() << "renew must return a terminal conflict";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED) << e.message();
        EXPECT_NE(e.message().find("superseded by a newer incarnation"), String::npos) << e.message();
    }
}

/// A foreign server holding our mount slot must FAIL CLOSED — and must not take the process with it.
///
/// This test used to be `ForeignUuidTouchStillDies`, an `EXPECT_DEATH` that pinned the abort. The abort
/// was the defect: the arm raised `LOGICAL_ERROR`, which aborts at CONSTRUCTION in debug/ASan builds,
/// and the runtime consumes it on its renewal worker — so an environment-reachable condition (clear the
/// prefix, recreate under a different server id, and the survivor's next renewal lands there; see
/// `CASRefContiguousAlloc.SurvivingWriterIsFencedByTheRecreatedPoolsMount`, which drives exactly that)
/// took the whole server down, and took the ASan gate down with it.
///
/// What must NOT change is the outcome, which is what this test now pins: synchronous renewal returns
/// a terminal failure that, when propagated, throws; the exception
/// carries the foreign holder's identity, and it is classified `ABORTED` — the same mount-lost class the
/// sibling fencing arms use, which the runtime terminal consumer turns into a latched write fence. The
/// `abort_on_logical_error` arming is deliberately kept: with it ON, a `LOGICAL_ERROR` would still abort,
/// so reaching the `EXPECT_THROW` at all is the proof that this condition is no longer classified as one.
TEST(CASHeartbeat, ForeignUuidTouchFailsClosedWithoutAborting)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    const auto observed = ops.op.read(layout.mountKey(srid), Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease foreign;
    foreign.server_uuid = UInt128(0x9999);
    foreign.writer_epoch = 1;
    foreign.seq = 1;
    foreign.write_attempt_id = UInt128{1};
    mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(foreign), observed->etag,
                              Retry::standard()), "foreign slot");

    /// Restored on every exit: this flag is process-global and every later test in this binary would
    /// inherit it.
    const bool armed_before = DB::abort_on_logical_error.load(std::memory_order_relaxed);
    DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
    SCOPE_EXIT({ DB::abort_on_logical_error.store(armed_before, std::memory_order_relaxed); });

    String message;
    int code = 0;
    try
    {
        renewOrThrow(renewer);
        FAIL() << "a foreign holder must fail the renewal closed, not be silently taken over";
    }
    catch (const DB::Exception & e)
    {
        message = e.message();
        code = e.code();
    }
    EXPECT_NE(message.find("held by a foreign server"), String::npos) << message;
    EXPECT_EQ(code, DB::ErrorCodes::ABORTED)
        << "the mount-lost class the runtime terminal consumer latches the write fence on -- and, critically, not "
           "LOGICAL_ERROR, which would abort the renewal worker and the whole process with it";
}

/// Mount-slot writer audit (the P1 "foreign writer" instrument): every mount-slot WRITE and every
/// OBSERVED foreign/conflicting body becomes an event, carrying the conflicting body's identity —
/// the payload the chronic "touched by a foreign writer" collisions need to be diagnosable.
TEST(CASMountAudit, ClaimReleaseAndForeignConflictEmitEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };

    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    const uint64_t now_ms = 1'000'000;
    /// mint for uuid 1 -> one mount_claim
    ASSERT_EQ(claimMount(ops.op, layout, "a", UInt128{1}, 1, now_ms, /*ttl_ms=*/10'000, {}, sink).kind,
              MountClaimResult::Claimed);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountClaim);
    EXPECT_EQ(seen[0].detail.at("server_root_id"), "a");
    EXPECT_EQ(seen[0].detail.at("branch"), "mint");

    /// a FOREIGN uuid claiming a live slot -> mount_conflict carrying the current holder's identity
    seen.clear();
    (void)claimMount(ops.op, layout, "a", UInt128{2}, 1, now_ms, /*ttl_ms=*/10'000, {}, sink);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("server_root_id"), "a");
    /// The conflict must carry the ORIGINAL holder's identity (uuid 1, the minter) — not the
    /// foreign claimer's (uuid 2).
    EXPECT_EQ(seen.back().detail.at("holder_uuid"), u128ToHex(UInt128{1}));
    EXPECT_NE(seen.back().detail.at("holder_uuid"), u128ToHex(UInt128{2}));
}

/// The MountLeaseRenewer wiring: `start` adopting an already-claimed slot emits mount_claim, `stop`
/// (the farewell write) emits mount_release.
TEST(CASMountAudit, RenewerAdoptEmitsClaimAndTerminateEmitsRelease)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, sink, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountClaim);
    EXPECT_EQ(seen[0].detail.at("branch"), "adopt");

    seen.clear();
    now_ms = 2000;
    renewer.release(renewer.lastCommittedAttemptStartBootMs() + 100);

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountRelease);
    EXPECT_EQ(seen[0].detail.at("branch"), "farewell");
}

/// Renewer-level foreign-conflict refusal: the mount slot is already held by a FOREIGN uuid (X) when
/// a renewer for a DIFFERENT uuid (Y) tries to claim it. This must fail closed and — since the
/// mount-audit sink is not yet installed at first-open — name X in the exception's message text
/// (the only identity carrier in err.log at that point). MountConflict payload coverage is above.
TEST(CASMountAudit, RenewerForeignConflictRefusesAndNamesHolder)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid_x(0x1111);
    const UInt128 uuid_y(0x2222);
    uint64_t now_ms = 1000;

    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    /// Foreign holder X claims the slot first.
    ASSERT_EQ(claimMount(ops.op, layout, srid, uuid_x, /*our_epoch=*/1, now_ms, /*ttl_ms=*/100).kind,
              MountClaimResult::Claimed);

    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid_y, /*writer_epoch=*/1,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, {}, std::chrono::milliseconds(2000),
                            [&] { return boot_ms; });

    /// The enriched refusal message must name the OBSERVED holder (X), not the caller (Y).
    const String holder_uuid = u128ToHex(uuid_x);
    DB::Cas::tests::expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        holder_uuid,
        [&] { renewer.start(); });
}

/// `Pool::open` can fail before/inside `doStart` (e.g. a foreign-conflict refusal, see
/// `RenewerForeignConflictRefusesAndNamesHolder` above) — the renewer is destroyed without ever having
/// claimed anything. Teardown must not throw "release before start"; there is nothing to release. A
/// stop AFTER a successful start still performs the farewell (covered by
/// `StopStampsExpiredAndFarewellSentinel` above); a genuinely-started DOUBLE terminate stays loud.
TEST(CASMountAudit, RenewerAdoptRefusesFencedSelfWithTypedError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;

    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    /// mint (uuid, epoch 9), then fence it in place (what computeHeartbeatFloor does on expiry):
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);
    {
        auto got = ops.op.read(layout.mountKey(srid), Retry::standard());
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(fenced), got->etag,
                                  Retry::standard()), "fence-out");
    }

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    /// A renewer for the SAME (uuid, epoch) tries to adopt the now-fenced slot.
    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, sink, std::chrono::milliseconds(2000),
                            [&] { return boot_ms; });

    bool threw = false;
    try
    {
        renewer.start();
    }
    catch (const MountFencedException & e)
    {
        threw = true;
        EXPECT_NE(e.message().find("fenced by GC"), String::npos) << e.message();
        EXPECT_EQ(e.message().find("foreign writer"), String::npos) << e.message();
    }
    EXPECT_TRUE(threw);

    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("branch"), "fenced_by_gc");
}

/// A renew mismatch is classified by BODY, not blamed on "a foreign writer" by default: the GC can
/// fence our OWN (uuid, epoch) mount slot after our lease expires (a late renewal beat racing the
/// GC's fence-out). The renewer must re-read and recognize this as its OWN incarnation being fenced —
/// a recoverable `MountFencedException`, not the generic single-writer-violation text.
TEST(CASHeartbeat, RenewOverFencedOwnSlotIsClassifiedNotForeign)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    MountLeaseRenewer renewer(ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9,
                            std::chrono::milliseconds(100), [&] { return now_ms; },
                            [] { return uint64_t{5}; }, sink, std::chrono::milliseconds(0),
                            [&] { return boot_ms; });
    renewer.start();
    seen.clear();

    /// Mid-run: the GC fences our own (uuid, epoch) mount slot in place (as `computeHeartbeatFloor`
    /// does on an expired lease), preserving the whole body — a guarded write against the incarnation
    /// it observed, exactly as the GC's own fence-out does it.
    {
        const auto got = ops.op.read(layout.mountKey(srid), Retry::standard());
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        mustCommit(ops.op.replace(layout.mountKey(srid), encodeMountLease(fenced), got->etag,
                                  Retry::standard()), "fence-out");
    }

    /// The renewal must classify the fence honestly — not "foreign writer":
    try
    {
        renewOrThrow(renewer);
        FAIL() << "renew over a fenced slot must be terminal";
    }
    catch (const MountFencedException & e)
    {
        EXPECT_TRUE(e.message().find("fenced by GC") != String::npos);
        EXPECT_TRUE(e.message().find("foreign writer") == String::npos);
    }
    /// and the capture sink saw mount_conflict branch=fenced_by_gc with the fenced body's identity.
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("branch"), "fenced_by_gc");
    EXPECT_EQ(seen.back().detail.at("holder_uuid"), u128ToHex(uuid));
}

TEST(CASHeartbeat, RenewerStateAllowsOnlyActiveReleaseOrTerminal)
{
#if defined(DEBUG_OR_SANITIZER_BUILD)
#define EXPECT_RENEWER_STATE_REJECTION(statement) EXPECT_DEATH({ statement; }, "allowed only in")
#else
#define EXPECT_RENEWER_STATE_REJECTION(statement) EXPECT_THROW(statement, DB::Exception)
#endif

    Layout layout("pool");
    const UInt128 uuid{0x1234};

    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "released", uuid, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "released", uuid, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{7}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::New);
        EXPECT_RENEWER_STATE_REJECTION(renewer.renew(renewalEnvironment()));
        EXPECT_RENEWER_STATE_REJECTION(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000));
        EXPECT_EQ(renewer.start(), 100u);
        EXPECT_RENEWER_STATE_REJECTION(renewer.start());
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::Active);
        renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::Released);
        EXPECT_RENEWER_STATE_REJECTION(renewer.start());
        EXPECT_RENEWER_STATE_REJECTION(renewer.renew(renewalEnvironment()));
        EXPECT_RENEWER_STATE_REJECTION(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000));
    }

    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "terminal", uuid, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "terminal", uuid, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{7}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        backend->read_calls = 0;
        /// Live until the ambiguous attempt's resolving read has run, so that attempt is the only one
        /// sent and the renewal ends terminal with it unsettled.
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(/*live=*/[&] { return backend->read_calls == 0; }));
        EXPECT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_NE(result.failure, nullptr);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_RENEWER_STATE_REJECTION(renewer.start());
        EXPECT_RENEWER_STATE_REJECTION(renewer.renew(renewalEnvironment()));
        EXPECT_RENEWER_STATE_REJECTION(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000));
    }

#undef EXPECT_RENEWER_STATE_REJECTION
}

TEST(CASHeartbeat, RenewalRetriesOneImmutableBodyAndAdoptsLostResponse)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid{0x1234};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, srid, uuid, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{7}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();

    backend->attempts.clear();
    backend->actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::Delegate};
    MountRenewResult retried = renewer.renew(renewalEnvironment());
    ASSERT_EQ(retried.outcome, MountRenewOutcome::Committed);
    ASSERT_EQ(backend->attempts.size(), 2u);
    EXPECT_EQ(backend->attempts[0].key, backend->attempts[1].key);
    EXPECT_EQ(backend->attempts[0].bytes, backend->attempts[1].bytes);
    EXPECT_EQ(backend->attempts[0].expected, backend->attempts[1].expected);
    const MountLease retry_body = decodeMountLease(backend->attempts[0].bytes);
    EXPECT_NE(retry_body.write_attempt_id, UInt128{});

    backend->attempts.clear();
    backend->actions = {RenewalScriptBackend::Action::LandThenThrow};
    MountRenewResult adopted = renewer.renew(renewalEnvironment());
    EXPECT_EQ(adopted.outcome, MountRenewOutcome::Committed);
    EXPECT_TRUE(adopted.resolved_by_read);
    EXPECT_EQ(adopted.attempts_sent, 1u);
    EXPECT_EQ(decodeMountLease(ops.op.read(layout.mountKey(srid), Retry::standard())->bytes).write_attempt_id,
              decodeMountLease(backend->attempts.front().bytes).write_attempt_id);
}

#if USE_AWS_S3
TEST(CASHeartbeat, RenewalOverConnectFailuresRecoversWithoutASettleRead)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid{0x1234};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, srid, uuid, 9, wall_ms, 30000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, srid, uuid, 9, std::chrono::milliseconds(30000),
        [&] { return wall_ms; }, [] { return uint64_t{7}; }, {}, std::chrono::milliseconds(2000),
        [&] { return boot_ms; });
    renewer.start();

    backend->attempts.clear();
    backend->read_calls = 0;
    /// Sixty connect failures, then the store answers.
    for (int i = 0; i < 60; ++i)
        backend->actions.push_back(RenewalScriptBackend::Action::ThrowConnectHint);
    backend->actions.push_back(RenewalScriptBackend::Action::Delegate);
    const MountRenewResult renewed = renewer.renew(renewalEnvironment());
    ASSERT_EQ(renewed.outcome, MountRenewOutcome::Committed);
    EXPECT_GT(renewed.attempts_sent, 1u);
    EXPECT_FALSE(renewed.resolved_by_read);            /// classification `committed_after_retry`
    EXPECT_EQ(backend->read_calls, 0u);
    EXPECT_EQ(backend->attempts.size(), 61u);
    for (const auto & attempt : backend->attempts)
        EXPECT_EQ(attempt.bytes, backend->attempts.front().bytes);
}
#endif

TEST(CASHeartbeat, CancellationBeforeSendIsNotAttemptedAndAllowsRelease)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();
    backend->attempts.clear();
    backend->read_calls = 0;
    const MountRenewResult result = renewer.renew(renewalEnvironment(/*live=*/[] { return false; }, /*cancelled=*/[] { return true; }));
    EXPECT_EQ(result.outcome, MountRenewOutcome::NotAttempted);
    EXPECT_EQ(result.failure, nullptr);
    EXPECT_EQ(renewer.state(), MountLeaseRenewerState::Active);
    EXPECT_TRUE(backend->attempts.empty());
    EXPECT_NO_THROW(renewer.release(renewer.lastCommittedAttemptStartBootMs() + 1000));
    EXPECT_EQ(renewer.state(), MountLeaseRenewerState::Released);
}

TEST(CASHeartbeat, CancellationAfterSendIsTerminalAndForbidsRelease)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    bool cancelled = false;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();
    backend->attempts.clear();
    backend->read_calls = 0;
    backend->cancel_after_write = [&] { cancelled = true; };
    backend->actions = {RenewalScriptBackend::Action::ReturnThenCancel};
    const MountRenewResult result = renewer.renew(
        renewalEnvironment(/*live=*/[&] { return !cancelled; }, /*cancelled=*/[&] { return cancelled; }));
    const DB::Exception failure = terminalException(result);
    EXPECT_EQ(failure.code(), DB::ErrorCodes::NETWORK_ERROR);
    EXPECT_GE(result.attempts_sent, 1u);
    EXPECT_EQ(backend->read_calls, 0u) << "post-write cancellation must not start a diagnostic read";
    EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
    const String bytes_before = ops.op.read(layout.mountKey("test"), Retry::standard())->bytes;
    EXPECT_FALSE(renewer.canRelease());
    EXPECT_EQ(ops.op.read(layout.mountKey("test"), Retry::standard())->bytes, bytes_before);
}

TEST(CASHeartbeat, SlowResolvedSuccessKeepsAttemptStartAnchor)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();
    boot_ms = 150;
    backend->cancel_after_write = [&] { boot_ms = 400; };
    backend->actions = {RenewalScriptBackend::Action::LandThenThrow};
    const MountRenewResult result = renewer.renew(renewalEnvironment());
    EXPECT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(result.attempt_start_boot_ms, 150u);
    EXPECT_EQ(renewer.lastCommittedAttemptStartBootMs(), 150u);
}

TEST(CASHeartbeat, SamePairTwinAndForeignOrSuccessorStayTerminal)
{
    const auto run_case = [](UInt128 current_uuid, uint64_t current_epoch, UInt128 current_attempt)
    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        Layout layout("pool");
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        const UInt128 uuid{1};
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "test", uuid, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "test", uuid, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        auto got = ops.op.read(layout.mountKey("test"), Retry::standard());
        MountLease current = decodeMountLease(got->bytes);
        current.server_uuid = current_uuid;
        current.writer_epoch = current_epoch;
        current.write_attempt_id = current_attempt;
        ++current.seq;
        mustCommit(ops.op.replace(layout.mountKey("test"), encodeMountLease(current), got->etag,
                                  Retry::standard()), "competing slot");
        backend->read_calls = 0;
        const MountRenewResult result = renewer.renew(renewalEnvironment());
        const DB::Exception failure = terminalException(result);
        EXPECT_NE(failure.code(), DB::ErrorCodes::LOGICAL_ERROR);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_EQ(backend->read_calls, 1u) << "the write's own resolving read must be the only terminal read";
    };

    run_case(UInt128{1}, 9, UInt128{0xAAAA});
    run_case(UInt128{2}, 9, UInt128{0xBBBB});
    run_case(UInt128{1}, 10, UInt128{0xCCCC});
}

TEST(CASHeartbeat, ExpectedPredecessorThenLateLandingIsAdoptedExactly)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();
    backend->attempts.clear();
    backend->actions = {
        RenewalScriptBackend::Action::ThrowBeforeThenLandAfterResolve,
        RenewalScriptBackend::Action::Delegate,
    };
    const MountRenewResult result = renewer.renew(renewalEnvironment());
    EXPECT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_TRUE(result.resolved_by_read);
    ASSERT_EQ(backend->attempts.size(), 2u);
    EXPECT_EQ(backend->attempts[0].bytes, backend->attempts[1].bytes);
    EXPECT_EQ(decodeMountLease(ops.op.read(layout.mountKey("test"), Retry::standard())->bytes).write_attempt_id,
              decodeMountLease(backend->attempts[0].bytes).write_attempt_id);
}

namespace
{
/// One renewer over a scripted store, started on its own seeded claim, for the cases of
/// `RenewReturnsWhatItsReportNeeds`. The clocks come first: hooks stored in `backend` capture them.
struct ReportFieldsCase
{
    explicit ReportFieldsCase(String srid_)
        : srid(std::move(srid_))
    {
        seedOwnClaim(ops.op, layout, srid, uuid, /*epoch=*/9, wall_ms, /*ttl_ms=*/1000);
        renewer.start();
        backend->attempts.clear();
    }

    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    String srid;
    UInt128 uuid{1};
    Layout layout{"pool"};
    std::shared_ptr<RenewalScriptBackend> backend = std::make_shared<RenewalScriptBackend>();
    Ops ops{backend, &boot_ms};
    MountLeaseRenewer renewer{
        ops.farewell, ops.lease, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(1000),
        [this] { return wall_ms; }, [] { return uint64_t{0}; }, CasEventSink{}, std::chrono::milliseconds(20),
        [this] { return boot_ms; }};
};
}

TEST(CASHeartbeat, GcFenceAndVanishedMountStayTerminal)
{
    const auto run_case = [](bool vanish)
    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        Layout layout("pool");
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        const String key = layout.mountKey("test");
        auto got = ops.op.read(key, Retry::standard());
        if (vanish)
            ASSERT_EQ(ops.op.remove(key, got->etag, Retry::standard()), Removal::Removed);
        else
        {
            MountLease fenced = decodeMountLease(got->bytes);
            fenced.gc_fenced = true;
            ++fenced.seq;
            mustCommit(ops.op.replace(key, encodeMountLease(fenced), got->etag, Retry::standard()),
                       "fence-out");
        }
        const DB::Exception failure = terminalException(renewer.renew(renewalEnvironment()));
        EXPECT_NE(failure.code(), DB::ErrorCodes::LOGICAL_ERROR);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
    };
    run_case(false);
    run_case(true);
}

/// A renewal returns what its report names: the body it wrote or tried to write, why it ended, and
/// how long it took on its own boot clock.
TEST(CASHeartbeat, RenewReturnsWhatItsReportNeeds)
{
    {
        ReportFieldsCase c("commit");
        c.backend->on_attempt = [&boot_ms = c.boot_ms] { boot_ms += 250; };
        const MountRenewResult result = c.renewer.renew(renewalEnvironment());
        ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
        ASSERT_EQ(c.backend->attempts.size(), 1u);
        const MountLease sent = decodeMountLease(c.backend->attempts.back().bytes);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_EQ(sent.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{});
        EXPECT_EQ(result.write_attempt_id, sent.write_attempt_id);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Unclassified);
        EXPECT_EQ(result.attempt_start_boot_ms, 100u);
        EXPECT_EQ(result.elapsed_ms, 250u);
    }

    {
        ReportFieldsCase c("conflict");
        const String key = c.layout.mountKey(c.srid);
        const auto got = c.ops.op.read(key, Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease foreign = decodeMountLease(got->bytes);
        foreign.server_uuid = UInt128{2};
        foreign.seq = 40;
        foreign.write_attempt_id = UInt128{0xF0F0};
        mustCommit(c.ops.op.replace(key, encodeMountLease(foreign), got->etag, Retry::standard()), "foreign slot");
        c.backend->attempts.clear();
        c.backend->on_attempt = [&boot_ms = c.boot_ms] { boot_ms += 250; };
        const MountRenewResult result = c.renewer.renew(renewalEnvironment());
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        ASSERT_EQ(c.backend->attempts.size(), 1u);
        const MountLease sent = decodeMountLease(c.backend->attempts.back().bytes);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Conflict);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u) << "the seq this renewal tried to write, not the occupant's";
        EXPECT_EQ(result.write_attempt_id, sent.write_attempt_id);
        EXPECT_EQ(result.elapsed_ms, 250u);
    }

    {
        ReportFieldsCase c("vanished");
        const String key = c.layout.mountKey(c.srid);
        const auto got = c.ops.op.read(key, Retry::standard());
        ASSERT_TRUE(got.has_value());
        ASSERT_EQ(c.ops.op.remove(key, got->etag, Retry::standard()), Removal::Removed);
        const MountRenewResult result = c.renewer.renew(renewalEnvironment());
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Vanished);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{});
    }

    {
        /// Ended before its first request by a liveness that refuses with no stop requested.
        ReportFieldsCase c("refused");
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(/*live=*/[] { return false; }, /*cancelled=*/[] { return false; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        EXPECT_TRUE(c.backend->attempts.empty());
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::FenceOrLifecycleLost);
        EXPECT_EQ(result.writer_epoch, 9u);
        EXPECT_EQ(result.seq, 2u);
        EXPECT_NE(result.write_attempt_id, UInt128{}) << "the id is minted before the renewal is admitted";
        EXPECT_EQ(result.elapsed_ms, 0u);
    }

    {
        /// Ended before its first request by a stop.
        ReportFieldsCase c("stopped");
        const MountRenewResult result = c.renewer.renew(renewalEnvironment(/*live=*/[] { return false; }, /*cancelled=*/[] { return true; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::NotAttempted);
        EXPECT_EQ(result.classification, MountRenewTerminalClassification::Cancelled);
        EXPECT_EQ(result.seq, 2u);
    }
}

TEST(CASHeartbeat, LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor)
{
    Layout layout("pool");
    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "before-reclaim", UInt128{1}, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "before-reclaim", UInt128{1}, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, CasEventSink{}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();
        backend->actions = {RenewalScriptBackend::Action::ThrowBeforeThenLandAfterResolve};
        backend->read_calls = 0;
        /// Live until the resolving read has run: the delayed write lands during that read, and the
        /// renewal ends terminal without a second attempt.
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(/*live=*/[&] { return backend->read_calls == 0; }));
        EXPECT_EQ(result.outcome, MountRenewOutcome::Terminal);

        /// The delayed write landed during the resolving read. It carries this renewer's own epoch, and
        /// it does not put the renewer back in business.
        const MountLease landed = decodeMountLease(
            ops.op.read(layout.mountKey("before-reclaim"), Retry::standard())->bytes);
        EXPECT_EQ(landed.writer_epoch, 9u);
        EXPECT_EQ(renewer.state(), MountLeaseRenewerState::RenewalTerminal);
    }
    {
        auto backend = std::make_shared<RenewalScriptBackend>();
        uint64_t wall_ms = 1000;
        uint64_t boot_ms = 100;
        Ops ops(backend, &boot_ms);
        seedOwnClaim(ops.op, layout, "after-successor", UInt128{1}, 9, wall_ms, 1000);
        MountLeaseRenewer renewer(
            ops.farewell, ops.lease, layout, "after-successor", UInt128{1}, 9, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        renewer.start();

        /// The incarnation the about-to-be-terminal renewal names as its precondition: a late delivery
        /// of that attempt can only ever be replayed against exactly this one.
        const Etag delayed_precondition
            = ops.op.read(layout.mountKey("after-successor"), Retry::standard())->etag;

        backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        backend->read_calls = 0;
        const MountRenewResult result = renewer.renew(
            renewalEnvironment(/*live=*/[&] { return backend->read_calls == 0; }));
        ASSERT_EQ(result.outcome, MountRenewOutcome::Terminal);
        ASSERT_FALSE(backend->attempts.empty());
        const auto delayed = backend->attempts.back();

        /// The GC fences the slot, then a successor claims it at a fresh epoch and adopts it.
        auto current = ops.op.read(delayed.key, Retry::standard());
        MountLease fenced = decodeMountLease(current->bytes);
        fenced.gc_fenced = true;
        ++fenced.seq;
        mustCommit(ops.op.replace(delayed.key, encodeMountLease(fenced), current->etag, Retry::standard()),
                   "fence-out");
        ASSERT_EQ(claimMount(ops.op, layout, "after-successor", UInt128{1}, 10, wall_ms, 1000).kind,
                  MountClaimResult::Claimed);
        MountLeaseRenewer successor(
            ops.farewell, ops.lease, layout, "after-successor", UInt128{1}, 10, std::chrono::milliseconds(1000),
            [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
            [&] { return boot_ms; });
        successor.start();

        /// Replaying the delayed attempt against the incarnation it named is refused; the successor's
        /// body is what stands.
        EXPECT_TRUE(std::holds_alternative<Conflict>(
            ops.op.replace(delayed.key, delayed.bytes, delayed_precondition, Retry::once())));
        EXPECT_EQ(decodeMountLease(ops.op.read(delayed.key, Retry::standard())->bytes).writer_epoch, 10u);
    }
}

TEST(CASHeartbeat, WallClockStepsAndBootSuspendCannotExtendAuthority)
{
    auto backend = std::make_shared<RenewalScriptBackend>();
    Layout layout("pool");
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    Ops ops(backend, &boot_ms);
    seedOwnClaim(ops.op, layout, "test", UInt128{1}, 9, wall_ms, 1000);
    MountLeaseRenewer renewer(
        ops.farewell, ops.lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(1000),
        [&] { return wall_ms; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(20),
        [&] { return boot_ms; });
    renewer.start();

    wall_ms = 9'000'000;
    EXPECT_EQ(renewer.renew(renewalEnvironment()).outcome, MountRenewOutcome::Committed);
    wall_ms = 1;
    EXPECT_EQ(renewer.renew(renewalEnvironment()).outcome, MountRenewOutcome::Committed);

    backend->attempts.clear();
    boot_ms += 10'000;
    const MountRenewResult suspended = renewer.renew(renewalEnvironment());
    ASSERT_EQ(suspended.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(suspended.attempt_start_boot_ms, boot_ms)
        << "a renewal after a suspend anchors at its own start, never at the deadline it last confirmed";
    EXPECT_EQ(renewer.lastCommittedAttemptStartBootMs(), boot_ms);
    EXPECT_EQ(backend->attempts.size(), 1u);
}

namespace
{
/// The production shape at test scale: a 30 s lease, renewed one 10 s period after its anchor, with a
/// 2 s safety margin and a 7 s attempt envelope.
class UnboundedRenewalFixture
{
public:
    static constexpr uint64_t ttl_ms = 30'000;
    static constexpr uint64_t period_ms = 10'000;
    static constexpr uint64_t margin_ms = 2'000;
    /// Far above the longest legitimate renewal (one request per second for a few lease lengths).
    static constexpr size_t max_requests = 500;

    UnboundedRenewalFixture()
    {
        backend->setAttemptTimeoutMs(7'000);
        ops = std::make_unique<Ops>(backend, &boot_ms);
        seedOwnClaim(ops->op, layout, "test", UInt128{1}, 9, wall_ms, ttl_ms);
        renewer = std::make_unique<MountLeaseRenewer>(
            ops->farewell, ops->lease, layout, "test", UInt128{1}, 9, std::chrono::milliseconds(ttl_ms),
            [this] { return wall_ms; }, [] { return uint64_t{0}; },
            [this](CasEvent event) { events.push_back(std::move(event)); },
            std::chrono::milliseconds(margin_ms), [this] { return boot_ms; });
        anchor = renewer->start();
        backend->attempts.clear();
        backend->read_calls = 0;
        events.clear();
        boot_ms = anchor + period_ms;
        renewal_start = boot_ms;
    }

    UnboundedRenewalFixture(const UnboundedRenewalFixture &) = delete;
    UnboundedRenewalFixture & operator=(const UnboundedRenewalFixture &) = delete;

    MountRenewResult renew(
        const std::function<bool()> & live = {},
        const std::function<bool()> & cancelled = {},
        std::function<void(const MountRenewRequestEvent &)> on_request = {})
    {
        /// The test clock moves only through the sleep seam, so a regression that zeroes every pause would
        /// loop forever; the request count bounds the renewal and the check below fails it instead.
        const auto bounded_live = [this, live]
        {
            if (backend->attempts.size() + backend->read_calls >= max_requests)
            {
                request_bound_hit = true;
                return false;
            }
            return !live || live();
        };
        MountRenewOperationEnvironment environment = renewalEnvironment(bounded_live, cancelled);
        environment.on_request = std::move(on_request);
        MountRenewResult result = renewer->renew(environment);
        EXPECT_FALSE(request_bound_hit) << "the renewal sent " << max_requests
            << " requests without the clock reaching the end of the outage: the spaced pauses are not advancing it";
        return result;
    }

    /// The `branch` of every `MountConflict` event, in order.
    std::vector<String> conflictBranches() const
    {
        std::vector<String> branches;
        for (const CasEvent & event : events)
            if (event.type == CasEventType::MountConflict)
                branches.push_back(event.detail.at("branch"));
        return branches;
    }

    String mountKey() const { return layout.mountKey("test"); }

    std::shared_ptr<RenewalScriptBackend> backend = std::make_shared<RenewalScriptBackend>();
    Layout layout{"pool"};
    uint64_t wall_ms = 1000;
    uint64_t boot_ms = 100;
    std::vector<CasEvent> events;
    std::unique_ptr<Ops> ops;
    std::unique_ptr<MountLeaseRenewer> renewer;
    uint64_t anchor = 0;
    uint64_t renewal_start = 0;
    bool request_bound_hit = false;
};

/// A request start (`P`) or a resolve read (`R`), with the boot clock when it was issued.
using RequestLog = std::vector<std::pair<char, uint64_t>>;

bool inSpacing(uint64_t gap_ms)
{
    return gap_ms >= kMountRenewRetrySpacingMs * 8 / 10 && gap_ms <= kMountRenewRetrySpacingMs * 12 / 10;
}
}

TEST(CASHeartbeat, RenewalOutlivesTheReservationCutoff)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 8'000; };

    const MountRenewResult result = f.renew();

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::Active);
    /// One request per spacing interval of 800 to 1200 ms until the outage ends, then the one that lands.
    EXPECT_GE(f.backend->attempts.size(), 8u);
    EXPECT_LE(f.backend->attempts.size(), 11u);
    EXPECT_EQ(static_cast<size_t>(result.attempts_sent), f.backend->attempts.size());
}

TEST(CASHeartbeat, RenewalSendsOneTupleOnEveryAttempt)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 8'000; };

    const MountRenewResult result = f.renew();

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    const auto & attempts = f.backend->attempts;
    ASSERT_GT(attempts.size(), 1u);
    const UInt128 attempt_id = decodeMountLease(attempts.front().bytes).write_attempt_id;
    for (const auto & attempt : attempts)
    {
        EXPECT_EQ(attempt.key, attempts.front().key);
        EXPECT_EQ(attempt.bytes, attempts.front().bytes);
        EXPECT_EQ(attempt.expected, attempts.front().expected);
        EXPECT_EQ(decodeMountLease(attempt.bytes).write_attempt_id, attempt_id);
    }
    EXPECT_EQ(f.ops->op.read(f.mountKey(), Retry::standard())->bytes, attempts.front().bytes)
        << "the body that landed is the one every attempt carried";
}

TEST(CASHeartbeat, RenewalSucceedsPastTheDeadlineWithItsFirstStart)
{
    UnboundedRenewalFixture f;
    f.backend->outage = [&] { return f.boot_ms < f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000; };

    const MountRenewResult result = f.renew();

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(result.attempt_start_boot_ms, f.renewal_start);
    EXPECT_EQ(f.renewer->lastCommittedAttemptStartBootMs(), f.renewal_start);
    EXPECT_LT(result.attempt_start_boot_ms + UnboundedRenewalFixture::ttl_ms, f.boot_ms)
        << "the lease this success confirms is already over";
}

TEST(CASHeartbeat, LandedAttemptIsAdoptedAfterALongOutage)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::LandThenThrow};
    f.backend->read_outage = [&] { return f.boot_ms < f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000; };
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        {}, {}, [&](const MountRenewRequestEvent & event) { reported.push_back(event); });
    const uint64_t reads = f.backend->read_calls;

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_TRUE(result.resolved_by_read);
    EXPECT_EQ(result.attempts_sent, 1u);
    EXPECT_GT(f.boot_ms, f.anchor + UnboundedRenewalFixture::ttl_ms) << "the adopting read came after the deadline";
    EXPECT_TRUE(f.conflictBranches().empty()) << "an adopted attempt is not a conflict such as same_epoch_state_uncertain";
    EXPECT_EQ(decodeMountLease(f.ops->op.read(f.mountKey(), Retry::standard())->bytes).write_attempt_id,
              decodeMountLease(f.backend->attempts.front().bytes).write_attempt_id);

    /// The one PUT sent, its failure, then one failed event per failed read; the last read succeeded.
    ASSERT_GE(reads, 2u);
    ASSERT_EQ(reported.size(), 2 + (reads - 1));
    EXPECT_FALSE(reported[0].failed);
    EXPECT_TRUE(reported[1].failed);
    EXPECT_NE(reported[1].failure_text.find("injected renewal response loss after commit"), String::npos)
        << reported[1].failure_text;
    for (size_t i = 2; i < reported.size(); ++i)
    {
        EXPECT_EQ(reported[i].request_no, 1u) << i;
        EXPECT_TRUE(reported[i].failed) << i << ": no request but the first PUT was sent";
        EXPECT_NE(reported[i].failure_text.find("injected renewal read outage"), String::npos)
            << i << ": " << reported[i].failure_text;
    }
}

/// Each definitive answer, met by a request sent past the deadline, ends the renewal with today's
/// classification.
TEST(CASHeartbeat, DefinitiveAnswersStayTerminalPastTheDeadline)
{
    enum class Answer : uint8_t { GcFenced, Foreign, NewerOwnEpoch, OwnEpochOtherBytes, Absent, StoreRefusal, LocalFailure };
    const auto run = [](Answer answer)
    {
        UnboundedRenewalFixture f;
        const String key = f.mountKey();
        const auto replace_slot = [&](const std::function<void(MountLease &)> & change)
        {
            const auto got = f.ops->op.read(key, Retry::standard());
            ASSERT_TRUE(got.has_value());
            MountLease lease = decodeMountLease(got->bytes);
            change(lease);
            ++lease.seq;
            mustCommit(f.ops->op.replace(key, encodeMountLease(lease), got->etag, Retry::standard()), "changed slot");
        };
        switch (answer)
        {
            case Answer::GcFenced:
                replace_slot([](MountLease & lease) { lease.gc_fenced = true; });
                break;
            case Answer::Foreign:
                replace_slot([](MountLease & lease) { lease.server_uuid = UInt128{2}; });
                break;
            case Answer::NewerOwnEpoch:
                replace_slot([](MountLease & lease) { lease.writer_epoch = 10; });
                break;
            case Answer::OwnEpochOtherBytes:
                replace_slot([](MountLease & lease) { lease.write_attempt_id = UInt128{0xAAAA}; });
                break;
            case Answer::Absent:
            {
                const auto got = f.ops->op.read(key, Retry::standard());
                ASSERT_TRUE(got.has_value());
                ASSERT_EQ(f.ops->op.remove(key, got->etag, Retry::standard()), Removal::Removed);
                break;
            }
            case Answer::StoreRefusal:
#if USE_AWS_S3
                f.backend->failNextWriteWith(key, std::make_exception_ptr(DB::S3Exception(
                    "the store answered MalformedXML", Aws::S3::S3Errors::UNKNOWN, "MalformedXML")));
#endif
                break;
            case Answer::LocalFailure:
                f.backend->failNextWriteWith(key, std::make_exception_ptr(DB::Exception(
                    DB::ErrorCodes::CORRUPTED_DATA, "injected deterministic local failure")));
                break;
        }
        f.backend->attempts.clear();
        f.events.clear();
        /// Past the lease: the renewal still sends, because it has no lease bound.
        f.boot_ms = f.anchor + UnboundedRenewalFixture::ttl_ms + 1'000;
        /// A definitive answer that was retried would never end this renewal; the bound makes it fail instead.
        const uint64_t live_until = f.boot_ms + 600'000;

        const MountRenewResult result = f.renew([&] { return f.boot_ms < live_until; });

        const DB::Exception failure = terminalException(result);
        EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_EQ(f.backend->attempts.size(), 1u) << "the answer came to a request sent past the deadline";
        const std::vector<String> branches = f.conflictBranches();
        switch (answer)
        {
            case Answer::GcFenced:
                EXPECT_EQ(branches, std::vector<String>{"fenced_by_gc"});
                break;
            case Answer::Foreign:
                EXPECT_EQ(branches, std::vector<String>{"foreign_writer"});
                break;
            case Answer::NewerOwnEpoch:
                EXPECT_EQ(branches, std::vector<String>{"superseded"});
                break;
            case Answer::OwnEpochOtherBytes:
                EXPECT_EQ(branches, std::vector<String>{"same_epoch_state_uncertain"});
                break;
            case Answer::Absent:
                EXPECT_EQ(branches, std::vector<String>{"vanished"});
                EXPECT_EQ(failure.code(), DB::ErrorCodes::FILE_DOESNT_EXIST) << failure.message();
                break;
            case Answer::StoreRefusal:
                EXPECT_TRUE(branches.empty());
                EXPECT_NE(failure.message().find("the store refused the renewal"), String::npos) << failure.message();
                break;
            case Answer::LocalFailure:
                EXPECT_TRUE(branches.empty());
                EXPECT_EQ(failure.code(), DB::ErrorCodes::CORRUPTED_DATA) << failure.message();
                EXPECT_NE(failure.message().find("injected deterministic local failure"), String::npos);
                break;
        }
    };
    for (Answer answer : {Answer::GcFenced, Answer::Foreign, Answer::NewerOwnEpoch, Answer::OwnEpochOtherBytes,
                          Answer::Absent, Answer::StoreRefusal, Answer::LocalFailure})
    {
#if !USE_AWS_S3
        if (answer == Answer::StoreRefusal)
            continue;
#endif
        SCOPED_TRACE(static_cast<int>(answer));
        run(answer);
    }
}

/// The cut-off node's story: renewals fail past the deadline, GC on a healthy node fences the slot,
/// and the first request that reaches the store reads the fence.
TEST(CASHeartbeat, AFenceSeenAfterALongOutageEndsTheRenewal)
{
    UnboundedRenewalFixture f;
    const uint64_t fenced_at = f.anchor + UnboundedRenewalFixture::ttl_ms + 15'000;
    bool fenced = false;
    f.backend->outage = [&] { return !fenced; };
    f.ops->lease.setSleepFnForTest([&](uint64_t ms)
    {
        f.boot_ms += ms;
        if (fenced || f.boot_ms < fenced_at)
            return;
        fenced = true;
        const auto got = f.ops->op.read(f.mountKey(), Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease lease = decodeMountLease(got->bytes);
        lease.gc_fenced = true;
        ++lease.seq;
        mustCommit(f.ops->op.replace(f.mountKey(), encodeMountLease(lease), got->etag, Retry::standard()), "fence-out");
    });

    /// A fence that was retried would never end this renewal; the bound makes it fail instead.
    const MountRenewResult result = f.renew(
        [&] { return f.boot_ms < f.renewal_start + 600'000; });

    const DB::Exception failure = terminalException(result);
    EXPECT_NE(failure.message().find("fenced by GC"), String::npos) << failure.message();
    EXPECT_EQ(f.conflictBranches(), std::vector<String>{"fenced_by_gc"});
    EXPECT_GE(f.boot_ms, fenced_at);
}

#if USE_AWS_S3
/// After an unclear attempt the engine cannot tell whether that attempt will still land, so a refusal
/// is not an answer about the slot: the renewal keeps retrying at the spacing until a stop ends it.
TEST(CASHeartbeat, ARefusalAfterAnUnclearAttemptIsRetriedUntilStopped)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
    f.backend->outage = [] { return true; };
    f.backend->outage_action = RenewalScriptBackend::Action::ThrowStoreRefusal;
    std::vector<uint64_t> sent_at;
    f.backend->on_attempt = [&] { sent_at.push_back(f.boot_ms); };
    bool stopped = false;
    f.ops->lease.setSleepFnForTest([&](uint64_t ms)
    {
        f.boot_ms += ms;
        if (f.boot_ms >= f.renewal_start + 30'000)
            stopped = true;
    });
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        /*live=*/[&] { return !stopped; }, /*cancelled=*/[&] { return stopped; },
        [&](const MountRenewRequestEvent & event) { reported.push_back(event); });

    const DB::Exception failure = terminalException(result);
    EXPECT_EQ(failure.code(), DB::ErrorCodes::NETWORK_ERROR) << failure.message();
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
    EXPECT_TRUE(f.conflictBranches().empty()) << "the read kept showing our own unchanged body";

    /// One PUT per spacing interval over the 30 s the store kept refusing.
    ASSERT_GE(sent_at.size(), 2u);
    for (size_t i = 1; i < sent_at.size(); ++i)
        EXPECT_TRUE(inSpacing(sent_at[i] - sent_at[i - 1])) << i << ": " << sent_at[i] - sent_at[i - 1];
    EXPECT_GE(sent_at.size(), 30'000 / (kMountRenewRetrySpacingMs * 12 / 10));
    EXPECT_LE(sent_at.size(), 30'000 / (kMountRenewRetrySpacingMs * 8 / 10) + 1);

    /// Every refusal was reported as a failed request.
    size_t refusals = 0;
    for (const MountRenewRequestEvent & event : reported)
        if (event.failed && event.failure_text.find("MalformedXML") != String::npos)
            ++refusals;
    EXPECT_EQ(refusals, sent_at.size() - 1) << "every PUT after the unclear first one was refused";
}
#endif

TEST(CASHeartbeat, RenewalSpacesRetries)
{
#if USE_AWS_S3
    {
        SCOPED_TRACE("fast connect failures");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->actions = {RenewalScriptBackend::Action::ThrowFirstAttemptFuse};
        f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 30'500; };
        f.backend->outage_action = RenewalScriptBackend::Action::ThrowConnectHint;

        ASSERT_EQ(f.renew().outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 4u);
        /// The fuse: its settling read and its reissue follow at once.
        EXPECT_EQ(log[0], std::make_pair('P', f.renewal_start));
        EXPECT_EQ(log[1], std::make_pair('R', f.renewal_start));
        EXPECT_EQ(log[2], std::make_pair('P', f.renewal_start));
        /// A connect failure is reissued without a read, one spacing interval after it started.
        for (size_t i = 3; i < log.size(); ++i)
        {
            EXPECT_EQ(log[i].first, 'P') << i;
            EXPECT_TRUE(inSpacing(log[i].second - log[i - 1].second)) << i << ": " << log[i].second - log[i - 1].second;
        }
        EXPECT_GE(log.back().second, f.renewal_start + 30'000) << "still renewing after 30 s";
    }
#endif
    {
        SCOPED_TRACE("unclear PUT with a failing read");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->read_actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};
        f.backend->outage = [&] { return f.boot_ms < f.renewal_start + 30'500; };

        ASSERT_EQ(f.renew().outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 6u);
        const uint64_t t0 = f.renewal_start;
        EXPECT_EQ(log[0], std::make_pair('P', t0));
        EXPECT_EQ(log[1], std::make_pair('R', t0)) << "the read that settles an unclear PUT is sent at once";
        EXPECT_EQ(log[2].first, 'R');
        EXPECT_TRUE(inSpacing(log[2].second - log[1].second)) << log[2].second - log[1].second;
        EXPECT_EQ(log[3].first, 'R');
        EXPECT_TRUE(inSpacing(log[3].second - log[2].second)) << log[3].second - log[2].second;
        EXPECT_EQ(log[4], std::make_pair('P', log[3].second))
            << "more than one interval has passed since the PUT started, so it is retried at once";
        uint64_t previous_put = log[4].second;
        for (size_t i = 5; i < log.size(); ++i)
        {
            if (log[i].first == 'R')
            {
                EXPECT_EQ(log[i].second, log[i - 1].second) << i;
                continue;
            }
            EXPECT_TRUE(inSpacing(log[i].second - previous_put)) << i << ": " << log[i].second - previous_put;
            previous_put = log[i].second;
        }
        EXPECT_GE(previous_put, t0 + 30'000) << "still renewing after 30 s";
    }
#if USE_AWS_S3
    {
        SCOPED_TRACE("first-attempt fuse of the read");
        UnboundedRenewalFixture f;
        RequestLog log;
        f.backend->on_attempt = [&] { log.emplace_back('P', f.boot_ms); };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore};
        f.backend->read_actions = {RenewalScriptBackend::Action::ThrowFirstAttemptFuse};

        ASSERT_EQ(f.renew().outcome, MountRenewOutcome::Committed);

        ASSERT_EQ(log.size(), 4u);
        EXPECT_EQ(log[1], std::make_pair('R', f.renewal_start));
        EXPECT_EQ(log[2], std::make_pair('R', f.renewal_start)) << "a fused read is reissued at once";
        EXPECT_EQ(log[3].first, 'P');
        EXPECT_TRUE(inSpacing(log[3].second - log[0].second)) << log[3].second - log[0].second;
    }
#endif
    {
        SCOPED_TRACE("slow requests");
        UnboundedRenewalFixture f;
        RequestLog log;
        bool failing = false;
        f.backend->on_attempt = [&]
        {
            log.emplace_back('P', f.boot_ms);
            failing = f.boot_ms < f.renewal_start + 30'500;
            if (failing)
                f.boot_ms += 5'000;
        };
        f.backend->on_read = [&] { log.emplace_back('R', f.boot_ms); };
        f.backend->outage = [&] { return failing; };

        ASSERT_EQ(f.renew().outcome, MountRenewOutcome::Committed);

        ASSERT_GE(log.size(), 3u);
        std::optional<uint64_t> previous_put;
        for (size_t i = 0; i < log.size(); ++i)
        {
            if (log[i].first == 'R')
            {
                EXPECT_EQ(log[i].second, log[i - 1].second + 5'000) << i;
                continue;
            }
            if (previous_put)
                EXPECT_EQ(log[i].second, *previous_put + 5'000) << i << ": a 5 s request is retried with no wait";
            previous_put = log[i].second;
        }
        EXPECT_GE(*previous_put, f.renewal_start + 30'000) << "still renewing after 30 s";
    }
}

TEST(CASHeartbeat, StopDuringARetryWaitEndsTheRenewal)
{
    {
        SCOPED_TRACE("a stop during the wait");
        UnboundedRenewalFixture f;
        bool stopped = false;
        std::vector<uint64_t> waits;
        f.backend->outage = [] { return true; };
        /// The stop wakes the wait, so the clock does not move.
        f.ops->lease.setSleepFnForTest([&](uint64_t ms)
        {
            waits.push_back(ms);
            stopped = true;
        });

        const MountRenewResult result = f.renew(
            /*live=*/[&] { return !stopped; }, /*cancelled=*/[&] { return stopped; });

        const DB::Exception failure = terminalException(result);
        EXPECT_EQ(failure.code(), DB::ErrorCodes::NETWORK_ERROR) << failure.message();
        ASSERT_EQ(waits.size(), 1u);
        EXPECT_TRUE(inSpacing(waits[0])) << waits[0];
        EXPECT_EQ(f.backend->attempts.size(), 1u) << "nothing is sent after the stop";
        EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::RenewalTerminal);
        EXPECT_FALSE(f.renewer->canRelease());
    }
    {
        SCOPED_TRACE("a stop after the renewal began, before its first send");
        UnboundedRenewalFixture f;

        const MountRenewResult result = f.renew(
            /*live=*/[] { return false; }, /*cancelled=*/[] { return false; });

        (void)terminalException(result);
        EXPECT_TRUE(f.backend->attempts.empty());
        EXPECT_FALSE(f.renewer->canRelease());
    }
}

TEST(CASHeartbeat, RenewalReportsEachRequestAsItHappens)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};
    std::vector<MountRenewRequestEvent> reported;

    const MountRenewResult result = f.renew(
        {}, {}, [&](const MountRenewRequestEvent & event) { reported.push_back(event); });

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    const std::vector<std::pair<uint32_t, bool>> expected{{1, false}, {1, true}, {2, false}, {2, true}, {3, false}};
    ASSERT_EQ(reported.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_EQ(reported[i].request_no, expected[i].first) << i;
        EXPECT_EQ(reported[i].failed, expected[i].second) << i;
        if (reported[i].failed)
            EXPECT_NE(reported[i].failure_text.find("injected renewal response uncertainty"), String::npos)
                << reported[i].failure_text;
        else
            EXPECT_TRUE(reported[i].failure_text.empty()) << i;
    }
}

TEST(CASHeartbeat, AThrowingRequestReportChangesNoOutcome)
{
    UnboundedRenewalFixture f;
    f.backend->actions = {RenewalScriptBackend::Action::ThrowBefore, RenewalScriptBackend::Action::ThrowBefore};

    const MountRenewResult result = f.renew(
        {}, {},
        [](const MountRenewRequestEvent &) { throw std::runtime_error("injected report failure"); });

    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
    EXPECT_EQ(result.attempts_sent, 3u);
    EXPECT_EQ(f.renewer->state(), MountLeaseRenewerState::Active);
}

