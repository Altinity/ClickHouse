#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <Poco/Exception.h>
#include <Poco/Message.h>
#include <Poco/StreamChannel.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
}

namespace ProfileEvents
{
    extern const Event CASMountLeaseLost;
    extern const Event CASMountExclusivityViolation;
}

using namespace DB::Cas;

namespace
{

const ObserveRefCatalog & emptyCatalogObservation()
{
    static const ObserveRefCatalog observe = [] { return RefCatalog{}; };
    return observe;
}

RefCatalog catalogOwning(const String & ns, NsState state)
{
    CatalogEntry entry{.ns = RootNamespace{ns}, .state = state, .incarnation = UInt128{42}};
    if (state == NsState::Creating)
        entry.creator = CreatorFence{.server_root_id = "root/x", .writer_epoch = 1, .fence_generation = 1};
    return RefCatalog{.entries = {std::move(entry)}};
}

void renewOrThrow(MountLeaseRenewer & renewer)
{
    const MountRenewResult result = renewer.renew(MountRenewOperationEnvironment{});
    if (result.outcome == MountRenewOutcome::Terminal)
        std::rethrow_exception(result.failure);
    ASSERT_EQ(result.outcome, MountRenewOutcome::Committed);
}

/// The two request planes a renewer in this file runs on, plus one operation for the protocol calls
/// driven directly. Both planes are open-fence: these fixtures hold no mount lease, so nothing here
/// should be refused by a fence it does not have. The clock and the sleep are ALWAYS injected -- a
/// fixture that drives a lease deadline passes its own so a slow machine cannot run the bound out
/// mid-test, and one that does not still must not sleep for real when a fault sends the engine round
/// again. `tests::OperationForTest` covers the one-operation case but neither the two planes nor the
/// clock, which is why this stays local.
class Ops
{
public:
    explicit Ops(std::shared_ptr<Backend> backend) : Ops(std::move(backend), nullptr) {}

    Ops(std::shared_ptr<Backend> backend, uint64_t * boot_ms)
        : mount(openRequestsForTest(backend))
        , farewell(openRequestsForTest(std::move(backend)))
        , op(mount.admit())
    {
        uint64_t * clock = boot_ms ? boot_ms : &own_clock;
        for (CasRequests * requests : {&mount, &farewell})
        {
            requests->setNowFnForTest([clock] { return *clock; });
            requests->setSleepFnForTest([clock](uint64_t ms) { *clock += ms; });
        }
    }

    Ops(const Ops &) = delete;
    Ops & operator=(const Ops &) = delete;

    CasRequests mount;
    CasRequests farewell;
    CasOperation op;

private:
    uint64_t own_clock = 0;
};

/// The incarnation currently at `key`, for a fixture that has to name it as a precondition.
Etag currentEtag(CasOperation & op, const String & key)
{
    const auto got = op.read(key, Retry::standard());
    if (!got)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "test fixture read of '{}' found nothing", key);
    return got->etag;
}

/// A fixture write that must land, so a mis-seeded fixture fails where it is written rather than in
/// the assertion it silently invalidated.
void mustCommit(WriteResult && result, const String & what)
{
    if (!std::holds_alternative<Committed>(result))
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "test fixture write '{}' did not commit", what);
}

class OwnerConflictRevealsManifestBackend : public InMemoryBackend
{
public:
    std::expected<String, RawConflict> write(
        const String & key, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!fired && !expected_value && key == "p/gc/server-roots/root/x/owner")
        {
            fired = true;
            InMemoryBackend::write("p/cas/manifests/root/x/table/debris", "x", std::nullopt, access);
            return std::unexpected(RawConflict{});
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    bool fired = false;
};

/// Loses the owner key to a racing claimer between the read and the create: installs `winner`'s owner
/// object, then refuses this write. The subtree stays empty, so the emptiness recompute passes and the
/// claim has to decide the race from what its own write observed.
class OwnerRaceBackend : public InMemoryBackend
{
public:
    explicit OwnerRaceBackend(UInt128 winner_) : winner(winner_) {}

    /// Counts only reads of the owner key, so an extra re-read the conflict decision no longer needs
    /// is visible even though the resolve read on the same key already counts once.
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key == "p/gc/server-roots/r/owner")
            ++owner_reads;
        return InMemoryBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!fired && !expected_value && key == "p/gc/server-roots/r/owner")
        {
            fired = true;
            InMemoryBackend::write(
                key, encodeOwner(OwnerObject{.server_uuid = winner, .retired_at_ms = std::nullopt}),
                std::nullopt, access);
            return std::unexpected(RawConflict{});
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    bool fired = false;
    size_t owner_reads = 0;

private:
    UInt128 winner;
};

/// Owner ETags derived from the bytes, as S3 derives them: equal bytes bring back an equal ETag.
class ContentEtagOwnerBackend : public InMemoryBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = InMemoryBackend::read(key, access);
        if (raw && isOwner(key))
            raw->value = contentValue(raw->bytes);
        return raw;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = InMemoryBackend::head(key, access);
        if (meta && isOwner(key))
            if (const auto raw = InMemoryBackend::read(key, access))
                meta->value = contentValue(raw->bytes);
        return meta;
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!isOwner(key))
            return InMemoryBackend::write(key, bytes, expected_value, access);
        std::optional<String> minted_expected;
        if (expected_value)
        {
            const auto current = InMemoryBackend::read(key, access);
            if (!current || contentValue(current->bytes) != *expected_value)
                return std::unexpected(RawConflict{});
            minted_expected = current->value;
        }
        const auto written = InMemoryBackend::write(key, bytes, minted_expected, access);
        if (!written)
            return written;
        return contentValue(bytes);
    }

private:
    static bool isOwner(const String & key) { return key.ends_with("/owner"); }
    static String contentValue(const String & bytes) { return "content-" + DB::Cas::tests::hexOf(bytes); }
};

/// Refuses the FIRST write of the epoch key after installing a competing allocator's own epoch, so the
/// absent-epoch decision has to be made a second time. `reveal_owned_work` decides whether owned work
/// becomes visible at that same instant -- the fact the second decision must re-establish.
class EpochConflictBackend : public InMemoryBackend
{
public:
    explicit EpochConflictBackend(bool reveal_owned_work_ = true) : reveal_owned_work(reveal_owned_work_) {}

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (!fired && key == "p/gc/server-roots/root/x/epoch")
        {
            fired = true;
            const auto winner = InMemoryBackend::write(
                key, encodeServerEpoch(ServerEpoch{.next_writer_epoch = 2}), expected_value, access);
            winner_installed = winner.has_value();
            if (reveal_owned_work)
                InMemoryBackend::write("p/cas/manifests/root/x/table/debris", "x", std::nullopt, access);
            return std::unexpected(RawConflict{});
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    bool fired = false;
    bool winner_installed = false;

private:
    bool reveal_owned_work;
};

/// Between this start's epoch allocation and its claim read, a second start of the same server
/// allocates the next epoch and installs its live body. Armed by the test after the bootstrap open.
class NewerStartBackend : public InMemoryBackend
{
public:
    String epoch_key;
    String mount_key;
    bool arm_on_next_epoch_write = false;
    std::optional<uint64_t> newer_epoch;

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        auto result = InMemoryBackend::write(key, bytes, expected_value, access);
        if (arm_on_next_epoch_write && key == epoch_key && result)
        {
            arm_on_next_epoch_write = false;
            install_on_next_mount_read = true;
        }
        return result;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (install_on_next_mount_read && key == mount_key)
        {
            install_on_next_mount_read = false;
            installNewerStart(access);
        }
        return InMemoryBackend::read(key, access);
    }

private:
    bool install_on_next_mount_read = false;

    void installNewerStart(TransportAccess & access)
    {
        const auto epoch = InMemoryBackend::read(epoch_key, access);
        const auto mount = InMemoryBackend::read(mount_key, access);
        if (!epoch || !mount)
            throw std::runtime_error("NewerStartBackend: a control object is missing");
        ServerEpoch counter = decodeServerEpoch(epoch->bytes);
        newer_epoch = counter.next_writer_epoch;
        ++counter.next_writer_epoch;
        if (!InMemoryBackend::write(epoch_key, encodeServerEpoch(counter), epoch->value, access))
            throw std::runtime_error("NewerStartBackend: the epoch bump conflicted");
        const MountLease newer{
            .server_uuid = UInt128(1),
            .writer_epoch = *newer_epoch,
            .hostname = "newer-start",
            .pid = 2,
            .started_at_ms = 1,
            .seq = 1,
            .expires_at_ms = 4'000'000'000'000,
            .write_attempt_id = UInt128{0xB}};
        if (!InMemoryBackend::write(mount_key, encodeMountLease(newer), mount->value, access))
            throw std::runtime_error("NewerStartBackend: the mount install conflicted");
    }
};

/// Records every write of `key`; while `lose_first` is set, the first one times out without applying.
class UnappliedFirstWriteBackend : public InMemoryBackend
{
public:
    String key;
    bool lose_first = false;
    std::vector<std::pair<String, std::optional<String>>> writes;

    std::expected<String, RawConflict> write(
        const String & k, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (k == key)
        {
            writes.emplace_back(bytes, expected_value);
            if (std::exchange(lose_first, false))
                throw Poco::TimeoutException("UnappliedFirstWriteBackend: response lost, the attempt did not apply");
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }
};

/// Applies the first conditional write of `key` after arming, then loses its response.
class LandedReplaceAckLostOnceBackend : public InMemoryBackend
{
public:
    String key;
    bool armed = false;

    std::expected<String, RawConflict> write(
        const String & k, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (armed && expected_value && k == key)
        {
            armed = false;
            (void)InMemoryBackend::write(k, bytes, expected_value, access);
            throw Poco::TimeoutException("LandedReplaceAckLostOnceBackend: simulated lost PUT response");
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }
};

/// On the first write of `key` after `competitor` is set, runs `competitor` (another allocator of the
/// same uuid) to completion, then times out our write without applying it.
class TwinAllocatorBackend : public InMemoryBackend
{
public:
    String key;
    std::function<void()> competitor;

    std::expected<String, RawConflict> write(
        const String & k, const String & bytes,
        const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (competitor && k == key)
        {
            std::exchange(competitor, {})();
            throw Poco::TimeoutException("TwinAllocatorBackend: response lost, our attempt did not apply");
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }
};

/// Counts every request that reaches the store, per primitive, so a test can pin how many a protocol
/// step costs rather than only what it produced.
class RequestCountingBackend final : public InMemoryBackend
{
public:
    size_t reads = 0;
    size_t heads = 0;
    size_t writes = 0;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        ++reads;
        return InMemoryBackend::read(key, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        ++heads;
        return InMemoryBackend::head(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        ++writes;
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};

class RenewalLogBackend final : public InMemoryBackend
{
public:
    bool throw_before_next_overwrite = false;

    /// The fault lives on the primitive every write reaches the store through, keyed to the mount
    /// slot so the pool's other conditional writes pass untouched.
    std::expected<String, RawConflict> write(
        const String & key,
        const String & bytes,
        const std::optional<String> & expected_value,
        TransportAccess & access) override
    {
        if (expected_value && key.ends_with("/mount") && std::exchange(throw_before_next_overwrite, false))
            throw Poco::TimeoutException("injected renewal timeout before commit");
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }
};

size_t countRenewalLogText(const String & haystack, std::string_view needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = haystack.find(needle, pos)) != String::npos; pos += needle.size())
        ++count;
    return count;
}

CasRequestBudget renewalLogBudget()
{
    return CasRequestBudget{
        .attempt_timeout_ms = 10,
        .lease_safety_margin_ms = 20,
        .connect_timeout_cap_ms = std::nullopt,
    };
}

}

/// `CASMountAudit.PhysicalRetryCannotBeDelayedByDebugLogging` was retired when mount renewal moved onto
/// `CasRequests`/`CasOperation` (the old hand-written renewal controller had a per-attempt progress
/// callback the test used to interleave a blocking debug log with the retry loop's own pacing; nothing
/// still exposes such a callback). Verified still true against the current engine, not just the
/// migration's own commit message: `grep -n "LOG_\|getLogger" .../Backend/CasRequests.cpp` finds exactly
/// one log call in the whole write-retry engine, `logCasWriteRetryLater`, reached only from the
/// `[[noreturn]]` `throwCasWriteRetryLater` -- the terminal give-up, called once, never between
/// attempts. No replacement test is needed: there is no per-attempt log call left to race.
TEST(CASMountAudit, RenewalDefaultLogsAreBounded)
{
    /// `boot_ms` is a shared, heap-owned atomic, not a plain reference parameter: the last block below
    /// mutates it after the Pool exists, and the Pool can outlive this lambda's own call (a background
    /// publish holds `shared_from_this()`), so a by-reference capture of a caller-local would dangle.
    const auto open_store = [](const std::shared_ptr<RenewalLogBackend> & backend,
        const std::shared_ptr<std::atomic<uint64_t>> & boot_ms, const String & prefix)
    {
        /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the
        /// budget field alone; pair the two so the fence math below matches what admits.
        backend->setAttemptTimeoutMs(renewalLogBudget().attempt_timeout_ms);
        return Pool::open(backend, PoolConfig{
            .pool_prefix = prefix,
            .server_root_id = "test",
            .mount_lease_ttl_ms = std::chrono::milliseconds(1000),
            .cas_request_budget = renewalLogBudget(),
            .boot_ms_fn = [boot_ms]
            {
                return boot_ms->load();
            },
        });
    };

    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-silent");
        DB::Cas::tests::ScopedLoggerCapture capture("CasMountLeaseRenewer", "information");
        EXPECT_NO_THROW(store->renewWatermarkOnce());
        EXPECT_EQ(countRenewalLogText(capture.captured(), "CAS mount renewal"), 0u);
    }

    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-recovered");
        DB::Cas::tests::ScopedLoggerCapture capture("CasMountLeaseRenewer", "information");
        backend->throw_before_next_overwrite = true;
        EXPECT_NO_THROW(store->renewWatermarkOnce());
        const String output = capture.captured();
        EXPECT_EQ(countRenewalLogText(output, "CAS mount renewal"), 1u) << output;
        EXPECT_EQ(countRenewalLogText(output, "recovered"), 1u) << output;
        EXPECT_EQ(countRenewalLogText(output, "physical retry attempt"), 0u) << output;
    }

    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-debug");
        DB::Cas::tests::ScopedLoggerCapture capture("CasMountLeaseRenewer", "debug");
        backend->throw_before_next_overwrite = true;
        EXPECT_NO_THROW(store->renewWatermarkOnce());
        EXPECT_EQ(countRenewalLogText(capture.captured(), "physical retry attempt 2"), 1u);
    }

    {
        auto backend = std::make_shared<RenewalLogBackend>();
        auto boot_ms = std::make_shared<std::atomic<uint64_t>>(100);
        auto store = open_store(backend, boot_ms, "renewal-log-fenced");
        DB::Cas::tests::ScopedLoggerCapture capture("CasMountLeaseRenewer", "information");
        /// The lease was claimed at boot 100 with the 1000 ms TTL above, so it expires at 1100. The
        /// fence admits only while the remaining time strictly clears the safety margin plus whatever
        /// the attempt reserves, so exactly `margin` remaining (with the reservation on top) refuses.
        boot_ms->store(1100 - renewalLogBudget().lease_safety_margin_ms);
        EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);
        const String output = capture.captured();
        EXPECT_EQ(countRenewalLogText(output, "CAS mount renewal"), 1u) << output;
        EXPECT_EQ(countRenewalLogText(output, "fenced"), 1u) << output;
    }
}

TEST(CASServerRootId, ValidationAcceptsCleanPathsRejectsBad)
{
    EXPECT_NO_THROW(validateServerRootId("replica-a"));
    EXPECT_NO_THROW(validateServerRootId("shard-01/replica-a"));
    EXPECT_THROW(validateServerRootId(""), DB::Exception);
    EXPECT_THROW(validateServerRootId("/replica"), DB::Exception);
    EXPECT_THROW(validateServerRootId("replica/"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a//b"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a/../b"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a/_files/b"), DB::Exception);
}

TEST(CASServerRoot, KeysAndCodecsRoundTrip)
{
    Layout layout("p");

    /// Layout keys under gc/server-roots/<srid>/.
    EXPECT_EQ(layout.serverRootPrefix("replica-a"), "p/gc/server-roots/replica-a/");
    EXPECT_EQ(layout.ownerKey("replica-a"), "p/gc/server-roots/replica-a/owner");
    EXPECT_EQ(layout.epochKey("replica-a"), "p/gc/server-roots/replica-a/epoch");
    EXPECT_EQ(layout.mountKey("replica-a"), "p/gc/server-roots/replica-a/mount");

    /// Owner round-trip.
    {
        OwnerObject o;
        o.server_uuid = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
        const OwnerObject back = decodeOwner(encodeOwner(o));
        EXPECT_EQ(back.server_uuid, o.server_uuid);
    }

    /// ServerEpoch round-trip.
    {
        ServerEpoch e;
        e.next_writer_epoch = 4242;
        const ServerEpoch back = decodeServerEpoch(encodeServerEpoch(e));
        EXPECT_EQ(back.next_writer_epoch, e.next_writer_epoch);
    }

    /// MountLease round-trip.
    {
        MountLease m;
        m.server_uuid = (UInt128(0xdeadbeefcafef00dULL) << 64) | UInt128(0x0011223344556677ULL);
        m.writer_epoch = 7;
        m.hostname = "host-1.example.com";
        m.pid = 12345;
        m.started_at_ms = 1700000000000ULL;
        m.seq = 99;
        m.expires_at_ms = 1700000030000ULL;
        m.write_attempt_id = UInt128{1};
        const MountLease back = decodeMountLease(encodeMountLease(m));
        EXPECT_EQ(back.server_uuid, m.server_uuid);
        EXPECT_EQ(back.writer_epoch, m.writer_epoch);
        EXPECT_EQ(back.hostname, m.hostname);
        EXPECT_EQ(back.pid, m.pid);
        EXPECT_EQ(back.started_at_ms, m.started_at_ms);
        EXPECT_EQ(back.seq, m.seq);
        EXPECT_EQ(back.expires_at_ms, m.expires_at_ms);
    }

    /// Fail-closed decode on garbage bytes.
    EXPECT_THROW(decodeOwner("not-a-proto-with-magic"), DB::Exception);
    EXPECT_THROW(decodeServerEpoch(""), DB::Exception);
    EXPECT_THROW(decodeMountLease(""), DB::Exception);
}

TEST(CASServerRootClaim, OwnerStickyAndForeignFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    EXPECT_NO_THROW(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()));     // fresh empty root → claim
    EXPECT_NO_THROW(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()));     // same uuid → ok
    try
    {
        claimOwnerOrThrow(ops.op, l, "r", UInt128(2), emptyCatalogObservation());
        FAIL() << "expected a foreign owner to fail closed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(e.message().find("<cas_server_root_id>"), String::npos) << e.message();
    }
}

TEST(CASServerRootClaim, TombstonedSameOwnerFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    mustCommit(ops.op.create(l.ownerKey("r"), encodeOwner(OwnerObject{
        .server_uuid = UInt128(1),
        .retired_at_ms = 1752537600000ULL,
    }), Retry::standard()), "tombstoned owner");

    try
    {
        claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
        FAIL() << "expected a tombstoned owner claim to fail closed";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_NE(e.message().find("decommissioned"), String::npos) << e.message();
    }
}

TEST(CASServerRootClaim, FirstClaimWritesLifeOne)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    EXPECT_EQ(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()), 1u);
    const OwnerObject stored = decodeOwner(ops.op.read(l.ownerKey("r"), Retry::standard())->bytes);
    EXPECT_EQ(stored.owner_life, 1u);
    EXPECT_FALSE(stored.retirement_verified);
    /// A restart under the same owner writes nothing and reads the same life.
    const Etag before = currentEtag(ops.op, l.ownerKey("r"));
    EXPECT_EQ(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()), 1u);
    EXPECT_EQ(currentEtag(ops.op, l.ownerKey("r")), before);
}

TEST(CASServerRootClaim, ClaimOverA2664OwnerReadsLifeZero)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    mustCommit(ops.op.create(l.ownerKey("r"), encodeOwner(OwnerObject{.server_uuid = UInt128(1), .retired_at_ms = std::nullopt}), Retry::standard()),
               "owner without a life");
    EXPECT_EQ(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()), 0u);
}

TEST(CASServerRootClaim, DecommissionPolicyAcceptsARetiredOwnerOfTheSameUuid)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    mustCommit(ops.op.create(l.ownerKey("r"), encodeOwner(OwnerObject{
        .server_uuid = UInt128(1), .retired_at_ms = 1752537600000ULL, .owner_life = 2}), Retry::standard()), "tombstone");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation(), RetiredOwnerPolicy::Refuse);
    });
    EXPECT_EQ(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation(),
                                RetiredOwnerPolicy::AcceptForDecommission), 2u);
    /// A retired owner of ANOTHER uuid is still foreign under either policy; the refusal also names a duplicate id.
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "same <cas_server_root_id>", [&]
    {
        claimOwnerOrThrow(ops.op, l, "r", UInt128(2), emptyCatalogObservation(), RetiredOwnerPolicy::AcceptForDecommission);
    });
}

/// The owner appears between the claim's read and its create: the conflict re-read applies the same retired
/// policy as the first read.
TEST(CASServerRootClaim, ConflictRereadAppliesTheRetiredPolicy)
{
    for (const RetiredOwnerPolicy policy :
         {RetiredOwnerPolicy::Refuse, RetiredOwnerPolicy::AcceptForDecommission, RetiredOwnerPolicy::RejoinIfVerified})
    {
        SCOPED_TRACE(static_cast<int>(policy));
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        bool raced = false;
        b->onBeforeWrite(l.ownerKey("r"), [&]
        {
            if (std::exchange(raced, true))
                return;
            Ops racer(b);
            mustCommit(racer.op.create(l.ownerKey("r"), encodeOwner(OwnerObject{
                .server_uuid = UInt128(1), .retired_at_ms = 1752537600000ULL, .owner_life = 2}), Retry::standard()),
                "racing tombstone");
        });
        if (policy != RetiredOwnerPolicy::AcceptForDecommission)
        {
            DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "decommission is not finished", [&]
            {
                claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation(), policy);
            });
        }
        else
        {
            EXPECT_EQ(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation(), policy), 2u);
        }
        EXPECT_TRUE(raced);
    }
}

/// A start over an unverified tombstone is refused and writes neither the counter nor the slot.
TEST(CASMount, PoolOpenRefusesAnUnverifiedRetirement)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    { auto first = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"}); }
    Ops ops(b);
    const auto owner = ops.op.read(l.ownerKey("r"), Retry::standard());
    OwnerObject tombstone = decodeOwner(owner->bytes);
    tombstone.retired_at_ms = 1;
    tombstone.owner_life += 1;
    tombstone.retirement_verified = false;
    mustCommit(ops.op.replace(l.ownerKey("r"), encodeOwner(tombstone), owner->etag, Retry::standard()), "tombstone");
    const Etag epoch_before = currentEtag(ops.op, l.epochKey("r"));
    const Etag mount_before = currentEtag(ops.op, l.mountKey("r"));
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "decommission is not finished", [&]
    {
        Pool::open(b, PoolConfig{.pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});
    });
    EXPECT_EQ(currentEtag(ops.op, l.epochKey("r")), epoch_before);
    EXPECT_EQ(currentEtag(ops.op, l.mountKey("r")), mount_before);
}

TEST(CASServerRootEpoch, AllocatorIsMonotoneAndSurvivesMountConcept)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("r");
    Ops ops(b);
    claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
    const uint64_t e1 = allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation());
    const uint64_t e2 = allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation());
    EXPECT_GE(e1, 1u);                                             // 0 is a reserved sentinel
    EXPECT_GT(e2, e1);                                             // strictly increasing

    /// Deleting the (separate) mount object must NOT reset the epoch. No mount has been written yet,
    /// so the removal is a no-op that touches nothing.
    ASSERT_FALSE(ops.op.head(l.mountKey("r"), Retry::standard()).has_value());
    EXPECT_EQ(ops.op.removeCurrent(l.mountKey("r"), Retry::standard()), Removal::Gone);
    EXPECT_GT(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), e2);
}

/// Our counter write is lost while a twin of the same uuid allocates the same next epoch. Its counter
/// body must not read back as our landed write, on both the create and the replace path.
TEST(CASServerRootEpoch, TwinAllocatorsNeverShareAnEpochAfterALostWrite)
{
    for (const bool counter_exists : {false, true})
    {
        SCOPED_TRACE(counter_exists ? "replace" : "create");
        auto b = std::make_shared<TwinAllocatorBackend>();
        Layout l("p");
        Ops ours(b);
        Ops twin(b);
        b->key = l.epochKey("r");
        if (counter_exists)
            ASSERT_EQ(allocateWriterEpoch(ours.op, l, "r", emptyCatalogObservation()), 1u);

        uint64_t twin_epoch = 0;
        b->competitor = [&]
        {
            twin_epoch = allocateWriterEpoch(twin.op, l, "r", emptyCatalogObservation());
        };
        const uint64_t our_epoch
            = allocateWriterEpoch(ours.op, l, "r", emptyCatalogObservation());
        ASSERT_FALSE(b->competitor) << "the twin never ran";
        ASSERT_NE(twin_epoch, 0u);
        EXPECT_NE(our_epoch, twin_epoch);
        EXPECT_EQ(decodeServerEpoch(ours.op.read(l.epochKey("r"), Retry::standard())->bytes).next_writer_epoch,
                  std::max(our_epoch, twin_epoch) + 1);
    }
}

/// A resend of one allocation decision carries that decision's nonce, so our own landed write with a
/// lost response is recognized as ours instead of costing a second epoch.
TEST(CASServerRootEpoch, LandedCounterWriteWithLostResponseIsOurs)
{
    auto b = std::make_shared<LandedReplaceAckLostOnceBackend>();
    Layout l("p");
    Ops ops(b);
    b->key = l.epochKey("r");
    ASSERT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 1u);

    b->armed = true;
    EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 2u);
    ASSERT_FALSE(b->armed) << "the lost response was never injected";
    EXPECT_EQ(decodeServerEpoch(ops.op.read(l.epochKey("r"), Retry::standard())->bytes).next_writer_epoch, 3u);
}

/// An unapplied write with an unclear outcome is reissued inside the same decision: every physical
/// attempt must carry the same body, nonce included, under the same precondition.
TEST(CASServerRootEpoch, ReissuedCounterWriteKeepsItsNonce)
{
    for (const bool counter_exists : {false, true})
    {
        SCOPED_TRACE(counter_exists ? "replace" : "create");
        auto b = std::make_shared<UnappliedFirstWriteBackend>();
        Layout l("p");
        Ops ops(b);
        b->key = l.epochKey("r");
        if (counter_exists)
            ASSERT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 1u);

        b->writes.clear();
        b->lose_first = true;
        const uint64_t expected_epoch = counter_exists ? 2 : 1;
        EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()),
                  expected_epoch);
        ASSERT_FALSE(b->lose_first) << "the lost write was never injected";
        ASSERT_GE(b->writes.size(), 2u) << "the unclear write was never reissued";
        for (const auto & [bytes, precondition] : b->writes)
        {
            EXPECT_EQ(bytes, b->writes.front().first);
            EXPECT_EQ(precondition, b->writes.front().second);
        }
        const ServerEpoch stored = decodeServerEpoch(ops.op.read(l.epochKey("r"), Retry::standard())->bytes);
        EXPECT_EQ(stored.next_writer_epoch, expected_epoch + 1);
        EXPECT_TRUE(stored.allocation_nonce.has_value());
    }
}

TEST(CASServerRootEpoch, EveryAllocationWritesAFreshNonce)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    std::vector<UInt128> nonces;
    for (int i = 0; i < 2; ++i)
    {
        (void)allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation());
        const std::optional<UInt128> nonce
            = decodeServerEpoch(ops.op.read(l.epochKey("r"), Retry::standard())->bytes).allocation_nonce;
        ASSERT_TRUE(nonce.has_value());
        nonces.push_back(*nonce);
    }
    EXPECT_NE(nonces[0], nonces[1]);
}

/// No writer emits a zero counter; an allocation over one would hand out writer_epoch 0.
TEST(CASServerRootEpoch, ZeroCounterFailsAllocationWithoutWriting)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    mustCommit(ops.op.create(l.epochKey("r"), encodeServerEpoch(ServerEpoch{.next_writer_epoch = 0}), Retry::standard()),
               "zero counter");
    const auto before = ops.op.read(l.epochKey("r"), Retry::standard());
    ASSERT_TRUE(before.has_value());

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        (void)allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation());
    });
    EXPECT_EQ(ops.op.read(l.epochKey("r"), Retry::standard())->bytes, before->bytes);
}

/// Phase C (spec rev.4): an ABSENT epoch object over a PRESENT mount object means durable epoch
/// state was lost while a mount is live/recent — re-minting epoch 1 there is how a same-(uuid,
/// epoch) twin is born. Refuse.
TEST(CASMount, EpochRemintOverExistingMountRefuses)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/1, /*now_ms=*/1000, /*ttl_ms=*/30000).kind,
              MountClaimResult::Claimed);
    /// The epoch object is ABSENT (never created in this sequence) while the mount exists:
    EXPECT_THROW(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), DB::Exception);   /// CORRUPTED_DATA
}

TEST(CASMount, EpochRemintAuthoritativeAbsenceMints)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
    EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 1u);   /// fresh root: both control objects absent
    EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 2u);   /// epoch present now: normal conditional bump, no probe
}

/// The probe outcome gates the mint: anything short of authoritative KeyAbsent fails closed.
TEST(CASMount, EpochRemintIndeterminateProbeFailsClosed)
{
    class IndeterminateProbeBackend final : public InMemoryBackend
    {
    public:
        SentinelProbeResult probeSentinelRaw(const String &, TransportAccess &) override
        {
            return {.outcome = ProbeOutcome::Indeterminate, .body = std::nullopt};
        }
    };
    auto b = std::make_shared<IndeterminateProbeBackend>();
    Layout l("p");
    Ops ops(b);
    claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
    EXPECT_THROW(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), DB::Exception);
}

/// The steady-state path (epoch object PRESENT) must never pay the probe — pins the zero
/// normal-path cost the spec claims.
TEST(CASMount, EpochBumpWithPresentEpochIssuesNoProbe)
{
    class ProbeCountingBackend final : public InMemoryBackend
    {
    public:
        int probes = 0;
        SentinelProbeResult probeSentinelRaw(const String & k, TransportAccess & access) override
        {
            ++probes;
            return InMemoryBackend::probeSentinelRaw(k, access);
        }
    };
    auto b = std::make_shared<ProbeCountingBackend>();
    Layout l("p");
    Ops ops(b);
    claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
    EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 1u);   /// bootstrap: ONE probe (absent-epoch branch)
    const int probes_after_bootstrap = b->probes;
    EXPECT_EQ(allocateWriterEpoch(ops.op, l, "r", emptyCatalogObservation()), 2u);   /// epoch present: normal conditional bump...
    EXPECT_EQ(b->probes, probes_after_bootstrap) << "...must not probe the mount key";
}

TEST(CASServerRootClaim, MissingOwnerOverNonEmptyRootIsCorrupted)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// Simulate existing data without an owner (identity lost): plant a key under roots/<srid>/.
    mustCommit(ops.op.create(l.serverRootDataPrefix("r") + "some-data", "x", Retry::standard()), "root debris");
    try
    {
        claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation());
        FAIL() << "the claim over an ownerless non-empty subtree must be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA) << e.message();
        EXPECT_NE(e.message().find("refusing to re-claim"), String::npos) << e.message();
        EXPECT_NE(e.message().find("Configure a new <cas_server_root_id> for this disk"), String::npos) << e.message();
    }
}

TEST(CASServerRootSafety, EveryCatalogLifecycleStateBlocksOwnerAndEpochRecreation)
{
    const Layout layout("p");
    for (const NsState state : {NsState::Creating, NsState::Live, NsState::Removing})
    {
        RefCatalog catalog = catalogOwning("root/x/table", state);
        const ObserveRefCatalog observe = [catalog] { return catalog; };

        Ops owner_ops(std::make_shared<InMemoryBackend>());
        EXPECT_THROW(claimOwnerOrThrow(owner_ops.op, layout, "root/x", UInt128{1}, observe), DB::Exception);
        EXPECT_FALSE(owner_ops.op.head(layout.ownerKey("root/x"), Retry::standard()).has_value());

        Ops epoch_ops(std::make_shared<InMemoryBackend>());
        EXPECT_THROW(allocateWriterEpoch(
            epoch_ops.op, layout, "root/x", observe), DB::Exception);
        EXPECT_FALSE(epoch_ops.op.head(layout.epochKey("root/x"), Retry::standard()).has_value());
    }
}

TEST(CASServerRootSafety, OwnershipUsesAPathComponentBoundary)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    const Layout layout("p");
    EXPECT_TRUE(serverRootSubtreeEmpty(
        ops.op, layout, "root/x", catalogOwning("root/xy/table", NsState::Live)));
    EXPECT_FALSE(serverRootSubtreeEmpty(
        ops.op, layout, "root/x", catalogOwning("root/x/table", NsState::Live)));
}

TEST(CASServerRootSafety, OpaqueStreamAndStateDebrisAloneDoesNotBlockRecreation)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    const Layout layout("p");
    const NamespaceLifeId dead = NamespaceLifeId::fromCatalogEntry(RootNamespace{"unowned"}, UInt128{99});
    mustCommit(ops.op.create(layout.refLogKey(dead, RefTxnId{1, 1}), "debris", Retry::standard()), "ref-log debris");
    mustCommit(ops.op.create(layout.refCkptKey(dead), "debris", Retry::standard()), "ckpt debris");
    mustCommit(ops.op.create(layout.namespaceFileKey(dead, "f"), "debris", Retry::standard()), "ns-file debris");

    EXPECT_NO_THROW(claimOwnerOrThrow(ops.op, layout, "root/x", UInt128{1}, emptyCatalogObservation()));
    EXPECT_EQ(allocateWriterEpoch(
        ops.op, layout, "root/x", emptyCatalogObservation()), 1u);
}

TEST(CASServerRootSafety, ManifestAndLooseRootDebrisStillBlockRecreation)
{
    const Layout layout("p");
    for (const String & key : {
             layout.casManifestsServerPrefix("root/x") + "table/debris",
             layout.serverRootDataPrefix("root/x") + "loose"})
    {
        Ops ops(std::make_shared<InMemoryBackend>());
        mustCommit(ops.op.create(key, "x", Retry::standard()), "blocking debris");
        EXPECT_THROW(claimOwnerOrThrow(
            ops.op, layout, "root/x", UInt128{1}, emptyCatalogObservation()), DB::Exception);
        EXPECT_THROW(allocateWriterEpoch(
            ops.op, layout, "root/x", emptyCatalogObservation()), DB::Exception);
    }
}

TEST(CASServerRootSafety, UnreadableCatalogNeverFallsBackToPhysicalGuesses)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    const Layout layout("p");
    const ObserveRefCatalog unreadable = []() -> RefCatalog
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected unreadable catalog");
    };
    EXPECT_THROW(claimOwnerOrThrow(ops.op, layout, "root/x", UInt128{1}, unreadable), DB::Exception);
    EXPECT_THROW(allocateWriterEpoch(
        ops.op, layout, "root/x", unreadable), DB::Exception);
    EXPECT_FALSE(ops.op.head(layout.ownerKey("root/x"), Retry::standard()).has_value());
    EXPECT_FALSE(ops.op.head(layout.epochKey("root/x"), Retry::standard()).has_value());
}

TEST(CASServerRootSafety, OwnerConflictRecomputesTheWholeEmptinessBundle)
{
    auto backend = std::make_shared<OwnerConflictRevealsManifestBackend>();
    Ops ops(backend);
    const Layout layout("p");
    /// The message, not just the code: without the post-conflict recompute the claim still throws
    /// `CORRUPTED_DATA`, from the vanished-anchor arm below it, so a bare code assertion would hold
    /// with the behaviour this test is named for deleted.
    DB::Cas::tests::expectThrowsCodeWithMessage(
        DB::ErrorCodes::CORRUPTED_DATA,
        "newly visible owned work blocks recreation",
        [&] { claimOwnerOrThrow(ops.op, layout, "root/x", UInt128{1}, emptyCatalogObservation()); });
    EXPECT_TRUE(backend->fired);
    EXPECT_FALSE(ops.op.head(layout.ownerKey("root/x"), Retry::standard()).has_value());
}

TEST(CASServerRootSafety, EpochConflictRecomputesTheWholeEmptinessBundle)
{
    auto backend = std::make_shared<EpochConflictBackend>();
    Ops ops(backend);
    const Layout layout("p");
    EXPECT_THROW(allocateWriterEpoch(
        ops.op, layout, "root/x", emptyCatalogObservation()), DB::Exception);
    EXPECT_TRUE(backend->fired);
    ASSERT_TRUE(backend->winner_installed);
    const auto epoch = ops.op.read(layout.epochKey("root/x"), Retry::standard());
    ASSERT_TRUE(epoch.has_value());
    EXPECT_EQ(decodeServerEpoch(epoch->bytes).next_writer_epoch, 2u)
        << "the rejected allocator must not consume an epoch from the conflict winner";
}

TEST(CASMountLease, AbsentClaimThenRenewBumpsSeq)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;
    Ops ops(b, &boot);
    auto r = claimMount(ops.op, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    MountLeaseRenewer k(ops.mount, ops.farewell, l, "r", UInt128(1), 7, std::chrono::milliseconds(100),
                       [&] { return now; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(0),
                       [&] { return boot; });
    k.start();
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).seq, 1u);
    renewOrThrow(k);
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).seq, 2u);
}

TEST(CASMountLease, HolderBodiesMintFreshAttemptIdsAndFenceCopiesIt)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    uint64_t boot = 0;
    Ops ops(backend, &boot);
    ASSERT_EQ(claimMount(ops.op, layout, "r", UInt128{1}, 7, now, 100).kind, MountClaimResult::Claimed);
    const String key = layout.mountKey("r");
    const MountLease claimed = decodeMountLease(ops.op.read(key, Retry::standard())->bytes);

    MountLeaseRenewer renewer(ops.mount, ops.farewell, layout, "r", UInt128{1}, 7, std::chrono::milliseconds(100),
                            [&] { return now; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(0),
                            [&] { return boot; });
    renewer.start();
    renewOrThrow(renewer);
    const MountLease renewed = decodeMountLease(ops.op.read(key, Retry::standard())->bytes);
    EXPECT_NE(claimed.write_attempt_id, UInt128{});
    EXPECT_NE(renewed.write_attempt_id, UInt128{});
    EXPECT_NE(claimed.write_attempt_id, renewed.write_attempt_id);

    auto observed = ops.op.read(key, Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease fenced = decodeMountLease(observed->bytes);
    fenced.gc_fenced = true;
    ++fenced.seq;
    mustCommit(ops.op.replace(key, encodeMountLease(fenced), observed->etag, Retry::standard()), "fence-out");
    EXPECT_EQ(decodeMountLease(ops.op.read(key, Retry::standard())->bytes).write_attempt_id, renewed.write_attempt_id);
}

TEST(CASMountLease, ReclaimAndSuccessorBodiesMintNewAttemptIds)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    Ops ops(backend);
    const String key = layout.mountKey("r");
    ASSERT_EQ(claimMount(ops.op, layout, "r", UInt128{1}, 7, 1000, 100).kind, MountClaimResult::Claimed);
    const MountLease first = decodeMountLease(ops.op.read(key, Retry::standard())->bytes);

    auto observed = ops.op.read(key, Retry::standard());
    ASSERT_TRUE(observed.has_value());
    MountLease fenced = decodeMountLease(observed->bytes);
    fenced.gc_fenced = true;
    ++fenced.seq;
    mustCommit(ops.op.replace(key, encodeMountLease(fenced), observed->etag, Retry::standard()), "fence-out");
    const MountLease fence = decodeMountLease(ops.op.read(key, Retry::standard())->bytes);
    EXPECT_EQ(fence.write_attempt_id, first.write_attempt_id);

    ASSERT_EQ(claimMount(ops.op, layout, "r", UInt128{1}, 8, 2000, 100).kind, MountClaimResult::Claimed);
    const MountLease successor = decodeMountLease(ops.op.read(key, Retry::standard())->bytes);
    EXPECT_NE(successor.write_attempt_id, first.write_attempt_id);
    EXPECT_NE(successor.write_attempt_id, UInt128{});
}

/// STID 3982-3b48: `rm -rf` of the pool dir under a live mount deletes the mount slot object out from
/// under a running renewer. The next synchronous renewal must return terminal WITHOUT constructing a
/// `LOGICAL_ERROR` -- that aborts debug/ASan builds at
/// exception construction, and there is no foreign writer here to fail closed against, only an
/// environmental condition.
TEST(CASMountLease, VanishedBackingStoreStopsRenewalWithoutLogicalError)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;
    Ops ops(b, &boot);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    MountLeaseRenewer k(ops.mount, ops.farewell, l, "r", UInt128(1), 7, std::chrono::milliseconds(100),
                       [&] { return now; }, [] { return uint64_t{0}; }, {}, std::chrono::milliseconds(0),
                       [&] { return boot; });
    k.start();

    const String mount_key = l.mountKey("r");
    const auto lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();  /// NOLINT(clang-analyzer-deadcode.DeadStores)

    /// Simulate `rm -rf` of the backing store: the mount slot object is gone, but the renewer still
    /// names a (now stale) incarnation as its precondition.
    ASSERT_EQ(ops.op.removeCurrent(mount_key, Retry::standard()), Removal::Removed);

    try
    {
        renewOrThrow(k);
        FAIL() << "renew against a vanished mount object must throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::FILE_DOESNT_EXIST) << e.message();
        EXPECT_NE(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before)
        << "renewer classification is metric-free; the runtime records operational loss";
}

/// STID 3982-3b48 (part 1b): the terminal/clean-release counterpart to the renewal fix above. When
/// the backing store vanishes (`rm -rf` of the pool dir), the renewal side already stops non-fatally
/// (see the previous test); teardown then runs the terminal release (`stop()` -> `terminate()`),
/// which used to unconditionally throw `LOGICAL_ERROR` once the token-guarded farewell PUT observed
/// an absent object. The desired end state of a release ("no live lease object") is already true, so
/// this must be a no-op, never a `LOGICAL_ERROR` (which aborts debug/ASan builds).
///
/// Driven WITHOUT a prior failed renew, so the count is deterministic: this is the only place along
/// this path that increments `CASMountLeaseLost`, so we expect exactly +1 (not +2, since renewal was
/// never invoked here).
TEST(CASMountLease, TerminateAfterVanishedBackingStoreIsNoOpRelease)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    MountLeaseRenewer k(ops.mount, ops.farewell, l, "r", UInt128(1), 7, std::chrono::milliseconds(100),
                       [&] { return now; }, [] { return uint64_t{0}; });
    k.start();

    const String mount_key = l.mountKey("r");
    const auto lost_before = ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load();

    /// Simulate `rm -rf` of the backing store: the mount slot object is gone before we ever attempt
    /// a renewal, so the farewell's guarded write is the first thing to observe it.
    ASSERT_EQ(ops.op.removeCurrent(mount_key, Retry::standard()), Removal::Removed);

    EXPECT_NO_THROW(k.release())
        << "clean release against a vanished store must be a no-op, not a LOGICAL_ERROR abort";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountLeaseLost].load(), lost_before);
}

/// rev.6: a bare `claimMount` (no `proven_dead_incarnation`) NEVER reclaims a same-uuid, different-epoch
/// lease off a wall-clock-looking-expired stamp — only `claimMountAwaitingExpiry`'s observation loop
/// can turn that into a reclaim. Renamed from `...ExpiredReclaims` to describe the corrected behavior.
TEST(CASMountLease, SameUuidLiveFailsForeignFailsExpiredStillLiveDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100);    // A live until 1100
    // same uuid, lease still live → double-start guard:
    EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(1), 8, 1050, 100).kind, MountClaimResult::LiveDoubleStart);
    // foreign uuid, even after expiry → fail closed:
    EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(2), 1, 1200, 100).kind, MountClaimResult::ForeignOwner);
    // same uuid, even after the stamp LOOKS expired on our wall clock → still LiveDoubleStart: no
    // proven_dead_incarnation was supplied, so there is no certificate of death to reclaim on.
    EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(1), 9, 1200, 100).kind, MountClaimResult::LiveDoubleStart);
}

TEST(CASMountMessage, DoubleStartTextHasIdentityAndRemediation)
{
    MountLease m;
    m.server_uuid = (UInt128(0xdeadbeefcafef00dULL) << 64) | UInt128(0x0011223344556677ULL);
    m.writer_epoch = 7;
    m.hostname = "host-9.example.com";
    m.pid = 4242;
    m.seq = 13;
    m.expires_at_ms = 1700000030000ULL;

    const std::string msg = mountDoubleStartMessage("replica-a", m);

    /// Identity / existing-holder fields.
    EXPECT_NE(msg.find("<cas_server_root_id>"), std::string::npos);
    EXPECT_NE(msg.find("'replica-a'"), std::string::npos);
    EXPECT_NE(msg.find("hostname=host-9.example.com"), std::string::npos);
    EXPECT_NE(msg.find("pid=4242"), std::string::npos);
    EXPECT_NE(msg.find("last_seq=13"), std::string::npos);
    EXPECT_NE(msg.find("expires_at_ms=1700000030000"), std::string::npos);
    /// New wait-aware remediation (this server already waited; the lease kept being renewed).
    EXPECT_NE(msg.find("waited"), std::string::npos);
    EXPECT_NE(msg.find("unique"), std::string::npos);
    EXPECT_NE(msg.find("reclaim the mount on restart"), std::string::npos);
    EXPECT_NE(msg.find("uuid file"), std::string::npos);
    /// Token-stability liveness statement (replaces the old wall-clock CLOCK SKEW caveat) + manual
    /// mount-object delete escape hatch + the unsafe-knob escape hatch.
    EXPECT_NE(msg.find("own clock"), std::string::npos);
    EXPECT_NE(msg.find("diagnostic"), std::string::npos);
    EXPECT_NE(msg.find("manually delete the mount"), std::string::npos);
    EXPECT_NE(msg.find("gc/server-roots/replica-a/mount"), std::string::npos);
    EXPECT_NE(msg.find("cas_unsafe_remount_no_delay"), std::string::npos);
}

/// rev.6: a stamped `expires_at_ms` that already looks past-due on our wall clock must NOT shortcut
/// the observation wait — the old "instant, zero-sleep" reclaim this test name described was exactly
/// the cross-node wall-clock trust rev.6 removes. Renamed to describe the CORRECTED behavior: the
/// wall-clock-looking-expired stamp buys nothing, the full threshold is still observed.
TEST(CASMountAwaitExpiry, PastExpiryStillPaysTheFullObservationThreshold)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// A prior incarnation (uuid=1, epoch=7) claimed a lease live until 1100.
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t wall = 1200;                // already past 1100 on wall clock — irrelevant to the decision
    uint64_t mono = 0;
    int sleeps = 0;
    auto now_fn = [&] { return wall; };
    auto mono_fn = [&] { return mono; };
    auto sleep_fn = [&](uint64_t ms) { wall += ms; mono += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, mono_fn, /*ttl*/ 100, /*poll*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_GT(sleeps, 0);                                    // NOT instant — no wall-clock trust
    EXPECT_GE(mono, 100 + 100 / 20 + 25);                     // full observation threshold paid
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 8u);   // reclaimed as us
}

TEST(CASMountAwaitExpiry, FutureExpiryReclaimsAfterClockAdvances)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t wall = 1000;                // lease looks live until 1100, holder does NOT renew
    uint64_t mono = 0;
    auto now_fn = [&] { return wall; };
    auto mono_fn = [&] { return mono; };
    auto sleep_fn = [&](uint64_t ms) { wall += ms; mono += ms; };

    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, mono_fn, /*ttl*/ 100, /*poll*/ 50, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    const auto body = decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes);
    EXPECT_EQ(body.writer_epoch, 8u);
    EXPECT_EQ(body.seq, 2u);                                         // reclaim continues seq (prev 1 + 1)
}

/// rev.6: a genuinely live twin now times out via BOUNDED OBSERVATION RESTARTS (its every renewal
/// bumps the write-token, forcing a restart each poll), never via a wall-clock deadline.
TEST(CASMountAwaitExpiry, LiveRenewingTwinTimesOutAsDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t wall = 1000;
    uint64_t mono = 0;
    auto now_fn = [&] { return wall; };
    auto mono_fn = [&] { return mono; };
    /// Each poll: both clocks advance AND the live holder (uuid=1, epoch=7) renews its own lease —
    /// the observed incarnation changes on EVERY poll, forcing a restart every time.
    auto sleep_fn = [&](uint64_t ms)
    {
        wall += ms;
        mono += ms;
        const auto got = ops.op.read(l.mountKey("r"), Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease renewed = decodeMountLease(got->bytes);
        ++renewed.seq;
        renewed.expires_at_ms = wall + 100;
        mustCommit(ops.op.replace(l.mountKey("r"), encodeMountLease(renewed), got->etag, Retry::standard()),
                   "twin renewal");
    };

    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, mono_fn, /*ttl*/ 100, /*poll*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 7u);   // still the holder's
}

namespace
{
/// Hides the watched key from every read while the object stays underneath, so `claimMount`'s create
/// conflicts with nothing observed and the observation loop's re-read finds the slot absent every time.
class AlwaysVanishesBackend final : public InMemoryBackend
{
public:
    String watched_key;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key == watched_key)
            return std::nullopt;
        return InMemoryBackend::read(key, access);
    }
};
}

/// A slot deleted under the observation loop (a 26.6.4 decommission still deletes it) is re-claimed on the
/// same paced, bounded restart budget as a renewing holder, not spun at backend RTT.
TEST(CASMountAwaitExpiry, PersistentSlotVanishPacesAndBoundsRestartsInsteadOfSpinning)
{
    auto vanishing = std::make_shared<AlwaysVanishesBackend>();
    Layout l("p");
    {
        Ops seed(vanishing);
        ASSERT_EQ(claimMount(seed.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    }
    vanishing->watched_key = l.mountKey("r");
    Ops ops(vanishing);

    uint64_t wall = 1000;
    uint64_t mono = 0;
    int sleeps = 0;
    auto now_fn = [&] { return wall; };
    auto mono_fn = [&] { return mono; };
    auto sleep_fn = [&](uint64_t ms) { wall += ms; mono += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, mono_fn, /*ttl*/ 100, /*poll*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_GT(sleeps, 0);
    vanishing->watched_key.clear();
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 7u);
}

TEST(CASMountAwaitExpiry, ForeignUuidFailsClosedImmediately)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// A foreign server (uuid=2) holds the mount.
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(2), 1, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    int sleeps = 0;
    auto now_fn = [&] { return now; };
    auto mono_fn = [&] { return uint64_t{0}; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, mono_fn, /*ttl*/ 100, /*poll*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::ForeignOwner);
    EXPECT_EQ(sleeps, 0);                                            // never waits across UUIDs
}

namespace
{
/// Restart reclaim as this server with a 30 s lease and a 1 s poll. Returns the observer clock at the
/// reclaim and records every threshold `on_wait_start` reports.
uint64_t reclaimAsShortLeaseObserver(Ops & ops, const Layout & l, std::vector<uint64_t> & thresholds)
{
    uint64_t mono = 0;
    const auto r = claimMountAwaitingExpiry(
        ops.op, l, "r", UInt128(1), /*our_epoch*/ 8, [] { return uint64_t{1000}; }, [&] { return mono; },
        /*ttl*/ 30'000, /*poll*/ 1'000, [&](uint64_t ms) { mono += ms; },
        [&](const MountLease &, uint64_t threshold_ms) { thresholds.push_back(threshold_ms); });
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, MountPriorState::UncleanObserved);
    return mono;
}
}

TEST(CASMountAwaitExpiry, HolderDeclaringALongerLeaseIsWaitedOutInFull)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 60'000).kind, MountClaimResult::Claimed);
    std::vector<uint64_t> thresholds;
    EXPECT_EQ(reclaimAsShortLeaseObserver(ops, l, thresholds), 64'000u)
        << "reclaimed before the predecessor's declared lease ran out";
    EXPECT_EQ(thresholds, std::vector<uint64_t>{64'000});
}

/// A shorter declared lease (10 s), a missing start stamp, equal stamps, an expiry before the start.
TEST(CASMountAwaitExpiry, ShorterOrUnusableDeclaredLeaseWaitsTheObserversOwn)
{
    const std::vector<std::pair<uint64_t, uint64_t>> stamps{{1'000, 11'000}, {0, 60'000}, {5'000, 5'000}, {70'000, 10'000}};
    for (const auto & [started, expires] : stamps)
    {
        SCOPED_TRACE(fmt::format("started_at_ms={} expires_at_ms={}", started, expires));
        Ops ops(std::make_shared<InMemoryBackend>());
        Layout l("p");
        const MountLease m{.server_uuid = UInt128(1), .writer_epoch = 7, .hostname = "h", .pid = 1,
            .started_at_ms = started, .seq = 1, .expires_at_ms = expires, .write_attempt_id = UInt128(1)};
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(m), Retry::standard()), "seeded mount");
        std::vector<uint64_t> thresholds;
        EXPECT_EQ(reclaimAsShortLeaseObserver(ops, l, thresholds), 33'000u);
        EXPECT_EQ(thresholds, std::vector<uint64_t>{32'500});
    }
}

/// The certificates of death need no wait, whatever lease the body declares.
TEST(CASMountAwaitExpiry, FencedAndFarewellSlotsReclaimWithoutWaitingWhateverTheyDeclare)
{
    for (const bool fenced : {true, false})
    {
        Ops ops(std::make_shared<InMemoryBackend>());
        Layout l("p");
        MountLease m{.server_uuid = UInt128(1), .writer_epoch = 7, .hostname = "h", .pid = 1,
            .started_at_ms = 1000, .seq = 1, .expires_at_ms = 61'000, .write_attempt_id = UInt128(1)};
        m.gc_fenced = fenced;
        m.min_active_build_sequence = fenced ? 0 : std::numeric_limits<uint64_t>::max();
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(m), Retry::standard()), "seeded mount");
        int sleeps = 0;
        const auto r = claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), 8, [] { return uint64_t{1000}; },
            [] { return uint64_t{0}; }, 30'000, 1'000, [&](uint64_t) { ++sleeps; });
        EXPECT_EQ(r.kind, MountClaimResult::Claimed);
        EXPECT_EQ(r.prior, fenced ? MountPriorState::Fenced : MountPriorState::Clean);
        EXPECT_EQ(sleeps, 0);
    }
}

TEST(CASMountClaim, UnsafeAuthorizationIsTokenExact)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 30000).kind, MountClaimResult::Claimed);
    const Etag stale = ops.op.read(l.mountKey("r"), Retry::standard())->etag;

    /// `Etag` equality compares (key, value), and it is minted only through the request planes -- there
    /// is no cross-key comparison to exercise here. Build the stale token by renewing the SAME slot once:
    /// `stale`, read before this renewal, is then a genuinely stale token for the slot's CURRENT value.
    {
        const auto got = ops.op.read(l.mountKey("r"), Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease renewed = decodeMountLease(got->bytes);
        ++renewed.seq;
        mustCommit(ops.op.replace(l.mountKey("r"), encodeMountLease(renewed), got->etag, Retry::standard()),
                   "holder renewal");
    }
    const Etag current = ops.op.read(l.mountKey("r"), Retry::standard())->etag;

    /// A stale token is refused: nothing authorizes a reclaim over a slot that moved.
    const MountClaimResult refused = claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 30000, /*proven_dead=*/{},
                                                /*sink=*/{}, /*unsafe_reclaim_authorization=*/stale);
    EXPECT_EQ(refused.kind, MountClaimResult::LiveDoubleStart);

    /// A foreign uuid is refused before the authorization is consulted.
    const MountClaimResult foreign = claimMount(ops.op, l, "r", UInt128(2), 8, 1000, 30000, {}, {}, current);
    EXPECT_EQ(foreign.kind, MountClaimResult::ForeignOwner);

    /// The exact token reclaims, with the prior state and the audit reason naming the setting.
    std::vector<CasEvent> events;
    const MountClaimResult reclaimed = claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 30000, {},
                                                  [&](CasEvent e) { events.push_back(std::move(e)); }, current);
    ASSERT_EQ(reclaimed.kind, MountClaimResult::Claimed);
    EXPECT_EQ(reclaimed.prior, MountPriorState::UncleanUnsafe);
    ASSERT_FALSE(events.empty());
    EXPECT_THAT(events.back().reason, testing::HasSubstr("cas_unsafe_remount_no_delay"));
    /// Pin the fields `system.cas_log` consumers actually key on, not just the free-form reason: the
    /// unsafe reclaim shares the same event type and outcome as every other reclaim (`emitMountEvent`'s
    /// "reclaim" branch argument), so nothing about this path is a separate, unaudited channel.
    EXPECT_EQ(events.back().type, CasEventType::MountClaim);
    EXPECT_EQ(events.back().outcome, "reclaim");
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 8u);
}

TEST(CASMountLease, RenewerStartAdoptsOurOwnClaimNotDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    Ops ops(b);
    // The normal flow: claimMount writes the live mount under (uuid=1, epoch=7), THEN renewer.start().
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    MountLeaseRenewer k(ops.mount, ops.farewell, l, "r", UInt128(1), /*epoch*/ 7, std::chrono::milliseconds(100),
                       [&] { return now; }, [] { return uint64_t{0}; });
    EXPECT_NO_THROW(k.start());     // adopts our own live (uuid=1,epoch=7) mount — NOT a double-start
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 7u);
}

TEST(CASMountFence, SupersededWriterRefusedNoS3Read)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "r"});

    /// Permissive default: a Pool that has NOT armed the fence allows mutations.
    EXPECT_TRUE(store->mayMutate());

    /// Latching loss: once the renewer trips the fence it stays lost (purely local — no S3 read).
    store->tripMountLost();
    EXPECT_FALSE(store->mayMutate());

    /// A real mutate entrypoint that funnels through mutateShard now fails closed at the gate, BEFORE
    /// the mutate lambda runs (so this is the ABORTED gate throw, not a FILE_DOESNT_EXIST from inside).
    const RootNamespace ns{"srv1/tbl"};
    EXPECT_THROW(store->dropRef(ns, "any_ref"), DB::Exception);
}

TEST(CASMountStartup, SecondServerSameRootFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});
    /// A second server (different uuid) on the SAME server_root_id + same backend → fail closed
    /// (the owner gate rejects the foreign uuid before any mount/epoch mutation).
    EXPECT_THROW(
        Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "r"}),
        DB::Exception);
}

TEST(CASMountStartup, WriterEpochStrictlyIncreasesAcrossReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});
    const uint64_t e1 = s1->writerEpoch();

    /// Simulate shutdown: the Pool dtor stops the renewer, whose terminate() retires the lease
    /// (stamps it already-expired). The owner + the durable epoch object stay sticky.
    s1.reset();

    /// Same server reopen → reclaims the (now-expired, different-epoch) mount and allocates a strictly
    /// higher durable writer_epoch.
    auto s2 = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});
    const uint64_t e2 = s2->writerEpoch();
    EXPECT_GT(e2, e1);
}

TEST(CASMountStartup, DelayedStartOverANewerIncarnationFailsWithoutWaiting)
{
    auto b = std::make_shared<NewerStartBackend>();
    const Layout l("p");
    b->epoch_key = l.epochKey("r");
    b->mount_key = l.mountKey("r");
    {
        /// Bootstrap: owner, counter and a clean farewell of epoch 1.
        auto first = Pool::open(b, PoolConfig{.pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});
    }
    b->arm_on_next_epoch_write = true;

    /// Shared, heap-owned: the Pool may outlive this frame through a background publish.
    auto fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    auto waits = std::make_shared<std::atomic<uint64_t>>(0);
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::ABORTED, "newer than the writer_epoch", [&]
    {
        (void)Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
            .boot_ms_fn = [fake_boot] { return fake_boot->load(); },
            .wait_sleep_fn = [fake_boot, waits](uint64_t ms)
            {
                *fake_boot += ms;
                ++(*waits);
            }});
    });
    ASSERT_TRUE(b->newer_epoch.has_value()) << "the newer start was never installed";
    EXPECT_EQ(waits->load(), 0u) << "a newer incarnation is refused, not observed";
    Ops ops(b);
    const MountLease slot = decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes);
    EXPECT_EQ(slot.writer_epoch, *b->newer_epoch);
    EXPECT_EQ(slot.write_attempt_id, UInt128{0xB});
}

TEST(CASMountStartup, FreshWritablePoolBootstrapsAnExplicitEmptyCatalog)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
        .skip_access_check = true});

    Ops ops(backend);
    const auto catalog = ops.op.read(layout.refCatalogKey(), Retry::standard());
    ASSERT_TRUE(catalog.has_value());
    EXPECT_TRUE(decodeRefCatalog(catalog->bytes).entries.empty());
}

TEST(CASMountStartup, ExistingPoolWithoutCatalogFailsBeforeSlotMutation)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    {
        auto store = Pool::open(backend, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
            .skip_access_check = true});
    }

    Ops ops(backend);
    /// Old raw fixtures did not persist an empty catalog. Make this an explicit existing-pool
    /// fixture before removing the mandatory object whose loss the mount must reject.
    if (!ops.op.head(layout.refCatalogKey(), Retry::standard()))
        mustCommit(ops.op.create(layout.refCatalogKey(), encodeRefCatalog(RefCatalog{}), Retry::standard()),
                   "empty catalog");
    ASSERT_EQ(ops.op.removeCurrent(layout.refCatalogKey(), Retry::standard()), Removal::Removed);

    const auto owner_before = ops.op.read(layout.ownerKey("r"), Retry::standard());
    const auto epoch_before = ops.op.read(layout.epochKey("r"), Retry::standard());
    const auto mount_before = ops.op.read(layout.mountKey("r"), Retry::standard());
    ASSERT_TRUE(owner_before.has_value());
    ASSERT_TRUE(epoch_before.has_value());
    ASSERT_TRUE(mount_before.has_value());

    EXPECT_THROW(Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
        .skip_access_check = true}), DB::Exception);

    const auto owner_after = ops.op.read(layout.ownerKey("r"), Retry::standard());
    const auto epoch_after = ops.op.read(layout.epochKey("r"), Retry::standard());
    const auto mount_after = ops.op.read(layout.mountKey("r"), Retry::standard());
    ASSERT_TRUE(owner_after.has_value());
    ASSERT_TRUE(epoch_after.has_value());
    ASSERT_TRUE(mount_after.has_value());
    EXPECT_EQ(owner_after->bytes, owner_before->bytes);
    EXPECT_EQ(owner_after->etag, owner_before->etag);
    EXPECT_EQ(epoch_after->bytes, epoch_before->bytes);
    EXPECT_EQ(epoch_after->etag, epoch_before->etag);
    EXPECT_EQ(mount_after->bytes, mount_before->bytes);
    EXPECT_EQ(mount_after->etag, mount_before->etag);
}

TEST(CASMountReadOnly, ForeignOwnedPoolOpensWithoutMutation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    /// Server A claims the pool (writable): owner = uuid(1), a durable epoch + a live mount lease.
    auto a = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"});

    Ops ops(b);
    /// Capture the control objects BEFORE the read-only open so we can prove it mutated nothing.
    const auto owner_before = ops.op.read(l.ownerKey("r"), Retry::standard());
    const auto mount_before = ops.op.read(l.mountKey("r"), Retry::standard());
    const auto epoch_before = ops.op.read(l.epochKey("r"), Retry::standard());
    ASSERT_TRUE(owner_before.has_value());
    ASSERT_TRUE(mount_before.has_value());
    ASSERT_TRUE(epoch_before.has_value());

    /// A READ-ONLY observer with a DIFFERENT server_id on the SAME backend/server_root_id must NOT
    /// throw — a read-only mount never participates in the owner/epoch/mount protocol, so a pool
    /// owned by another server_uuid is freely observable.
    PoolPtr ro;
    EXPECT_NO_THROW(
        ro = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "r",
            .read_only = true}));
    EXPECT_NE(ro, nullptr);

    /// And it mutated nothing: owner still decodes to A's uuid, the mount body is still A's, and the
    /// raw bytes of owner/epoch/mount are byte-for-byte unchanged (no second owner, no re-claim).
    const auto owner_after = ops.op.read(l.ownerKey("r"), Retry::standard());
    const auto mount_after = ops.op.read(l.mountKey("r"), Retry::standard());
    const auto epoch_after = ops.op.read(l.epochKey("r"), Retry::standard());
    ASSERT_TRUE(owner_after.has_value());
    ASSERT_TRUE(mount_after.has_value());
    ASSERT_TRUE(epoch_after.has_value());

    EXPECT_EQ(decodeOwner(owner_after->bytes).server_uuid, UInt128(1));
    EXPECT_EQ(decodeMountLease(mount_after->bytes).server_uuid, UInt128(1));

    EXPECT_EQ(owner_after->bytes, owner_before->bytes);
    EXPECT_EQ(mount_after->bytes, mount_before->bytes);
    EXPECT_EQ(epoch_after->bytes, epoch_before->bytes);
}

/// `validateCasRequestBudget` itself, isolated from `Pool::open`: a consistent default budget is
/// accepted silently, and the overflow-safe comparison (subtraction against the TTL rather than
/// computing `attempt_timeout_ms + lease_safety_margin_ms` directly) really does reject an absurd
/// near-`UINT64_MAX` config rather than letting the sum wrap to a spuriously small value that would
/// pass the inequality when it should fail closed.
TEST(CASRequestBudget, ValidateAcceptsDefaultsAndRejectsAnOverflowingSumWithoutWrapping)
{
    EXPECT_NO_THROW(validateCasRequestBudget(
        CasRequestBudget{}, /*mount_lease_ttl_ms=*/30000, /*mount_renew_period_ms=*/10000, /*background_renewal=*/false));

    const CasRequestBudget overflowing{
        .attempt_timeout_ms = std::numeric_limits<uint64_t>::max() - 100,
        .lease_safety_margin_ms = std::numeric_limits<uint64_t>::max() - 100};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        validateCasRequestBudget(overflowing, /*mount_lease_ttl_ms=*/30000, /*mount_renew_period_ms=*/10000, /*background_renewal=*/false);
    });
}

/// Pool::open must call validateCasRequestBudget itself (not just the free function in isolation,
/// pinned directly above): an inconsistent cas_request_budget must refuse a writable mount end-to-end
/// (RFC cas-s3-timeout-retry-control §required-timeout-model), never mount silently with a budget that
/// could let a controlled attempt outlive the lease it is fenced under.
TEST(CASMountStartup, RefusesWritableOpenWithInconsistentCasRequestBudget)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// attempt_timeout_ms + lease_safety_margin_ms == mount_lease_ttl_ms below (30000): not STRICTLY
    /// less, so this must be rejected.
    const CasRequestBudget bad_budget{
        .attempt_timeout_ms = 25000, .lease_safety_margin_ms = 5000};
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
            .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
            .cas_request_budget = bad_budget});
    });
}

TEST(CASMountStartup, StaleSelfMountReclaimedAfterWait)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Server A opens writable with a SHORT lease TTL and no background renewer (`background_watermark`
    /// defaults false). The test captures its live mount body, destroys the real Pool cleanly, then
    /// replays that body to simulate a crashed process whose lease survives but is never renewed.
    /// This test's short lease TTL is far below the CasRequestBudget defaults (RFC
    /// cas-s3-timeout-retry-control §required-timeout-model requires attempt_timeout + safety_margin <
    /// lease TTL), so it also scales down cas_request_budget to fit — the budget itself is not
    /// exercised here, only Pool::open's validateCasRequestBudget startup gate.
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .lease_safety_margin_ms = 50, .connect_timeout_cap_ms = std::nullopt};
    auto a = Pool::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = tiny_budget});
    ASSERT_NE(a, nullptr);
    const uint64_t e1 = a->writerEpoch();
    const String mount_key = a->layout().mountKey("r");
    Ops ops(b);
    const auto stale_mount = ops.op.read(mount_key, Retry::standard());
    ASSERT_TRUE(stale_mount.has_value());

    /// Preserve A's live lease as if its process disappeared without running C++ teardown. Destroying
    /// the real Pool first keeps the parent process valid; replaying the saved body recreates the exact
    /// durable stale-lease state that a crashed process would leave behind.
    a.reset();
    const auto farewell = ops.op.read(mount_key, Retry::standard());
    ASSERT_TRUE(farewell.has_value());
    mustCommit(ops.op.replace(mount_key, stale_mount->bytes, farewell->etag, Retry::standard()),
               "replayed stale lease");

    /// A restart of the SAME server (same uuid) must NOT abort: it waits out the stale lease (<= ~300ms)
    /// and reclaims the mount, coming up with a strictly higher durable writer_epoch. The replayed live
    /// body hides A's clean farewell, so the reclaim is `MountPriorState::UncleanObserved`. Inject a
    /// fake `boot_ms_fn` + `wait_sleep_fn` (mirroring
    /// `CASMountOpenWaits.UncleanOpenPaysOnlyTheObservationWindow`) so the observation window resolves
    /// instantly instead of blocking this test on real time.
    /// Held in a shared atomic, not a plain local: `wait_sleep_fn` below mutates it, and the Pool can
    /// outlive this stack frame (a background publish holds `shared_from_this()`), so a by-reference
    /// capture of a local would dangle.
    auto a2_fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    PoolPtr a2;
    EXPECT_NO_THROW(
        a2 = Pool::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
            .mount_lease_ttl_ms = std::chrono::milliseconds(300),
            .mount_renew_period = std::chrono::milliseconds(100),
            .cas_request_budget = tiny_budget,
            .boot_ms_fn = [a2_fake_boot]
            {
                return a2_fake_boot->load();
            },
            .wait_sleep_fn = [a2_fake_boot](uint64_t ms)
            {
                *a2_fake_boot += ms;
            }}));
    ASSERT_NE(a2, nullptr);
    EXPECT_GT(a2->writerEpoch(), e1);

    /// The original live-object overlap: a first Pool is still alive when a replacement reclaims its
    /// slot, so the first one's release meets a stranger. This was an `EXPECT_DEATH` pinning a
    /// `LOGICAL_ERROR` abort — which fires from `~Pool`, defeating `finishTeardown`'s own catch by
    /// aborting at exception construction. The first Pool never observed a deposition (nothing failed
    /// its renewal; the slot was reclaimed underneath it), so this is the exclusivity-violation arm:
    /// refuse, leave the reclaimer's slot untouched, latch the fence, and SURVIVE.
    auto overlap_backend = std::make_shared<InMemoryBackend>();
    auto first = Pool::open(overlap_backend, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = tiny_budget});
    const String overlap_mount_key = first->layout().mountKey("r");

    /// Held in a shared atomic, not a plain local: `wait_sleep_fn` below mutates it, and the Pool can
    /// outlive this stack frame (a background publish holds `shared_from_this()`), so a by-reference
    /// capture of a local would dangle.
    auto overlap_fake_boot = std::make_shared<std::atomic<uint64_t>>(0);
    auto replacement = Pool::open(overlap_backend, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r",
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = tiny_budget,
        .boot_ms_fn = [overlap_fake_boot]
        {
            return overlap_fake_boot->load();
        },
        .wait_sleep_fn = [overlap_fake_boot](uint64_t ms)
        {
            *overlap_fake_boot += ms;
        }});
    ASSERT_NE(replacement, nullptr);

    Ops overlap_ops(overlap_backend);
    const auto reclaimer_slot_before = overlap_ops.op.read(overlap_mount_key, Retry::standard());
    ASSERT_TRUE(reclaimer_slot_before.has_value());
    const uint64_t overlap_violations_before
        = ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load();

    first.reset();   /// must not abort, must not terminate

    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASMountExclusivityViolation].load(),
              overlap_violations_before + 1);
    const auto reclaimer_slot_after = overlap_ops.op.read(overlap_mount_key, Retry::standard());
    ASSERT_TRUE(reclaimer_slot_after.has_value());
    EXPECT_EQ(reclaimer_slot_after->bytes, reclaimer_slot_before->bytes)
        << "the deposed Pool's release must not retire the reclaimer's lease";
    EXPECT_TRUE(replacement->mayMutate()) << "and must not disturb the live reclaimer";
}

TEST(CASMountLease, BodyCarriesFloorAndFence)
{
    MountLease m;
    m.server_uuid = UInt128(0xAB);
    m.writer_epoch = 7;
    m.hostname = "h";
    m.pid = 42;
    m.started_at_ms = 1000;
    m.seq = 3;
    m.expires_at_ms = 2000;
    m.min_active_build_sequence = 5;
    m.gc_fenced = true;
    m.write_attempt_id = UInt128{1};
    const MountLease d = decodeMountLease(encodeMountLease(m));
    EXPECT_EQ(d.min_active_build_sequence, 5u);
    EXPECT_TRUE(d.gc_fenced);
    EXPECT_EQ(d.writer_epoch, 7u);
}

TEST(CASMountLease, RetiredSentinelRoundTrips)
{
    MountLease m;
    m.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    m.write_attempt_id = UInt128{1};
    EXPECT_EQ(decodeMountLease(encodeMountLease(m)).min_active_build_sequence,
              std::numeric_limits<uint64_t>::max());
}

/// ---- Task 7 / Task 9: GC heartbeat classification with token-guarded, observation-based fence-out ----

namespace
{
/// A fixed, fake "now" — no real clocks in these tests. Lease timestamps are chosen relative to it.
/// Rev.6 §token-stability observation removed the wall clock from the fence DECISION; `kNowMs` below
/// is threaded through only as `computeHeartbeatFloor`'s audit-only `now_ms`.
constexpr uint64_t kNowMs = 1'000'000;
/// The observing leader's own lease and step, and the threshold they give a body that declares nothing.
constexpr uint64_t kObserverLeaseMs = 30'000;
constexpr uint64_t kCadenceMs = 1'000;
constexpr uint64_t kStableThresholdMs = 32'500;
constexpr std::string_view kLeaseMismatchMarker = "declares a lease";

/// Seed one mount body under mountKey(srid) via the on-storage codec — the same interface the renewer
/// writes through.
MountLease seedMount(
    CasOperation & op, const Layout & l, const String & srid,
    uint64_t expires_at_ms, bool gc_fenced, uint64_t min_active_build_sequence, uint64_t seq = 1,
    uint64_t started_at_ms = kNowMs)
{
    MountLease m;
    m.server_uuid = UInt128(srid.back());   // distinct per srid; content is irrelevant to the gate
    m.writer_epoch = 1;
    m.hostname = "h-" + srid;
    m.pid = 100;
    m.started_at_ms = started_at_ms;
    m.seq = seq;
    m.expires_at_ms = expires_at_ms;
    m.min_active_build_sequence = min_active_build_sequence;
    m.gc_fenced = gc_fenced;
    m.write_attempt_id = UInt128{1};
    mustCommit(op.create(l.mountKey(srid), encodeMountLease(m), Retry::standard()), "seeded mount " + srid);
    return m;
}

/// Simulate a renewer's real renewal between two `computeHeartbeatFloor` calls: a guarded write that
/// bumps `seq` (and so mints a fresh incarnation), leaving everything else as-is. Models the one
/// thing the observation-based fence cares about: the incarnation changed, so any in-progress
/// observation of the OLD one must restart.
void renewMount(CasOperation & op, const Layout & l, const String & srid)
{
    const auto got = op.read(l.mountKey(srid), Retry::standard());
    ASSERT_TRUE(got.has_value());
    MountLease m = decodeMountLease(got->bytes);
    m.seq += 1;
    mustCommit(op.replace(l.mountKey(srid), encodeMountLease(m), got->etag, Retry::standard()),
               "renewed mount " + srid);
}

/// An observer clock that does not move during a call: the entry sample and every sighting read the
/// same value.
std::function<uint64_t()> frozenClock(uint64_t ms)
{
    return [ms] { return ms; };
}
}

TEST(CASHeartbeatFloor, FirstSightNeverFencesEvenIfStampLooksExpired)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    Ops ops(b);
    /// A stamp that would have read as long-expired under the old skew-margin comparison — under
    /// rev.6 observation the stamp is never even consulted for the fence decision.
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, /*now_ms*/ kNowMs, frozenClock(0),
                                                         kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(floor.live, 1u);
    ASSERT_TRUE(obs.contains("s1"));
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, 0u);
}

TEST(CASHeartbeatFloor, StableIncarnationPastThresholdIsFenced)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor_before = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(floor_before.fenced_now, 0u);

    const MountLease before = decodeMountLease(ops.op.read(l.mountKey("s1"), Retry::standard())->bytes);

    /// No renewal in between: the SAME incarnation, observed since mono 0, is now stable for the full
    /// threshold on the leader's own clock.
    const HeartbeatFloor floor2 = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs),
                                                          kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor2.fenced_now, 1u);
    EXPECT_EQ(floor2.fenced_srids, std::vector<String>{"s1"});
    const MountLease fenced = decodeMountLease(ops.op.read(l.mountKey("s1"), Retry::standard())->bytes);
    EXPECT_TRUE(fenced.gc_fenced);
    EXPECT_EQ(fenced.seq, before.seq + 1);
}

TEST(CASHeartbeatFloor, RenewalBetweenRoundsRestartsObservation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_TRUE(obs.contains("s1"));
    const Etag first_etag = obs.at("s1").etag;

    renewMount(ops.op, l, "s1");
    const Etag renewed_etag = currentEtag(ops.op, l.mountKey("s1"));
    EXPECT_NE(renewed_etag, first_etag);

    const HeartbeatFloor floor2 = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs),
                                                          kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor2.fenced_now, 0u);
    ASSERT_TRUE(obs.contains("s1"));
    EXPECT_EQ(obs.at("s1").etag, renewed_etag);
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, kStableThresholdMs);
}

/// A slot that turns terminal leaves the observation map in the walk that sees it.
TEST(CASHeartbeatFloor, TerminalSlotLeavesTheObservationMap)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_TRUE(obs.contains("s1"));
    ASSERT_TRUE(obs.contains("s2"));

    renewMount(ops.op, l, "s1");
    const auto got = ops.op.read(l.mountKey("s2"), Retry::standard());
    MountLease farewell = decodeMountLease(got->bytes);
    farewell.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    mustCommit(ops.op.replace(l.mountKey("s2"), encodeMountLease(farewell), got->etag, Retry::standard()), "s2 farewell");

    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs), kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(floor.terminated, 1u);
    EXPECT_TRUE(obs.contains("s1"));
    EXPECT_FALSE(obs.contains("s2"));
}

/// A slot deleted by a 26.6.4 decommission or by hand is never listed again; its observation is dropped.
TEST(CASHeartbeatFloor, UnseenSridPrunedFromObservationMap)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_TRUE(obs.contains("s2"));

    /// s1 renews so it stays observed for its own reason; only s2's absence from the listing is under test.
    renewMount(ops.op, l, "s1");
    ASSERT_EQ(ops.op.removeCurrent(l.mountKey("s2"), Retry::standard()), Removal::Removed);
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs), kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_TRUE(obs.contains("s1"));
    EXPECT_FALSE(obs.contains("s2"));
}

TEST(CASHeartbeatFloor, ClassifiesAndFencesOut)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    /// two live mounts — genuinely renewing between the two rounds below, so their observation never
    /// stabilizes.
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    /// dead — no renewal between the two rounds below — must be fenced-out by the second call.
    seedMount(ops.op, l, "s3", /*expires*/ kNowMs - 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    /// already-fenced — excluded, body byte-identical after both calls (no write).
    seedMount(ops.op, l, "s4", /*expires*/ kNowMs - 60'000, /*fenced*/ true, /*min_active_build_sequence*/ 0);
    /// terminated (min_active_build_sequence == UINT64_MAX) with expired-looking timestamps — excluded, not fenced.
    seedMount(ops.op, l, "s5", /*expires*/ kNowMs - 60'000, /*fenced*/ false,
              /*min_active_build_sequence*/ std::numeric_limits<uint64_t>::max());

    MountObservationMap obs;
    std::set<String> warned;

    /// Round 1 (mono 0): first sight of every non-terminal mount — nothing is fence-eligible yet.
    const HeartbeatFloor floor_before = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(floor_before.live, 3u);            // s1, s2, s3: observation just started
    EXPECT_EQ(floor_before.terminated, 1u);      // s5
    EXPECT_EQ(floor_before.fenced_now, 0u);
    EXPECT_EQ(floor_before.already_fenced, 1u);  // s4

    /// s1 and s2 renew between rounds (as a live renewer would); s3 does not (it crashed).
    renewMount(ops.op, l, "s1");
    renewMount(ops.op, l, "s2");

    const auto s3_before = ops.op.read(l.mountKey("s3"), Retry::standard());
    const auto s4_before = ops.op.read(l.mountKey("s4"), Retry::standard());
    ASSERT_TRUE(s3_before.has_value());
    ASSERT_TRUE(s4_before.has_value());

    /// Round 2 (mono == threshold): s1/s2's renewed incarnations restart their observation (still
    /// live); s3's original incarnation has now held stable for the full threshold -> fenced.
    const HeartbeatFloor floor2 = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs),
                                                          kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor2.live, 2u);            // s1, s2: renewed, observation restarted
    EXPECT_EQ(floor2.terminated, 1u);      // s5
    EXPECT_EQ(floor2.fenced_now, 1u);      // s3
    EXPECT_EQ(floor2.already_fenced, 1u);  // s4

    /// The dead body was fenced: gc_fenced set, seq bumped, the rest of the body preserved.
    const auto s3_after = ops.op.read(l.mountKey("s3"), Retry::standard());
    ASSERT_TRUE(s3_after.has_value());
    const MountLease s3_prev = decodeMountLease(s3_before->bytes);
    const MountLease s3_now = decodeMountLease(s3_after->bytes);
    EXPECT_TRUE(s3_now.gc_fenced);
    EXPECT_EQ(s3_now.seq, s3_prev.seq + 1);
    EXPECT_EQ(s3_now.server_uuid, s3_prev.server_uuid);
    EXPECT_EQ(s3_now.writer_epoch, s3_prev.writer_epoch);
    EXPECT_EQ(s3_now.hostname, s3_prev.hostname);
    EXPECT_EQ(s3_now.expires_at_ms, s3_prev.expires_at_ms);

    /// The already-fenced body was not touched (no write) across either call.
    const auto s4_after = ops.op.read(l.mountKey("s4"), Retry::standard());
    ASSERT_TRUE(s4_after.has_value());
    EXPECT_EQ(s4_after->bytes, s4_before->bytes);
}

namespace
{
/// A delegating backend whose guarded write of the target mount key first performs an inner renewal
/// (a real, correctly-guarded write that pushes expiry far into the future) and THEN delegates — so
/// the caller's fence-out write lands on a stale precondition and is refused. The inner renewal runs
/// exactly once (`renewed`), modelling a holder that renews concurrently in the window between the
/// function's read and its fence-out write.
class RenewOnFenceBackend : public InMemoryBackend
{
public:
    RenewOnFenceBackend(String target_key_, uint64_t renewed_expires_ms_)
        : target_key(std::move(target_key_)), renewed_expires_ms(renewed_expires_ms_)
    {
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (expected_value && key == target_key && !renewed)
        {
            renewed = true;
            /// The holder renews under the real current incarnation: fresh far-future expiry.
            const auto got = InMemoryBackend::read(key, access);
            MountLease m = decodeMountLease(got->bytes);
            m.seq += 1;
            m.expires_at_ms = renewed_expires_ms;
            const auto renew = InMemoryBackend::write(key, encodeMountLease(m), got->value, access);
            EXPECT_TRUE(renew.has_value());
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

private:
    String target_key;
    uint64_t renewed_expires_ms;
    bool renewed = false;
};
}

TEST(CASHeartbeatFloor, FenceOutLosesTheIncarnationRaceAndReclassifiesLive)
{
    Layout l("p");
    auto b = std::make_shared<RenewOnFenceBackend>(
        l.mountKey("s1"), /*renewed_expires*/ kNowMs + 120'000);
    Ops ops(b);

    seedMount(ops.op, l, "s1", /*expires*/ kNowMs - 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    MountObservationMap obs;
    std::set<String> warned;
    /// Round 1: first sight, observation starts — never reaches the fence-out path (the race
    /// decorator stays armed for round 2).
    const HeartbeatFloor floor_before = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(floor_before.fenced_now, 0u);

    /// Round 2: the incarnation has been stable past threshold, so the function attempts the
    /// fence-out. The decorator renews concurrently under the real incarnation, the write is refused,
    /// and the re-decision reclassifies the slot as live (observation restarted on the new
    /// incarnation) — never fenced.
    const HeartbeatFloor floor2 = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs),
                                                          kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor2.fenced_now, 0u);
    EXPECT_EQ(floor2.live, 1u);

    const auto after = ops.op.read(l.mountKey("s1"), Retry::standard());
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(decodeMountLease(after->bytes).gc_fenced);
}

namespace
{
/// Moves the observer clock by `read_cost_ms` on every read of a mount slot, so a call spends that
/// long between its entry sample and the read it decides on. `renew_before_next_fence` makes the
/// holder renew just before the next guarded write of a slot lands, so the fence-out is refused and
/// decided again on the new token. `fail_slot_reads` makes every slot read time out.
class SlotClockBackend : public InMemoryBackend
{
public:
    explicit SlotClockBackend(uint64_t & mono_) : mono(mono_) {}

    uint64_t read_cost_ms = 0;
    bool renew_before_next_fence = false;
    bool fail_slot_reads = false;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key.ends_with("/mount"))
        {
            if (fail_slot_reads)
                throw Poco::TimeoutException("injected: mount slot read timed out");
            mono += read_cost_ms;
        }
        return InMemoryBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (renew_before_next_fence && expected_value && key.ends_with("/mount"))
        {
            renew_before_next_fence = false;
            const auto got = InMemoryBackend::read(key, access);
            MountLease m = decodeMountLease(got->bytes);
            m.seq += 1;
            EXPECT_TRUE(InMemoryBackend::write(key, encodeMountLease(m), got->value, access).has_value());
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

private:
    uint64_t & mono;
};
}

TEST(CASHeartbeatFloor, ConfirmingCallDecidesOnItsEntrySample)
{
    uint64_t mono = 0;
    auto b = std::make_shared<SlotClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;
    std::set<String> warned;

    computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_EQ(obs.at("s1").first_seen_mono_ms, 0u);

    /// The call starts one millisecond short of the threshold; its walk passes the threshold many times.
    mono = kStableThresholdMs - 1;
    b->read_cost_ms = 10 * kStableThresholdMs;
    const HeartbeatFloor slow = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(slow.fenced_now, 0u);
    EXPECT_EQ(slow.live, 1u);
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, 0u) << "an unchanged token keeps its first sighting";

    b->read_cost_ms = 0;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 1u);
}

TEST(CASHeartbeatFloor, FailedSlotReadKeepsTheWatch)
{
    uint64_t mono = 0;
    auto b = std::make_shared<SlotClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;
    std::set<String> warned;

    computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_EQ(obs.at("s1").first_seen_mono_ms, 0u);

    mono = kStableThresholdMs / 2;
    b->fail_slot_reads = true;
    EXPECT_ANY_THROW(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned));
    ASSERT_TRUE(obs.contains("s1"));
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, 0u);

    b->fail_slot_reads = false;
    mono = kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 1u)
        << "a failed read restarted the observation";
}

TEST(CASHeartbeatFloor, SampleOlderThanTheSightingNeverFences)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    MountObservationMap obs;
    std::set<String> warned;

    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs), kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_EQ(obs.at("s1").first_seen_mono_ms, kStableThresholdMs);

    /// A clock that went back: the unsigned difference would wrap past any threshold.
    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(kStableThresholdMs - 1),
                                                       kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(floor.live, 1u);
}

namespace
{
constexpr uint64_t kWalkMs = 4'000;
}

TEST(CASHeartbeatFloor, FirstSightingCountsFromAfterItsRead)
{
    uint64_t mono = 0;
    auto b = std::make_shared<SlotClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;
    std::set<String> warned;

    /// The call starts at 0 and reaches the slot at `kWalkMs`.
    b->read_cost_ms = kWalkMs;
    ASSERT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 0u);
    ASSERT_TRUE(obs.contains("s1"));
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, kWalkMs);

    /// One threshold after the first call started, less than one after its read.
    b->read_cost_ms = 0;
    mono = kStableThresholdMs;
    const HeartbeatFloor early = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    EXPECT_EQ(early.fenced_now, 0u) << "fenced a token watched for less than the threshold";
    EXPECT_EQ(early.live, 1u);

    mono = kWalkMs + kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 1u);
}

TEST(CASHeartbeatFloor, RedecisionAfterARefusedFenceCountsFromItsRead)
{
    uint64_t mono = 0;
    auto b = std::make_shared<SlotClockBackend>(mono);
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ 10, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const auto clock = [&mono] { return mono; };
    MountObservationMap obs;
    std::set<String> warned;

    computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_TRUE(obs.contains("s1"));
    const Etag first = obs.at("s1").etag;

    /// Stable at this call's start, so it tries the fence-out. The holder renews first, the write is
    /// refused, and the call decides again on the new token after reading it.
    mono = kStableThresholdMs;
    b->read_cost_ms = kWalkMs;
    b->renew_before_next_fence = true;
    const HeartbeatFloor refused = computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_FALSE(b->renew_before_next_fence) << "the call never attempted the fence-out";
    ASSERT_EQ(refused.fenced_now, 0u);
    ASSERT_EQ(refused.live, 1u);
    ASSERT_NE(obs.at("s1").etag, first);

    /// Nothing reads after the re-decision, so the clock holds the value of the last read.
    const uint64_t reread_at = mono;
    ASSERT_GT(reread_at, kStableThresholdMs);
    EXPECT_EQ(obs.at("s1").first_seen_mono_ms, reread_at);

    /// One threshold after that call started, less than one after its re-read.
    b->read_cost_ms = 0;
    mono = 2 * kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 0u)
        << "fenced the renewed token before it was watched for the threshold";

    mono = reread_at + kStableThresholdMs;
    EXPECT_EQ(computeHeartbeatFloor(ops.op, l, kNowMs, clock, kObserverLeaseMs, kCadenceMs, obs, warned).fenced_now, 1u);
}

TEST(CASHeartbeatFloor, EmptyPrefixYieldsNoLiveMounts)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    Ops ops(b);
    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor.live, 0u);
    EXPECT_EQ(floor.terminated, 0u);
    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(floor.already_fenced, 0u);
}

namespace
{
HeartbeatFloor floorAt(Ops & ops, const Layout & l, uint64_t mono_ms, MountObservationMap & obs, std::set<String> & warned)
{
    return computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(mono_ms), kObserverLeaseMs, kCadenceMs, obs, warned);
}
}

/// The body declares 60 s against the observer's 30 s: 60 s + 3 s + the 1 s step.
TEST(CASHeartbeatFloor, HolderDeclaringALongerLeaseIsWatchedForThatLease)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    MountObservationMap obs;
    std::set<String> warned;
    floorAt(ops, l, 0, obs, warned);
    EXPECT_EQ(floorAt(ops, l, kStableThresholdMs, obs, warned).fenced_now, 0u)
        << "fenced after the observer's own lease while the declared lease still ran";
    EXPECT_EQ(floorAt(ops, l, 63'999, obs, warned).fenced_now, 0u);
    EXPECT_EQ(floorAt(ops, l, 64'000, obs, warned).fenced_now, 1u);
}

/// A 10 minute declared lease (the longest a new holder declares) against the observer's 30 s: 600 s + 30 s + the 1 s step.
TEST(CASHeartbeatFloor, HolderDeclaringTheMaximumLeaseIsFencedAtItsThresholdNotLater)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 600'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    MountObservationMap obs;
    std::set<String> warned;
    floorAt(ops, l, 0, obs, warned);
    EXPECT_EQ(floorAt(ops, l, 630'999, obs, warned).fenced_now, 0u);
    EXPECT_EQ(floorAt(ops, l, 631'000, obs, warned).fenced_now, 1u);
}

/// A shorter declared lease, a missing start stamp, equal stamps, an expiry before the start.
TEST(CASHeartbeatFloor, ShorterOrUnusableDeclaredLeaseIsWatchedForTheObserversOwn)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 10'000, false, 0);
    seedMount(ops.op, l, "s2", /*expires*/ 60'000, false, 0, /*seq*/ 1, /*started_at_ms*/ 0);
    seedMount(ops.op, l, "s3", /*expires*/ kNowMs, false, 0);
    seedMount(ops.op, l, "s4", /*expires*/ kNowMs - 1, false, 0);
    MountObservationMap obs;
    std::set<String> warned;
    floorAt(ops, l, 0, obs, warned);
    EXPECT_EQ(floorAt(ops, l, kStableThresholdMs - 1, obs, warned).fenced_now, 0u);
    EXPECT_EQ(floorAt(ops, l, kStableThresholdMs, obs, warned).fenced_now, 4u);
}

/// `gc_fenced` and farewell slots stay terminal whatever lease they declare: no observation, no write.
TEST(CASHeartbeatFloor, TerminalSlotsAreExcludedWhateverTheyDeclare)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ true, 0);
    seedMount(ops.op, l, "s2", /*expires*/ kNowMs + 60'000, false, std::numeric_limits<uint64_t>::max());
    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = floorAt(ops, l, 0, obs, warned);
    EXPECT_EQ(floor.already_fenced, 1u);
    EXPECT_EQ(floor.terminated, 1u);
    EXPECT_TRUE(obs.empty());
}

TEST(CASMountObservationThreshold, LongerOfOwnAndDeclaredLeasePlusDriftAndStep)
{
    EXPECT_EQ(mountObservationThresholdMs(30'000, 0, 1'000), 32'500u);
    EXPECT_EQ(mountObservationThresholdMs(30'000, 10'000, 1'000), 32'500u);
    EXPECT_EQ(mountObservationThresholdMs(30'000, 60'000, 1'000), 64'000u);
}

TEST(CASMountObservationThreshold, SaturatesInsteadOfWrapping)
{
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    /// `20 * (max / 21)` plus its twentieth is `max - 15`: the step alone decides the overflow.
    EXPECT_EQ(mountObservationThresholdMs(0, 20 * (max / 21), 14), max - 1);
    EXPECT_EQ(mountObservationThresholdMs(0, 20 * (max / 21), 16), max);
    EXPECT_EQ(mountObservationThresholdMs(30'000, max - 10, 0), max);
}

TEST(CASMountObservationThreshold, DeclaredLeaseIsThePositiveStampInterval)
{
    MountLease m;
    m.started_at_ms = 1'000;
    m.expires_at_ms = 61'000;
    EXPECT_EQ(declaredMountLeaseMs(m), 60'000u);
    m.started_at_ms = 0;
    EXPECT_EQ(declaredMountLeaseMs(m), 0u) << "a body without the start stamp";
    m.started_at_ms = 61'000;
    EXPECT_EQ(declaredMountLeaseMs(m), 0u) << "a farewell stamps both equal";
    m.started_at_ms = 70'000;
    EXPECT_EQ(declaredMountLeaseMs(m), 0u) << "expiry before start";
}

TEST(CASHeartbeatFloor, HolderWithADifferentLeaseIsWarnedAboutWithoutChangingTheClassification)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    const String before = ops.op.read(l.mountKey("s1"), Retry::standard())->bytes;

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(capture.count(
        "This server is configured with cas_mount_lease_ttl_ms=30000 but the holder of slot s1 "
        "(hostname=h-s1, pid=100) declares a lease of 60000 ms. Observers wait for the longer of the two; "
        "a longer lease on one server delays recovery after that server's crash."), 1u)
        << capture.captured();
    EXPECT_EQ(floor.live, 1u);
    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(ops.op.read(l.mountKey("s1"), Retry::standard())->bytes, before);
    EXPECT_EQ(warned, std::set<String>{"s1"});
}

TEST(CASHeartbeatFloor, HolderWithTheSameLeaseIsNotWarnedAbout)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + kObserverLeaseMs, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(capture.count(kLeaseMismatchMarker), 0u) << capture.captured();
    EXPECT_TRUE(warned.empty());
}

TEST(CASHeartbeatFloor, FarewellBodyIsNotWarnedAbout)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// What `MountLeaseRenewer::terminate` writes: both stamps equal, farewell sentinel.
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs, /*fenced*/ false,
              /*min_active_build_sequence*/ std::numeric_limits<uint64_t>::max());

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(floor.terminated, 1u);
    EXPECT_EQ(capture.count(kLeaseMismatchMarker), 0u) << capture.captured();
}

/// Both slots keep a 60 s body that differs from the observer's 30 s, but a fenced or departed holder
/// cannot delay anything, so naming it would only alarm the operator.
TEST(CASHeartbeatFloor, TerminalSlotsWithADifferentLeaseAreNotWarnedAbout)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l("p");
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ true, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ kNowMs + 60'000, /*fenced*/ false,
              /*min_active_build_sequence*/ std::numeric_limits<uint64_t>::max());

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    const HeartbeatFloor floor = floorAt(ops, l, 0, obs, warned);

    EXPECT_EQ(floor.already_fenced, 1u);
    EXPECT_EQ(floor.terminated, 1u);
    EXPECT_EQ(capture.count(kLeaseMismatchMarker), 0u) << capture.captured();
    EXPECT_TRUE(warned.empty());
}

TEST(CASHeartbeatFloor, MissingStartStampIsNotWarnedAbout)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// A body without `started_at_ms` decodes it as 0; the interval to `expires_at_ms` means nothing.
    seedMount(ops.op, l, "s1", /*expires*/ 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0,
              /*seq*/ 1, /*started_at_ms*/ 0);

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(capture.count(kLeaseMismatchMarker), 0u) << capture.captured();
}

TEST(CASHeartbeatFloor, LeaseMismatchIsWarnedOncePerServerRootId)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    DB::Cas::tests::ScopedLogCapture capture("CasHeartbeatFloor", "warning");
    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    /// A renewal gives s1 a new incarnation; the next round reads both bodies again.
    renewMount(ops.op, l, "s1");
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(1), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(capture.count(kLeaseMismatchMarker), 2u) << capture.captured();
    EXPECT_EQ(capture.count("holder of slot s1 ("), 1u) << capture.captured();
    EXPECT_EQ(capture.count("holder of slot s2 ("), 1u) << capture.captured();
}

TEST(CASHeartbeatFloor, UnseenSridPrunedFromLeaseMismatchWarned)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedMount(ops.op, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);
    seedMount(ops.op, l, "s2", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active_build_sequence*/ 0);

    MountObservationMap obs;
    std::set<String> warned;
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(0), kObserverLeaseMs, kCadenceMs, obs, warned);
    ASSERT_EQ(warned, (std::set<String>{"s1", "s2"}));

    ASSERT_EQ(ops.op.removeCurrent(l.mountKey("s2"), Retry::standard()), Removal::Removed);
    computeHeartbeatFloor(ops.op, l, kNowMs, frozenClock(1), kObserverLeaseMs, kCadenceMs, obs, warned);

    EXPECT_EQ(warned, std::set<String>{"s1"});
}

/// ---- Task 1 (Phase 2): `listMounts` — read-only mount-slot enumeration for introspection ----

TEST(CASListMounts, ClassifiesEveryStateReadOnly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const uint64_t now_ms = 1'000'000;
    const uint64_t ttl_ms = 10'000;

    Ops ops(backend);
    /// live: fresh claim for srid "a"
    ASSERT_EQ(claimMount(ops.op, layout, "a", UInt128{1}, /*our_epoch=*/1, now_ms, ttl_ms).kind,
              MountClaimResult::Claimed);
    /// expired: claim for "b" whose lease ran out long before now_ms
    ASSERT_EQ(claimMount(ops.op, layout, "b", UInt128{2}, 1, now_ms - 100'000, ttl_ms).kind,
              MountClaimResult::Claimed);
    /// corrupt: garbage bytes in "c"'s mount slot
    mustCommit(ops.op.create(layout.mountKey("c"), "garbage-not-a-proto", Retry::standard()), "corrupt slot");

    auto mounts = listMounts(ops.op, layout, now_ms, /*skew_margin_ms=*/ttl_ms / 2);
    ASSERT_EQ(mounts.size(), 3u);
    std::map<String, String> by_srid;
    for (const auto & m : mounts)
        by_srid[m.srid] = m.state;
    EXPECT_EQ(by_srid["a"], "live");
    EXPECT_EQ(by_srid["b"], "expired");
    EXPECT_EQ(by_srid["c"], "corrupt");

    /// READ-ONLY guarantee: "b" is expired but must NOT be fenced by listMounts
    /// (computeHeartbeatFloor would stamp gc_fenced=true; the introspection view must not).
    auto again = listMounts(ops.op, layout, now_ms, ttl_ms / 2);
    for (const auto & m : again)
        if (m.srid == "b")
        {
            EXPECT_FALSE(m.lease.gc_fenced);
            EXPECT_EQ(m.state, "expired");
        }
}

/// A `srid` may itself contain `/` (e.g. `shard-01/replica-a` — legal per
/// `CASServerRootId.ValidationAcceptsCleanPathsRejectsBad`). Slicing the key by the last `/` before
/// the `/mount` suffix (as opposed to by `serverRootsPrefix()` length) truncates it to `replica-a`.
TEST(CASListMounts, NestedSridIsNotTruncated)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const uint64_t now_ms = 1'000'000;
    const uint64_t ttl_ms = 10'000;

    Ops ops(backend);
    ASSERT_EQ(claimMount(ops.op, layout, "shard-01/replica-a", UInt128{1}, /*our_epoch=*/1, now_ms, ttl_ms).kind,
              MountClaimResult::Claimed);

    auto mounts = listMounts(ops.op, layout, now_ms, /*skew_margin_ms=*/ttl_ms / 2);
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0].srid, "shard-01/replica-a");
    EXPECT_EQ(mounts[0].state, "live");
}

/// "A fence costs an epoch": a same-(uuid, epoch) re-claim must NOT refresh a `gc_fenced` body in
/// place — that would reactivate a fenced incarnation. It is terminal for THIS epoch; only a
/// DIFFERENT (fresh) epoch may reclaim the slot.
TEST(CASClaimMount, SameEpochFencedIsNotRefreshable)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    Ops ops(backend);
    /// mint for (uuid 1, epoch 1), then fence it in place (what computeHeartbeatFloor does):
    ASSERT_EQ(claimMount(ops.op, layout, "a", DB::UInt128{1}, 1, 1000, 10'000).kind,
              MountClaimResult::Claimed);
    {
        auto got = ops.op.read(layout.mountKey("a"), Retry::standard());
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        mustCommit(ops.op.replace(layout.mountKey("a"), encodeMountLease(fenced), got->etag, Retry::standard()),
                   "fence-out");
    }
    /// Same (uuid, epoch) re-claim must NOT refresh a fenced body — a fence costs an epoch:
    const auto r = claimMount(ops.op, layout, "a", DB::UInt128{1}, 1, 2000, 10'000);
    EXPECT_EQ(r.kind, MountClaimResult::FencedSelf);
    /// The body on the backend is still the fenced one (no write happened):
    EXPECT_TRUE(decodeMountLease(ops.op.read(layout.mountKey("a"), Retry::standard())->bytes).gc_fenced);
    /// A DIFFERENT epoch reclaims immediately (existing branch, unchanged):
    EXPECT_EQ(claimMount(ops.op, layout, "a", DB::UInt128{1}, 2, 2000, 10'000).kind,
              MountClaimResult::Claimed);
}

/// An older allocation that reaches the slot late must not replace a newer incarnation of this server,
/// whatever certifies that incarnation's end: recovery seals only epochs below the live one.
TEST(CASClaimMountMonotonicity, LowerEpochOverANewerBodyFailsWithoutWriting)
{
    enum class Shape { Live, GcFenced, CleanFarewell, ProvenDead, UnsafeAuthorized };
    for (const Shape shape : {Shape::Live, Shape::GcFenced, Shape::CleanFarewell, Shape::ProvenDead,
                              Shape::UnsafeAuthorized})
    {
        SCOPED_TRACE(static_cast<int>(shape));
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        const String key = l.mountKey("r");
        ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/8, 1000, 30000).kind, MountClaimResult::Claimed);
        if (shape == Shape::GcFenced || shape == Shape::CleanFarewell)
        {
            const auto got = ops.op.read(key, Retry::standard());
            ASSERT_TRUE(got.has_value());
            MountLease body = decodeMountLease(got->bytes);
            ++body.seq;
            if (shape == Shape::GcFenced)
                body.gc_fenced = true;
            else
                body.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
            mustCommit(ops.op.replace(key, encodeMountLease(body), got->etag, Retry::standard()), "terminal newer body");
        }
        const auto before = ops.op.read(key, Retry::standard());
        ASSERT_TRUE(before.has_value());
        const std::optional<Etag> token = before->etag;

        DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::ABORTED, "newer than the writer_epoch", [&]
        {
            (void)claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/7, 2000, 30000,
                             shape == Shape::ProvenDead ? token : std::nullopt, /*sink=*/{},
                             shape == Shape::UnsafeAuthorized ? token : std::nullopt);
        });
        EXPECT_EQ(ops.op.read(key, Retry::standard())->bytes, before->bytes) << "the newer body was replaced";
    }
}

/// Two processes holding one epoch, as 26.6.4 allocators without the counter nonce could produce.
TEST(CASClaimMountMonotonicity, TwoClaimersOfOneEpochDoNotShareTheSlot)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    const uint64_t shared_epoch = 2;

    /// The competitor claims first.
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), shared_epoch, 1000, 30000).kind, MountClaimResult::Claimed);
    const auto competitor = ops.op.read(l.mountKey("r"), Retry::standard());
    ASSERT_TRUE(competitor.has_value());

    const MountClaimResult ours = claimMount(ops.op, l, "r", UInt128(1), shared_epoch, 2000, 30000);
    EXPECT_EQ(ours.kind, MountClaimResult::LiveDoubleStart);
    ASSERT_TRUE(ours.body.has_value());
    EXPECT_EQ(ours.body->write_attempt_id, decodeMountLease(competitor->bytes).write_attempt_id);
    ASSERT_TRUE(ours.etag.has_value());
    EXPECT_TRUE(*ours.etag == competitor->etag);
    EXPECT_EQ(ops.op.read(l.mountKey("r"), Retry::standard())->bytes, competitor->bytes) << "the slot was written";
}

TEST(CASClaimMountMonotonicity, AwaitingExpiryReturnsAtOnceOnABodyOfOurOwnPair)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    /// Another process allocated epoch 7 as well; its body is in the slot.
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, 1000, 100).kind, MountClaimResult::Claimed);
    const auto before = ops.op.read(l.mountKey("r"), Retry::standard());
    ASSERT_TRUE(before.has_value());

    uint64_t mono = 0;
    int sleeps = 0;
    const auto r = claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), /*our_epoch=*/7,
        [] { return uint64_t{1000}; }, [&] { return mono; }, /*ttl_ms=*/100, /*poll_interval_ms=*/20,
        [&](uint64_t ms)
        {
            mono += ms;
            if (++sleeps > 100)
                throw std::runtime_error("the observation of our own (uuid, epoch) never ended");
        });
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_EQ(sleeps, 0) << "no observation can authorize replacing our own pair";
    EXPECT_EQ(ops.op.read(l.mountKey("r"), Retry::standard())->bytes, before->bytes);
}

TEST(CASClaimMountMonotonicity, AwaitingExpiryFailsAtOnceWhenARacedWriteMeetsANewerEpoch)
{
    auto b = std::make_shared<DB::Cas::tests::MountSlotRaceBackend>();
    Layout l("p");
    Ops ops(b);
    /// Absent at our read; a newer start of this server mints the slot before our create lands.
    b->before_put_if_absent = [&]
    {
        DB::Cas::tests::OperationForTest racer(*b);
        ASSERT_EQ(claimMount(*racer, l, "r", UInt128(1), /*our_epoch=*/8, 1000, 100).kind, MountClaimResult::Claimed);
    };

    uint64_t mono = 0;
    int sleeps = 0;
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::ABORTED, "newer than the writer_epoch", [&]
    {
        (void)claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), /*our_epoch=*/7,
            [] { return uint64_t{1000}; }, [&] { return mono; }, /*ttl_ms=*/100, /*poll_interval_ms=*/20,
            [&](uint64_t ms)
            {
                mono += ms;
                ++sleeps;
            });
    });
    EXPECT_FALSE(b->before_put_if_absent) << "the newer start was never installed";
    EXPECT_EQ(sleeps, 0) << "a newer incarnation is refused, not observed";
    EXPECT_EQ(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).writer_epoch, 8u);
}

/// The engine settles our own landed attempt inside the write call, so a claim never meets its own body
/// on a later call.
TEST(CASClaimMountMonotonicity, AConfirmedLandedReclaimIsStillClaimed)
{
    auto b = std::make_shared<LandedReplaceAckLostOnceBackend>();
    Layout l("p");
    Ops ops(b);
    b->key = l.mountKey("r");
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, 1000, 30000).kind, MountClaimResult::Claimed);
    {
        const auto got = ops.op.read(b->key, Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        ++fenced.seq;
        mustCommit(ops.op.replace(b->key, encodeMountLease(fenced), got->etag, Retry::standard()), "fence-out");
    }

    b->armed = true;
    const MountClaimResult r = claimMount(ops.op, l, "r", UInt128(1), 8, 2000, 30000);
    ASSERT_FALSE(b->armed) << "the lost response was never injected";
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, MountPriorState::Fenced);
    ASSERT_TRUE(r.body.has_value());
    const MountLease slot = decodeMountLease(ops.op.read(b->key, Retry::standard())->bytes);
    EXPECT_EQ(slot.writer_epoch, 8u);
    EXPECT_EQ(slot.write_attempt_id, r.body->write_attempt_id);
}

/// ---- rev.6 Task 4: observation-based lease reclaim (no cross-node wall-clock trust) ----

/// A same-uuid, different-epoch lease whose STAMPED `expires_at_ms` looks long expired on OUR wall
/// clock must NOT be reclaimed by that comparison alone — a clock-skewed or simply late-observing
/// caller must never trust a bare wall-clock read across incarnations. `claimMount` (without a
/// `proven_dead_incarnation`) always reports `LiveDoubleStart` for this branch now; only the observation
/// loop (`claimMountAwaitingExpiry`) may turn it into a reclaim, and only after proving death on ITS
/// OWN clock.
TEST(CASMountObservation, ExpiredLookingLeaseIsNotReclaimedByWallClock)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    Ops ops(b);
    /// Predecessor epoch 7 stamped expires_at_ms = 1000; our wall clock says 999999 (long past).
    auto first = claimMount(ops.op, l, "r", UInt128(1), 7, /*now_ms=*/500, /*ttl_ms=*/500);
    ASSERT_EQ(first.kind, MountClaimResult::Claimed);
    auto r = claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/8, /*now_ms=*/999999, 500);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);  /// no wall-clock trust
}

/// The observation loop reclaims once the observed incarnation has held stable for the FULL
/// rate-bound threshold (`ttl_ms + ttl_ms/20 + poll_interval_ms`) on its OWN (injected, fake) clock —
/// never short-circuiting on the wall clock, which this test drives to an irrelevant, already-expired
/// value.
TEST(CASMountObservation, IncarnationStableForThresholdThenReclaimed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, 500, 500).kind, MountClaimResult::Claimed);
    uint64_t mono = 0;
    std::vector<uint64_t> sleeps;
    auto r = claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), 8,
        []{ return uint64_t{999999}; },                 /// wall clock: irrelevant
        [&]{ return mono; },                             /// observation clock
        /*ttl_ms=*/500, /*poll_interval_ms=*/50,
        [&](uint64_t ms){ sleeps.push_back(ms); mono += ms; });
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, MountPriorState::UncleanObserved);
    EXPECT_GE(mono, 500 + 500 / 20 + 50);               /// full threshold actually waited
}

/// A renewal DURING the observation window (the real holder is still alive) mints a new incarnation —
/// the loop must detect the mismatch and RESTART the observation from it, never reclaiming off a
/// window that started watching a now-superseded incarnation.
TEST(CASMountObservation, RenewalDuringObservationRestartsIt)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    uint64_t renewer_boot = 0;
    Ops ops(b, &renewer_boot);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, 500, 500).kind, MountClaimResult::Claimed);

    /// The real (still-alive) holder's renewer for epoch 7: `start()` adopts the slot `claimMount` just
    /// wrote (no seq bump, per the ADOPT RULE), then a synchronous renewal mints a new incarnation
    /// mid-observation.
    uint64_t renewer_wall = 500;
    MountLeaseRenewer renewer(ops.mount, ops.farewell, l, "r", UInt128(1), 7, std::chrono::milliseconds(500),
                             [&] { return renewer_wall; }, [] { return uint64_t{0}; }, {},
                             std::chrono::milliseconds(0), [&] { return renewer_boot; });
    renewer.start();

    const uint64_t threshold_ms = 500 + 500 / 20 + 50;   /// = 575
    uint64_t mono = 0;
    bool renewed = false;
    int wait_starts = 0;
    auto r = claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), 8,
        []{ return uint64_t{999999}; },                 /// wall clock: irrelevant
        [&]{ return mono; },                             /// observation clock
        /*ttl_ms=*/500, /*poll_interval_ms=*/50,
        [&](uint64_t ms)
        {
            mono += ms;
            /// Renew once, close to (but before) the first window's threshold would complete —
            /// almost the whole first window is wasted, forcing a near-full second window.
            if (!renewed && mono >= threshold_ms - 50)
            {
                renewed = true;
                renewOrThrow(renewer);
            }
        },
        /*on_wait_start=*/[&](const MountLease &, uint64_t) { ++wait_starts; });

    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, MountPriorState::UncleanObserved);
    EXPECT_EQ(wait_starts, 2);                           /// the renewal forced exactly one restart
    /// The restart's own window did not begin until at least (threshold - poll) had already elapsed,
    /// so total elapsed time is well over a single threshold window.
    EXPECT_GE(mono, (threshold_ms - 50) + threshold_ms);
}

/// A GC-fenced lease is a terminal, already-threshold-gated certificate of death (the fence-out
/// itself cost the predecessor an epoch) — the observation loop must reclaim it on the FIRST attempt,
/// with zero polling/sleeping.
TEST(CASMountObservation, GcFencedIsReclaimedInstantlyWithPriorFenced)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l{"p"};
    Ops ops(b);
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), 7, 1000, 500).kind, MountClaimResult::Claimed);

    /// Fence it manually (what `computeHeartbeatFloor`'s fence-out does): gc_fenced=true, seq+1,
    /// guarded by the observed incarnation.
    {
        auto got = ops.op.read(l.mountKey("r"), Retry::standard());
        ASSERT_TRUE(got.has_value());
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        mustCommit(ops.op.replace(l.mountKey("r"), encodeMountLease(fenced), got->etag, Retry::standard()),
                   "fence-out");
    }

    int sleeps = 0;
    auto r = claimMountAwaitingExpiry(ops.op, l, "r", UInt128(1), /*our_epoch=*/8,
        []{ return uint64_t{999999}; },
        []{ return uint64_t{0}; },
        /*ttl_ms=*/500, /*poll_interval_ms=*/50,
        [&](uint64_t) { ++sleeps; });

    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, MountPriorState::Fenced);
    EXPECT_EQ(sleeps, 0);
}

/// ---- Stage B Task 3: `isCreatorFenceTerminal` -- the cross-process terminality predicate
/// `CasRefCatalog::reconcileStaleCreator` gates on. Built from `writer_epoch` plus the SAME two
/// clock-free certificates `probeNonTerminalMountSlots`/`computeHeartbeatFloor` already use, PLUS a
/// third certificate available only here: a currently-live DIFFERENT `writer_epoch` at the slot. ----

TEST(CASFenceTerminal, AbsentMountSlotIsNotTerminal)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    EXPECT_FALSE(isCreatorFenceTerminal(ops.op, l, "never-mounted", 1))
        << "absence proves nothing about liveness -- never waved through";
}

TEST(CASFenceTerminal, UndecodableMountBodyIsNotTerminal)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    mustCommit(ops.op.create(l.mountKey("r"), "garbage-not-a-lease", Retry::standard()), "undecodable lease");
    EXPECT_FALSE(isCreatorFenceTerminal(ops.op, l, "r", 1))
        << "an unreadable lease of some other format generation must block, never wave through";
}

TEST(CASFenceTerminal, GcFencedIsTerminal)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/7, 1000, 500).kind, MountClaimResult::Claimed);
    auto got = ops.op.read(l.mountKey("r"), Retry::standard());
    ASSERT_TRUE(got.has_value());
    MountLease fenced = decodeMountLease(got->bytes);
    fenced.gc_fenced = true;
    mustCommit(ops.op.replace(l.mountKey("r"), encodeMountLease(fenced), got->etag, Retry::standard()),
               "fence-out");

    EXPECT_TRUE(isCreatorFenceTerminal(ops.op, l, "r", 7));
}

TEST(CASFenceTerminal, CleanFarewellIsTerminal)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/7, 1000, 500).kind, MountClaimResult::Claimed);
    auto got = ops.op.read(l.mountKey("r"), Retry::standard());
    ASSERT_TRUE(got.has_value());
    MountLease retired = decodeMountLease(got->bytes);
    retired.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    mustCommit(ops.op.replace(l.mountKey("r"), encodeMountLease(retired), got->etag, Retry::standard()),
               "farewell");

    EXPECT_TRUE(isCreatorFenceTerminal(ops.op, l, "r", 7));
}

TEST(CASFenceTerminal, ADifferentLiveWriterEpochIsTerminalForTheOldOne)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    /// Slot now held at epoch 8 -- epoch 7's incarnation is superseded regardless of ITS OWN
    /// certificate (neither fenced nor farewelled).
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/8, 1000, 500).kind, MountClaimResult::Claimed);

    EXPECT_TRUE(isCreatorFenceTerminal(ops.op, l, "r", 7))
        << "a different epoch is currently live at this slot -- epoch 7 can never reclaim it";
    EXPECT_FALSE(isCreatorFenceTerminal(ops.op, l, "r", 8))
        << "epoch 8 IS the current live epoch -- not terminal";
}

/// A merely EXPIRED lease (wall-clock past `expires_at_ms`, same epoch, no certificate) must NOT be
/// treated as terminal -- mirrors `claimMount`'s own refusal to trust a bare timestamp comparison.
TEST(CASFenceTerminal, ExpiredButSameEpochAndUncertifiedIsNotTerminal)
{
    Ops ops(std::make_shared<InMemoryBackend>());
    Layout l{"p"};
    /// A lease whose stamped expiry is already far in the past, same epoch throughout.
    ASSERT_EQ(claimMount(ops.op, l, "r", UInt128(1), /*our_epoch=*/7, /*now_ms=*/0, /*ttl_ms=*/1).kind,
              MountClaimResult::Claimed);

    EXPECT_FALSE(isCreatorFenceTerminal(ops.op, l, "r", 7))
        << "expiry alone is never a certificate of death, exactly like claimMount's own discipline";
}

/// The absent-epoch path's post-conflict recheck is a CHECK, not a blanket refusal, and both halves
/// have to hold: work that became visible across the conflict must block the allocation, and an
/// unchanged, still-empty subtree must let it proceed from the winner's own epoch state. Dropping the
/// recheck breaks the first arm; turning it into an unconditional refusal breaks the second.
TEST(CASServerRoot, AllocateWriterEpochKeepsThePostConflictCorruptionCheck)
{
    const Layout layout("p");
    {
        auto backend = std::make_shared<EpochConflictBackend>(/*reveal_owned_work=*/true);
        Ops ops(backend);
        EXPECT_THROW(allocateWriterEpoch(
            ops.op, layout, "root/x", emptyCatalogObservation()), DB::Exception);
        EXPECT_TRUE(backend->fired);
        ASSERT_TRUE(backend->winner_installed);
    }
    {
        auto backend = std::make_shared<EpochConflictBackend>(/*reveal_owned_work=*/false);
        Ops ops(backend);
        EXPECT_EQ(allocateWriterEpoch(
            ops.op, layout, "root/x", emptyCatalogObservation()), 2u)
            << "the second decision must allocate from the conflict winner's epoch, not refuse outright";
        EXPECT_TRUE(backend->fired);
        ASSERT_TRUE(backend->winner_installed);
        const auto epoch = ops.op.read(layout.epochKey("root/x"), Retry::standard());
        ASSERT_TRUE(epoch.has_value());
        EXPECT_EQ(decodeServerEpoch(epoch->bytes).next_writer_epoch, 3u);
    }
}

/// Adoption costs exactly two requests: one read that both decides the branch and supplies the
/// precondition, and one write. A presence probe ahead of the read, or a second read to recover a
/// precondition the first one already carried, shows up here as a third request.
TEST(CASMountLease, ClaimAdoptIsTwoRequests)
{
    auto backend = std::make_shared<RequestCountingBackend>();
    Layout l("p");
    uint64_t now = 1000;
    Ops ops(backend);

    /// The absent-slot mint.
    backend->reads = backend->heads = backend->writes = 0;
    MountLeaseRenewer minting(ops.mount, ops.farewell, l, "fresh", UInt128(1), 7,
                             std::chrono::milliseconds(100), [&] { return now; }, [] { return uint64_t{0}; });
    minting.start();
    EXPECT_EQ(backend->reads, 1u);
    EXPECT_EQ(backend->writes, 1u);
    EXPECT_EQ(backend->heads, 0u);

    /// The adoption of a slot `claimMount` already wrote.
    ASSERT_EQ(claimMount(ops.op, l, "adopted", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100).kind,
              MountClaimResult::Claimed);
    backend->reads = backend->heads = backend->writes = 0;
    MountLeaseRenewer adopting(ops.mount, ops.farewell, l, "adopted", UInt128(1), 7,
                              std::chrono::milliseconds(100), [&] { return now; }, [] { return uint64_t{0}; });
    adopting.start();
    EXPECT_EQ(backend->reads, 1u);
    EXPECT_EQ(backend->writes, 1u);
    EXPECT_EQ(backend->heads, 0u);
}

/// A mount whose fence has dropped must still hand its slot back: the renewal is refused (it would be
/// writing under authority this node no longer holds), while the farewell runs on the open plane and
/// lands. Deliberately two renewers: `release` is admitted only from `Active`, so a renewer whose
/// renewal already went terminal never reaches its own farewell -- the ordering the two halves below
/// pin separately.
TEST(CASMountLease, FarewellRunsOnAnOpenFenceAfterTheMountFenceIsLost)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;
    bool fence_lost = false;

    CasRequests mount_requests(backend, Fence{
        [] { return uint64_t{0}; },
        [&fence_lost](uint64_t, uint64_t) { return fence_lost ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
        [&fence_lost](uint64_t)
        {
            if (fence_lost)
                throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "mount fence lost");
        }});
    mount_requests.setNowFnForTest([&boot] { return boot; });
    mount_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasRequests open_requests = openRequestsForTest(backend);
    open_requests.setNowFnForTest([&boot] { return boot; });
    open_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasOperation seed = open_requests.admit();

    ASSERT_EQ(claimMount(seed, l, "renewing", UInt128(1), 7, now, /*ttl*/ 1000).kind, MountClaimResult::Claimed);
    ASSERT_EQ(claimMount(seed, l, "departing", UInt128(1), 7, now, /*ttl*/ 1000).kind, MountClaimResult::Claimed);

    MountLeaseRenewer renewing(mount_requests, open_requests, l, "renewing", UInt128(1), 7,
                              std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                              {}, std::chrono::milliseconds(0), [&] { return boot; });
    MountLeaseRenewer departing(mount_requests, open_requests, l, "departing", UInt128(1), 7,
                               std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                               {}, std::chrono::milliseconds(0), [&] { return boot; });
    renewing.start();
    departing.start();

    fence_lost = true;

    const MountRenewResult refused = renewing.renew(MountRenewOperationEnvironment{});
    EXPECT_EQ(refused.outcome, MountRenewOutcome::Terminal);
    EXPECT_FALSE(refused.sent_any);
    EXPECT_FALSE(renewing.canRelease()) << "a terminal renewal leaves no farewell to run";

    EXPECT_NO_THROW(departing.release());
    const MountLease farewell = decodeMountLease(seed.read(l.mountKey("departing"), Retry::standard())->bytes);
    EXPECT_EQ(farewell.min_active_build_sequence, std::numeric_limits<uint64_t>::max());
}

/// The claim is admitted off the mount fence, and it has to be: a self-remount runs with the fence
/// already latched lost, so a claim gated on it could never reclaim the slot. What keeps the claim
/// safe is the conditional write it makes, not the fence.
TEST(CASMountLease, ClaimIsNotAdmittedUnderTheMountFence)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;

    CasRequests mount_requests(backend, Fence{
        [] { return uint64_t{0}; },
        [](uint64_t, uint64_t) { return Fence::Admit::LostOrRearmed; },
        [](uint64_t) { throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "mount fence lost"); }});
    mount_requests.setNowFnForTest([&boot] { return boot; });
    mount_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasRequests open_requests = openRequestsForTest(backend);
    open_requests.setNowFnForTest([&boot] { return boot; });
    open_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });

    MountLeaseRenewer renewer(mount_requests, open_requests, l, "r", UInt128(1), 7,
                            std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                            {}, std::chrono::milliseconds(0), [&] { return boot; });
    EXPECT_NO_THROW(renewer.start());

    CasOperation reader = open_requests.admit();
    const MountLease claimed = decodeMountLease(reader.read(l.mountKey("r"), Retry::standard())->bytes);
    EXPECT_EQ(claimed.writer_epoch, 7u);
    EXPECT_EQ(claimed.seq, 1u);
}

/// A lost owner-claim race is decided from the conflict's OWN resolve observation, so the two outcomes
/// have to be told apart from that alone: a racer that installed our uuid leaves nothing to do, a
/// foreign one fails closed. Reading the key again would answer a later question than the conflict
/// asked, and would cost a request per race.
TEST(CASServerRootClaim, OwnerLostToARacerIsDecidedFromTheConflictObservation)
{
    Layout l("p");
    {
        auto backend = std::make_shared<OwnerRaceBackend>(UInt128(1));
        Ops ops(backend);
        EXPECT_NO_THROW(claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()));
        EXPECT_TRUE(backend->fired);
        /// The pre-claim read plus the create's own conflict-resolve read, and no third: a re-read
        /// added back for the decision itself would raise this to 3.
        EXPECT_EQ(backend->owner_reads, 2u);
    }
    {
        auto backend = std::make_shared<OwnerRaceBackend>(UInt128(2));
        Ops ops(backend);
        DB::Cas::tests::expectThrowsCodeWithMessage(
            DB::ErrorCodes::CORRUPTED_DATA,
            "claimed by a different server during our claim",
            [&] { claimOwnerOrThrow(ops.op, l, "r", UInt128(1), emptyCatalogObservation()); });
        EXPECT_TRUE(backend->fired);
        EXPECT_EQ(backend->owner_reads, 2u);
    }
}

/// A tombstone sent against the owner's first ETag, delivered after a tombstone and a rejoin. Without a
/// life the rejoined owner's bytes equal the first owner's, so a content-derived ETag would admit it.
TEST(CASOwner, DelayedTombstoneAfterRejoinFailsOnLife)
{
    auto b = std::make_shared<ContentEtagOwnerBackend>();
    Layout l("p");
    Ops ops(b);
    const String key = l.ownerKey("r");
    const OwnerObject first{.server_uuid = UInt128(1), .retired_at_ms = std::nullopt, .owner_life = 1};
    mustCommit(ops.op.create(key, encodeOwner(first), Retry::standard()), "first owner");
    const Etag stale = currentEtag(ops.op, key);

    OwnerObject tombstone = first;
    tombstone.retired_at_ms = 1000;
    tombstone.owner_life = 2;
    mustCommit(ops.op.replace(key, encodeOwner(tombstone), currentEtag(ops.op, key), Retry::standard()), "tombstone");
    OwnerObject rejoined = first;
    rejoined.owner_life = 3;
    mustCommit(ops.op.replace(key, encodeOwner(rejoined), currentEtag(ops.op, key), Retry::standard()), "rejoin");
    ASSERT_NE(currentEtag(ops.op, key), stale) << "the life makes the rejoined bytes differ from the first";

    OwnerObject late = first;
    late.retired_at_ms = 2000;
    late.owner_life = 2;
    const WriteResult landed = ops.op.replace(key, encodeOwner(late), stale, Retry::standard());
    EXPECT_TRUE(std::holds_alternative<Conflict>(landed));
    EXPECT_EQ(decodeOwner(ops.op.read(key, Retry::standard())->bytes).owner_life, 3u);
}

/// A remount re-anchors its lease BEFORE it arms the fence for the new incarnation, so the fence is
/// still latched lost at that moment. The steady-state renewal is refused there — the sibling test
/// above pins that — and the remount's own renewal has to be admitted off the fence, or the pool could
/// never re-anchor and the remount attempt would fail on exactly the throttled store that caused it.
TEST(CASMountLease, RemountRenewalIsAdmittedOffTheMountFence)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    uint64_t boot = 0;

    CasRequests mount_requests(backend, Fence{
        [] { return uint64_t{0}; },
        [](uint64_t, uint64_t) { return Fence::Admit::LostOrRearmed; },
        [](uint64_t) { throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "mount fence lost"); }});
    mount_requests.setNowFnForTest([&boot] { return boot; });
    mount_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });
    CasRequests open_requests = openRequestsForTest(backend);
    open_requests.setNowFnForTest([&boot] { return boot; });
    open_requests.setSleepFnForTest([&boot](uint64_t ms) { boot += ms; });

    MountLeaseRenewer renewer(mount_requests, open_requests, l, "r", UInt128(1), 7,
                            std::chrono::milliseconds(1000), [&] { return now; }, [] { return uint64_t{0}; },
                            {}, std::chrono::milliseconds(0), [&] { return boot; });
    renewer.start();

    const MountRenewResult redo = renewer.renewForRemount();
    EXPECT_EQ(redo.outcome, MountRenewOutcome::Committed);

    CasOperation reader = open_requests.admit();
    EXPECT_EQ(decodeMountLease(reader.read(l.mountKey("r"), Retry::standard())->bytes).seq, 2u);
}

namespace
{
/// A rejoin by hand: the next life under `uuid`, not retired.
void rewriteOwnerAt(Backend & backend, const Layout & l, const String & srid, UInt128 uuid)
{
    DB::Cas::tests::OperationForTest op(backend);
    const auto current = (*op).read(l.ownerKey(srid), Retry::once());
    ASSERT_TRUE(current.has_value());
    const OwnerObject next{
        .server_uuid = uuid, .retired_at_ms = std::nullopt, .owner_life = decodeOwner(current->bytes).owner_life + 1,
        .retirement_verified = false};
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(l.ownerKey(srid), encodeOwner(next), current->etag, Retry::once())));
}

MountLease slotBody(Backend & backend, const Layout & l, const String & srid)
{
    DB::Cas::tests::OperationForTest op(backend);
    const auto current = (*op).read(l.mountKey(srid), Retry::once());
    if (!current)
        throw std::runtime_error("mount absent: " + l.mountKey(srid));
    return decodeMountLease(current->bytes);
}

PoolConfig rootConfig()
{
    return PoolConfig{.pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r"};
}
}

/// The tombstone lands between the owner claim and the mount write; the only write of the mount key in
/// this open is the reclaim of the first open's farewell.
TEST(CASMount, NormalMountRefusesARetiredOwnerAfterClaim)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    { auto first = Pool::open(b, rootConfig()); }
    bool fired = false;
    b->onBeforeWrite(l.mountKey("r"), [&]
    {
        if (std::exchange(fired, true))
            return;
        DB::Cas::tests::tombstoneOwnerForTest(*b, l.ownerKey("r"));
    });
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "decommissioned", [&] { Pool::open(b, rootConfig()); });
    ASSERT_TRUE(fired);
    const MountLease slot = slotBody(*b, l, "r");
    EXPECT_EQ(slot.server_uuid, UInt128(1));
    EXPECT_EQ(slot.writer_epoch, 2u) << "the refused claim's own body, not the first open's farewell";
    EXPECT_TRUE(slot.gc_fenced) << "the refused claimer fences itself so nobody waits its lease out";
}

/// A victim prechecked before a decommission wakes after a new-uuid rejoin and reclaims its own farewell.
TEST(CASMount, ClaimRecheckRefusesAForeignOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    { auto first = Pool::open(b, rootConfig()); }
    bool fired = false;
    b->onBeforeWrite(l.mountKey("r"), [&]
    {
        if (std::exchange(fired, true))
            return;
        rewriteOwnerAt(*b, l, "r", UInt128(2));
    });
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "different server", [&] { Pool::open(b, rootConfig()); });
    ASSERT_TRUE(fired);
    const MountLease slot = slotBody(*b, l, "r");
    EXPECT_EQ(slot.server_uuid, UInt128(1));
    EXPECT_EQ(slot.writer_epoch, 2u);
    EXPECT_TRUE(slot.gc_fenced);
}

TEST(CASMount, ClaimRecheckRefusesAnAbsentOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    { auto first = Pool::open(b, rootConfig()); }
    bool fired = false;
    b->onBeforeWrite(l.mountKey("r"), [&]
    {
        if (std::exchange(fired, true))
            return;
        DB::Cas::tests::OperationForTest op(*b);
        ASSERT_EQ((*op).removeCurrent(l.ownerKey("r"), Retry::once()), Removal::Removed);
    });
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "vanished", [&] { Pool::open(b, rootConfig()); });
    ASSERT_TRUE(fired);
    EXPECT_TRUE(slotBody(*b, l, "r").gc_fenced);
}

/// The owner read refuses, and the self-fence write cannot reach the store: the refusal is still the
/// owner's reason and the slot keeps the claimer's live body for GC to fence.
TEST(CASMount, RecheckRefusalSurvivesAFailedSelfFence)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    { auto first = Pool::open(b, rootConfig()); }
    bool fired = false;
    b->onBeforeWrite(l.mountKey("r"), [&]
    {
        if (std::exchange(fired, true))
            return;
        DB::Cas::tests::tombstoneOwnerForTest(*b, l.ownerKey("r"));
        /// The claim write has already passed its armed-failure check, so this arming fails the next
        /// mount write: the self-fence.
        b->failNextWriteWith(l.mountKey("r"), std::make_exception_ptr(
            DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "store refused the fence write")));
    });
    DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "decommissioned", [&] { Pool::open(b, rootConfig()); });
    ASSERT_TRUE(fired);
    const MountLease slot = slotBody(*b, l, "r");
    EXPECT_EQ(slot.writer_epoch, 2u) << "the claim itself landed";
    EXPECT_FALSE(slot.gc_fenced) << "the fence write was refused; the body is untouched";
}

/// The GC fenced our lease; the remount reclaims it, and the owner was tombstoned in between.
TEST(CASMount, FenceRecoveryReclaimRechecksTheOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    auto store = Pool::open(b, rootConfig());
    DB::Cas::tests::fenceOutMountForTest(*b, l.mountKey("r"));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
    bool fired = false;
    b->onBeforeWrite(l.mountKey("r"), [&]
    {
        if (std::exchange(fired, true))
            return;
        DB::Cas::tests::tombstoneOwnerForTest(*b, l.ownerKey("r"));
    });
    EXPECT_FALSE(store->tryRemountOnce());
    ASSERT_TRUE(fired);
    const MountLease slot = slotBody(*b, l, "r");
    EXPECT_EQ(slot.writer_epoch, 2u) << "the remount's fresh epoch reached the slot";
    EXPECT_TRUE(slot.gc_fenced) << "and was fenced by the refused recheck";
    EXPECT_FALSE(store->mayMutate());
}

/// Another process of the same uuid rejoined (the life moved) while this one was fenced. The remount sees the
/// moved life before it allocates an epoch and stops for good instead of retrying.
TEST(CASMount, RemountRefusesAfterASameUuidRejoin)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    auto store = Pool::open(b, rootConfig());
    rewriteOwnerAt(*b, l, "r", UInt128(1));   /// life 2, same uuid, not retired
    DB::Cas::tests::fenceOutMountForTest(*b, l.mountKey("r"));
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
    Ops ops(b);
    const String counter_before = ops.op.read(l.epochKey("r"), Retry::standard())->bytes;

    EXPECT_FALSE(store->tryRemountOnce());
    EXPECT_EQ(store->lifecycle(), PoolLifecycle::IdentityLost);
    EXPECT_FALSE(store->tryRemountOnce());

    EXPECT_EQ(ops.op.read(l.epochKey("r"), Retry::standard())->bytes, counter_before) << "no epoch was allocated";
    const MountLease slot = slotBody(*b, l, "r");
    EXPECT_EQ(slot.writer_epoch, 1u);
    EXPECT_TRUE(slot.gc_fenced);
}

namespace
{
void seedOwner(Ops & ops, const Layout & l, const OwnerObject & owner)
{
    mustCommit(ops.op.create(l.ownerKey("r"), encodeOwner(owner), Retry::standard()), "owner");
}

MountLease liveBody(UInt128 uuid, uint64_t epoch)
{
    return MountLease{
        .server_uuid = uuid, .writer_epoch = epoch, .hostname = "h", .pid = 1, .started_at_ms = 1, .seq = 1,
        .expires_at_ms = 100, .min_active_build_sequence = 0, .gc_fenced = false, .write_attempt_id = UInt128(9)};
}
}

/// A refusing recheck rewrites only its own live `(uuid, epoch)` body; every other slot body keeps its bytes.
TEST(CASMount, RecheckSelfFenceLeavesAnyOtherBodyAlone)
{
    MountLease other_epoch = liveBody(UInt128(1), 6);
    MountLease foreign = liveBody(UInt128(2), 7);
    MountLease farewell = liveBody(UInt128(1), 7);
    farewell.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    MountLease fenced = liveBody(UInt128(1), 7);
    fenced.gc_fenced = true;
    const std::vector<std::pair<String, MountLease>> cases{
        {"same uuid, another epoch", other_epoch}, {"foreign uuid", foreign}, {"clean farewell", farewell},
        {"already fenced", fenced}};
    for (const auto & [name, body] : cases)
    {
        SCOPED_TRACE(name);
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        seedOwner(ops, l, OwnerObject{.server_uuid = UInt128(1), .retired_at_ms = std::nullopt, .owner_life = 2,
                                      .retirement_verified = false});
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(body), Retry::standard()), "slot");
        const auto before = ops.op.read(l.mountKey("r"), Retry::standard());
        ASSERT_TRUE(before.has_value());
        DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, "owner moved from life 1 to life 2", [&]
        {
            recheckOwnerAfterClaim(ops.op, l, "r",
                OwnerExpectation{.server_uuid = UInt128(1), .owner_life = 1, .retired = RetiredOwnerPolicy::Refuse}, 7);
        });
        const auto after = ops.op.read(l.mountKey("r"), Retry::standard());
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->bytes, before->bytes);
        EXPECT_EQ(after->etag, before->etag);
    }
}

/// The decommission admin accepts a retired owner, but not a foreign one or one at another life.
TEST(CASMount, RecheckForDecommissionStillRefusesForeignOwnerAndMovedLife)
{
    const OwnerExpectation admin{.server_uuid = UInt128(1), .owner_life = 2, .retired = RetiredOwnerPolicy::AcceptForDecommission};
    struct Case
    {
        String name;
        OwnerObject owner;
        String refusal;
    };
    const std::vector<Case> cases{
        {"retired at the expected life", {.server_uuid = UInt128(1), .retired_at_ms = 5, .owner_life = 2, .retirement_verified = false}, ""},
        {"foreign uuid", {.server_uuid = UInt128(2), .retired_at_ms = 5, .owner_life = 2, .retirement_verified = false}, "different server"},
        {"moved life", {.server_uuid = UInt128(1), .retired_at_ms = 5, .owner_life = 3, .retirement_verified = false}, "owner moved from life 2 to life 3"}};
    for (const Case & c : cases)
    {
        SCOPED_TRACE(c.name);
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        seedOwner(ops, l, c.owner);
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(liveBody(UInt128(1), 7)), Retry::standard()), "slot");
        const auto run = [&] { recheckOwnerAfterClaim(ops.op, l, "r", admin, 7); };
        if (c.refusal.empty())
        {
            EXPECT_NO_THROW(run());
            EXPECT_FALSE(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).gc_fenced);
        }
        else
        {
            DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CORRUPTED_DATA, c.refusal, run);
            EXPECT_TRUE(decodeMountLease(ops.op.read(l.mountKey("r"), Retry::standard())->bytes).gc_fenced);
        }
    }
}

namespace
{
MountLease terminalBody(UInt128 uuid, uint64_t epoch, bool fenced)
{
    MountLease body = liveBody(uuid, epoch);
    if (fenced)
        body.gc_fenced = true;
    else
        body.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    return body;
}

const OwnerExpectation uuid_one_at_life_two{.server_uuid = UInt128(1), .owner_life = 2, .retired = RetiredOwnerPolicy::Refuse};

OwnerObject liveOwner(UInt128 uuid, uint64_t life)
{
    return OwnerObject{.server_uuid = uuid, .retired_at_ms = std::nullopt, .owner_life = life, .retirement_verified = false};
}

/// The slot's body with another `server_uuid`, everything else kept: what a rejoin under a new uuid leaves.
void rewriteSlotUuid(Backend & backend, const Layout & l, const String & srid, UInt128 uuid)
{
    DB::Cas::tests::OperationForTest op(backend);
    const auto got = (*op).read(l.mountKey(srid), Retry::standard());
    ASSERT_TRUE(got.has_value());
    MountLease m = decodeMountLease(got->bytes);
    m.server_uuid = uuid;
    m.seq += 1;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(l.mountKey(srid), encodeMountLease(m), got->etag, Retry::standard())));
}
}

/// After a rejoin under a new uuid the slot can hold another uuid's farewell or self-fence; the server
/// whose owner life it is takes it over.
TEST(CASMount, ForeignTerminalBodyIsReclaimable)
{
    for (const bool fenced : {false, true})
    {
        SCOPED_TRACE(fenced ? "gc_fenced" : "clean farewell");
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        seedOwner(ops, l, liveOwner(UInt128(1), 2));
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(terminalBody(UInt128(2), 7, fenced)), Retry::standard()), "slot");
        const MountClaimResult claim = claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 100, {}, {}, {}, uuid_one_at_life_two);
        ASSERT_EQ(claim.kind, MountClaimResult::Claimed);
        EXPECT_EQ(claim.prior, fenced ? MountPriorState::Fenced : MountPriorState::Clean);
        const MountLease slot = slotBody(*b, l, "r");
        EXPECT_EQ(slot.server_uuid, UInt128(1));
        EXPECT_EQ(slot.writer_epoch, 8u);
    }
}

/// The owner is read before the reclaim: a reclaim that a later recheck refuses would leave an old-uuid
/// body that a 26.6.4 starter (no recheck) could take.
TEST(CASMount, ForeignReclaimReadsTheOwnerFirst)
{
    struct Case
    {
        String name;
        std::optional<OwnerObject> owner;
        std::optional<OwnerExpectation> expected;
    };
    OwnerObject retired = liveOwner(UInt128(1), 2);
    retired.retired_at_ms = 5;
    const std::vector<Case> cases{
        {"owner of another uuid", liveOwner(UInt128(3), 2), uuid_one_at_life_two},
        {"owner retired", retired, uuid_one_at_life_two},
        {"owner at another life", liveOwner(UInt128(1), 3), uuid_one_at_life_two},
        {"owner absent", std::nullopt, uuid_one_at_life_two},
        {"no expectation passed", liveOwner(UInt128(1), 2), std::nullopt}};
    for (const Case & c : cases)
    {
        SCOPED_TRACE(c.name);
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        if (c.owner)
            seedOwner(ops, l, *c.owner);
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(terminalBody(UInt128(2), 7, false)), Retry::standard()), "slot");
        const auto before = ops.op.read(l.mountKey("r"), Retry::standard());
        EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 100, {}, {}, {}, c.expected).kind, MountClaimResult::ForeignOwner);
        EXPECT_EQ(ops.op.read(l.mountKey("r"), Retry::standard())->etag, before->etag);
    }
}

/// A foreign body at or above our epoch is never replaced: a delayed claimer with a lower epoch stays out.
TEST(CASMount, ForeignTerminalBodyAtOrAboveOurEpochStaysForeign)
{
    for (const uint64_t body_epoch : {8, 9})
    {
        SCOPED_TRACE(body_epoch);
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        Ops ops(b);
        seedOwner(ops, l, liveOwner(UInt128(1), 2));
        mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(terminalBody(UInt128(2), body_epoch, false)), Retry::standard()), "slot");
        const auto before = ops.op.read(l.mountKey("r"), Retry::standard());
        EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 100, {}, {}, {}, uuid_one_at_life_two).kind, MountClaimResult::ForeignOwner);
        EXPECT_EQ(ops.op.read(l.mountKey("r"), Retry::standard())->etag, before->etag);
    }
}

/// A live body of another uuid is never taken, whatever the owner says.
TEST(CASMount, ForeignLiveBodyStaysForeignOwner)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    seedOwner(ops, l, liveOwner(UInt128(1), 2));
    mustCommit(ops.op.create(l.mountKey("r"), encodeMountLease(liveBody(UInt128(2), 7)), Retry::standard()), "slot");
    const auto before = ops.op.read(l.mountKey("r"), Retry::standard());
    EXPECT_EQ(claimMount(ops.op, l, "r", UInt128(1), 8, 1000, 100, {}, {}, {}, uuid_one_at_life_two).kind, MountClaimResult::ForeignOwner);
    EXPECT_EQ(ops.op.read(l.mountKey("r"), Retry::standard())->etag, before->etag);
}

/// A start and a self-remount pass their owner expectation, so both take over a terminal body of another uuid.
TEST(CASMount, ClaimPathsReclaimAForeignTerminalBodyOfTheirOwnOwner)
{
    {
        SCOPED_TRACE("start");
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        { auto first = Pool::open(b, rootConfig()); }
        rewriteSlotUuid(*b, l, "r", UInt128(2));
        auto store = Pool::open(b, rootConfig());
        const MountLease slot = slotBody(*b, l, "r");
        EXPECT_EQ(slot.server_uuid, UInt128(1));
        EXPECT_EQ(slot.writer_epoch, store->writerEpoch());
    }
    {
        SCOPED_TRACE("self-remount");
        auto b = std::make_shared<InMemoryBackend>();
        Layout l("p");
        auto store = Pool::open(b, rootConfig());
        DB::Cas::tests::fenceOutMountForTest(*b, l.mountKey("r"));
        DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->renewWatermarkOnce(); });
        rewriteSlotUuid(*b, l, "r", UInt128(2));
        EXPECT_TRUE(store->tryRemountOnce());
        const MountLease slot = slotBody(*b, l, "r");
        EXPECT_EQ(slot.server_uuid, UInt128(1));
        EXPECT_FALSE(slot.gc_fenced);
    }
}

/// A terminal row reads its owner: a tombstone makes it `retired`, and the row carries whether the retirement is
/// verified. A live row never reads its owner (its owner here is undecodable).
TEST(CASListMounts, TerminalSlotOfATombstonedOwnerIsRetired)
{
    const auto state_of = [](Ops & ops, const Layout & l, const String & srid) -> String
    {
        for (const MountInfo & m : listMounts(ops.op, l, /*now_ms=*/0, /*skew_margin_ms=*/0))
            if (m.srid == srid)
                return m.state + (m.retirement_verified ? "+verified" : "");
        return "absent";
    };
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    { auto first = Pool::open(b, rootConfig()); }
    EXPECT_EQ(state_of(ops, l, "r"), "terminated");
    DB::Cas::tests::tombstoneOwnerForTest(*b, l.ownerKey("r"));
    EXPECT_EQ(state_of(ops, l, "r"), "retired") << "an unverified tombstone";
    DB::Cas::tests::tombstoneOwnerForTest(*b, l.ownerKey("r"), /*verified=*/true);
    EXPECT_EQ(state_of(ops, l, "r"), "retired+verified");
    DB::Cas::tests::fenceOutMountForTest(*b, l.mountKey("r"));
    EXPECT_EQ(state_of(ops, l, "r"), "retired+verified") << "a fenced slot of a retired owner";
    rewriteOwnerAt(*b, l, "r", UInt128(2));
    EXPECT_EQ(state_of(ops, l, "r"), "fenced") << "a rejoined owner";

    mustCommit(ops.op.create(l.ownerKey("x"), "garbage-not-a-proto", Retry::standard()), "undecodable owner of x");
    mustCommit(ops.op.create(l.mountKey("x"), encodeMountLease(liveBody(UInt128(1), 4)), Retry::standard()), "live slot of x");
    EXPECT_EQ(state_of(ops, l, "x"), "live") << "a non-terminal row keeps its lease state and never reads its owner";
}

TEST(CASListMounts, TerminalSlotWithAnUndecodableOwnerIsCorrupt)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    Ops ops(b);
    { auto first = Pool::open(b, rootConfig()); }
    DB::Cas::tests::OperationForTest op(*b);
    const auto current = (*op).read(l.ownerKey("r"), Retry::once());
    ASSERT_TRUE(current.has_value());
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(l.ownerKey("r"), "garbage-not-a-proto", current->etag, Retry::once())));
    mustCommit(ops.op.create(l.mountKey("other"), encodeMountLease(liveBody(UInt128(1), 4)), Retry::standard()), "live slot of other");
    std::map<String, String> by_srid;
    for (const MountInfo & m : listMounts(ops.op, l, /*now_ms=*/0, /*skew_margin_ms=*/0))
        by_srid[m.srid] = m.state;
    EXPECT_EQ(by_srid.size(), 2u);
    EXPECT_EQ(by_srid["r"], "corrupt");
    EXPECT_EQ(by_srid["other"], "live");
}
