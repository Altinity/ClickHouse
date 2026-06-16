#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <chrono>
#include <limits>
#include <thread>

namespace DB::ErrorCodes
{
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

/// HeartbeatKeeper behavior (spec §4 builds/<build_id> objects, §5 rule W-HEARTBEAT). The heartbeat
/// gates only DEBRIS reclamation by full GC — the publish GATE is the safety mechanism, so these
/// tests pin the writer-side invariants: durable-before-return, strict monotonicity, exact-token
/// discard, and fail-closed (never re-mint) on any foreign touch.

TEST(CasHeartbeat, DurableBeforeReturnAndMonotone)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    HeartbeatKeeper hb(b, layout, build_id, /*server_id*/ hexToU128("00000000000000000000000000000001"));
    hb.start();                                                  /// durable FIRST write (W-HEARTBEAT)
    auto got = b->get(layout.buildHeartbeatKey(u128ToHex(build_id)));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeHeartbeat(got->bytes).heartbeat_seq, 1u);
    EXPECT_EQ(decodeHeartbeat(got->bytes).server_id, hexToU128("00000000000000000000000000000001"));
    hb.renewOnce();
    hb.renewOnce();
    got = b->get(layout.buildHeartbeatKey(u128ToHex(build_id)));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeHeartbeat(got->bytes).heartbeat_seq, 3u);    /// strictly monotone
}

TEST(CasHeartbeat, RenewalsKeepCreatedAtMs)
{
    /// created_at_ms is DIAGNOSTIC ONLY (spec §3.1: no protocol decision reads writer clocks);
    /// renewals reuse the value minted at start, so the object always records build creation time.
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    HeartbeatKeeper hb(b, layout, build_id, hexToU128("00000000000000000000000000000001"));
    hb.start();
    auto got = b->get(layout.buildHeartbeatKey(u128ToHex(build_id)));
    ASSERT_TRUE(got.has_value());
    const uint64_t created = decodeHeartbeat(got->bytes).created_at_ms;
    EXPECT_GT(created, 0u);
    hb.renewOnce();
    got = b->get(layout.buildHeartbeatKey(u128ToHex(build_id)));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeHeartbeat(got->bytes).created_at_ms, created);
}

TEST(CasHeartbeat, DiscardDeletesExactToken)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    const UInt128 server_id = hexToU128("00000000000000000000000000000001");
    const String key = layout.buildHeartbeatKey(u128ToHex(build_id));

    HeartbeatKeeper hb(b, layout, build_id, server_id);
    hb.start();
    EXPECT_TRUE(b->head(key).exists);
    hb.discard();
    EXPECT_FALSE(b->head(key).exists);                           /// the key is fully released

    /// The keeper is dead after discard: renew and a second discard are programming errors.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { hb.renewOnce(); });
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { hb.discard(); });

    /// A SECOND keeper with the same build id can start after discard — the key is absent, so
    /// putIfAbsent succeeds again. This is fine (build ids are globally unique by construction);
    /// the point is that discard fully releases the key.
    HeartbeatKeeper hb2(b, layout, build_id, server_id);
    hb2.start();
    EXPECT_TRUE(b->head(key).exists);

    /// start while the key EXISTS must throw — a heartbeat key collision means the world is broken.
    HeartbeatKeeper hb3(b, layout, build_id, server_id);
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { hb3.start(); });
}

TEST(CasHeartbeat, DestructorDoesNotDiscard)
{
    /// Destruction without discard is the crash path: the heartbeat object must persist so full GC
    /// can apply the debris rules once it stops advancing (spec §5 wedged-heartbeat trace).
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    const String key = layout.buildHeartbeatKey(u128ToHex(build_id));
    {
        HeartbeatKeeper hb(b, layout, build_id, hexToU128("00000000000000000000000000000001"));
        hb.start();
    }
    EXPECT_TRUE(b->head(key).exists);
}

TEST(CasHeartbeat, ForeignOverwriteFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    HeartbeatKeeper hb(b, layout, build_id, hexToU128("00000000000000000000000000000001"));
    hb.start();

    /// Simulate a foreign writer replacing the object: observe the current token via head, then
    /// overwrite with arbitrary bytes. Nobody else may touch our key — renew must fail closed
    /// (LOGICAL_ERROR) and never re-mint.
    const String key = layout.buildHeartbeatKey(u128ToHex(build_id));
    auto head = b->head(key);
    ASSERT_TRUE(head.exists);
    ASSERT_EQ(b->putOverwrite(key, "foreign", head.token), PutOutcome::Done);

    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { hb.renewOnce(); });
}

TEST(CasHeartbeat, BackgroundThreadRenews)
{
    /// ONE smoke assertion for the renewal thread; the renewal logic itself is driven directly by
    /// the tests above. Bounded-deadline polling, not a fixed sleep: wait until at least one
    /// background renewal is observable through lastRenewTime, with a generous deadline.
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    const UInt128 build_id = hexToU128("0123456789abcdef0123456789abcdef");
    HeartbeatKeeper hb(b, layout, build_id, hexToU128("00000000000000000000000000000001"));
    hb.start();
    const auto started_at = hb.lastRenewTime();

    hb.startBackground(std::chrono::milliseconds(10));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (hb.lastRenewTime() == started_at && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    hb.stopBackground();
    hb.stopBackground();                                         /// idempotent

    EXPECT_GT(hb.lastRenewTime(), started_at);
    auto got = b->get(layout.buildHeartbeatKey(u128ToHex(build_id)));
    ASSERT_TRUE(got.has_value());
    EXPECT_GE(decodeHeartbeat(got->bytes).heartbeat_seq, 2u);    /// at least one background renewal
}

/// WatermarkKeeper behavior (spec 2026-06-16-ca-build-watermark, rule W-ANCHOR). The per-server
/// watermark is durable before return, claims a slot that may already exist from a prior process
/// incarnation, bumps seq on renewal, and retires the epoch (min_active = UINT64_MAX) on farewell.

TEST(CasWatermarkKeeper, AnchorIsDurableThenRenewBumpsSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const UInt128 server_id(0x1234);
    uint64_t min_active_now = 5;
    WatermarkKeeper keeper(backend, layout, server_id, /*epoch=*/9,
                           [&]{ return min_active_now; });
    keeper.start();                                  // anchor durable
    auto hr = backend->head(layout.serverWatermarkKey(u128ToHex(server_id)));
    ASSERT_TRUE(hr.exists);
    auto w = decodeServerWatermark(backend->get(layout.serverWatermarkKey(u128ToHex(server_id)))->bytes);
    ASSERT_EQ(w.epoch, 9u); ASSERT_EQ(w.min_active, 5u); ASSERT_EQ(w.seq, 1u);

    min_active_now = 8;
    keeper.renewOnce();
    w = decodeServerWatermark(backend->get(layout.serverWatermarkKey(u128ToHex(server_id)))->bytes);
    ASSERT_EQ(w.min_active, 8u); ASSERT_EQ(w.seq, 2u);
}

TEST(CasWatermarkKeeper, FarewellRetiresEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    WatermarkKeeper keeper(backend, layout, UInt128(0x1234), 9, []{ return 5; });
    keeper.start();
    keeper.farewell();
    auto w = decodeServerWatermark(backend->get(layout.serverWatermarkKey(u128ToHex(UInt128(0x1234))))->bytes);
    ASSERT_EQ(w.min_active, std::numeric_limits<uint64_t>::max());
}
