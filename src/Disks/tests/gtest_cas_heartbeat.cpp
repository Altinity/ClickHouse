#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <limits>

using namespace DB::Cas;

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
