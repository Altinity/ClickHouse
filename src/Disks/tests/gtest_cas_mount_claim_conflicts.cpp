#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

using namespace DB::Cas;
using DB::Cas::tests::MountSlotRaceBackend;
using DB::Cas::tests::expectThrowsCodeWithMessage;

namespace
{

/// The two request planes the keeper runs on. Both are open-fence: what these tests exercise is the
/// mount protocol's own exclusivity, not a fence's, and a race hook that fires from inside the engine
/// admits its own operation rather than re-entering the one already in flight.
class Ops
{
public:
    explicit Ops(std::shared_ptr<Backend> backend)
        : mount(openRequestsForTest(backend)), farewell(openRequestsForTest(std::move(backend))), op(mount.admit())
    {
    }

    Ops(const Ops &) = delete;
    Ops & operator=(const Ops &) = delete;

    CasRequests mount;
    CasRequests farewell;
    CasOperation op;
};

/// One keeper for the mount slot of server-root "r", under (uuid=1, epoch=7) unless overridden.
MountLeaseKeeper makeKeeper(
    Ops & ops,
    uint64_t & now,
    DB::UInt128 uuid = DB::UInt128(1),
    uint64_t epoch = 7)
{
    return MountLeaseKeeper(
        ops.mount,
        ops.farewell,
        Layout("p"),
        "r",
        uuid,
        epoch,
        std::chrono::milliseconds(100),
        [&now] { return now; },
        [] { return uint64_t{0}; });
}

void markMountGcFenced(CasOperation & op, const Layout & layout, const String & server_root_id)
{
    const String key = layout.mountKey(server_root_id);
    const auto got = op.read(key, Retry::standard());
    ASSERT_TRUE(got);
    MountLease lease = decodeMountLease(got->bytes);
    lease.gc_fenced = true;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        op.replace(key, encodeMountLease(lease), got->incarnation, Retry::standard())));
}

}

TEST(CASMountClaimConflicts, SlotAppearedBetweenTheReadAndTheCreate)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    /// Absent at the read; another process mints it before our create lands.
    backend->before_put_if_absent = [&]
    {
        CasOperation racer = ops.mount.admit();
        claimMount(racer, layout, "r", DB::UInt128(2), 1, now, /*ttl_ms=*/100);
    };
    auto keeper = makeKeeper(ops, now);
    expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        "appeared between the read and the create",
        [&] { keeper.start(); });
}

TEST(CASMountClaimConflicts, SlotHeldByForeignServer)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(2), 1, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    auto keeper = makeKeeper(ops, now);
    expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        "held by a foreign server",
        [&] { keeper.start(); });
}

TEST(CASMountClaimConflicts, SlotHeldByDifferentWriterEpoch)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(1), 7, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    auto keeper = makeKeeper(ops, now, DB::UInt128(1), /*epoch=*/8);
    expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        "held by a different writer_epoch",
        [&] { keeper.start(); });
}

TEST(CASMountClaimConflicts, SlotChangedInsideAdoptionWindow)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(1), 7, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    /// Rewrite the slot under a NEW incarnation after our read, so our adoption write is refused.
    backend->before_put_overwrite = [&]
    {
        CasOperation racer = ops.mount.admit();
        claimMount(racer, layout, "r", DB::UInt128(1), 7, now + 1, /*ttl_ms=*/100);
    };
    auto keeper = makeKeeper(ops, now);
    expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        "changed while adopting our own mount slot",
        [&] { keeper.start(); });
}

TEST(CASMountClaimConflicts, SlotVanishedInsideAdoptionWindow)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(1), 7, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    backend->before_put_overwrite = [&]
    {
        CasOperation racer = ops.mount.admit();
        ASSERT_EQ(racer.removeCurrent(layout.mountKey("r"), Retry::standard()), Removal::Removed);
    };
    auto keeper = makeKeeper(ops, now);
    expectThrowsCodeWithMessage(
        DB::ErrorCodes::ABORTED,
        "vanished while adopting our own mount slot",
        [&] { keeper.start(); });
}

/// The two fenced branches keep their own type, and keep PRECEDENCE over the conflicts above: the
/// mount-open loop catches `MountFencedException` by type and recovers with a fresh writer epoch, so
/// a fence reported as a plain conflict would turn a recoverable state into a failed mount.
TEST(CASMountClaimConflicts, FencedBeforeAdoptionRaisesMountFenced)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(1), 7, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    markMountGcFenced(ops.op, layout, "r");
    auto keeper = makeKeeper(ops, now);
    EXPECT_THROW(keeper.start(), MountFencedException);
}

TEST(CASMountClaimConflicts, FencedInsideAdoptionWindowRaisesMountFencedNotAborted)
{
    auto backend = std::make_shared<MountSlotRaceBackend>();
    Layout layout("p");
    uint64_t now = 1000;
    Ops ops(backend);
    ASSERT_EQ(
        claimMount(ops.op, layout, "r", DB::UInt128(1), 7, now, /*ttl_ms=*/100).kind,
        MountClaimResult::Claimed);
    /// The slot changes inside the adoption window AND the new body is fenced: the fenced branch must
    /// win over the "changed while adopting" one.
    backend->before_put_overwrite = [&]
    {
        CasOperation racer = ops.mount.admit();
        markMountGcFenced(racer, layout, "r");
    };
    auto keeper = makeKeeper(ops, now);
    EXPECT_THROW(keeper.start(), MountFencedException);
}
