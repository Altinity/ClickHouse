#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <limits>

using namespace DB::Cas;


/// MountLeaseKeeper behavior after the ack-floor merge (spec 2026-07-02-cas-gc-ack-floor-fence):
/// the per-server mount lease and the merged build-watermark floor + GC-round acknowledgement all
/// ride the SAME slot, renewed by one beat. The keeper anchors durably before return, adopts a slot
/// already written by `claimMount` (same uuid+epoch), re-reads BOTH callbacks on each renew and bumps
/// `seq`, stamps the farewell sentinel (`min_active = UINT64_MAX`, `expires_at_ms <= now`) on `stop`,
/// and fails closed on any foreign touch (`renewOnce` throws).

namespace
{
/// The normal steady-state flow: `claimMount` writes the live (uuid, epoch) mount, THEN the keeper
/// adopts it. Seed that claim so `start` adopts instead of self-tripping the double-start guard.
void seedOwnClaim(Backend & b, const Layout & l, const String & srid, UInt128 uuid, uint64_t epoch,
                  uint64_t now_ms, uint64_t ttl_ms)
{
    ASSERT_EQ(claimMount(b, l, srid, uuid, epoch, now_ms, ttl_ms).kind, MountClaimResult::Claimed);
}
}

TEST(CasHeartbeat, AnchorCarriesFloorAndAck)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t min_active_now = 5;
    uint64_t observed_round_now = 9;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [&] { return min_active_now; }, [&] { return observed_round_now; });
    keeper.start();

    auto hr = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(hr.exists);
    auto m = decodeMountLease(backend->get(layout.mountKey(srid))->bytes);
    EXPECT_EQ(m.writer_epoch, 9u);
    EXPECT_EQ(m.min_active, 5u);
    EXPECT_EQ(m.observed_gc_round, 9u);
    EXPECT_EQ(m.seq, 1u);
    EXPECT_FALSE(m.gc_fenced);
}

TEST(CasHeartbeat, RenewRereadsBothCallbacksAndBumpsSeq)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    uint64_t min_active_now = 5;
    uint64_t observed_round_now = 9;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [&] { return min_active_now; }, [&] { return observed_round_now; });
    keeper.start();

    /// Both dynamic fields move; the renewal re-reads both off the callbacks and bumps seq.
    now_ms = 1500;
    min_active_now = 8;
    observed_round_now = 12;
    keeper.renewOnce();

    auto m = decodeMountLease(backend->get(layout.mountKey(srid))->bytes);
    EXPECT_EQ(m.min_active, 8u);
    EXPECT_EQ(m.observed_gc_round, 12u);
    EXPECT_EQ(m.seq, 2u);
    EXPECT_EQ(m.expires_at_ms, 1500u + 100u);
}

TEST(CasHeartbeat, StopStampsExpiredAndFarewellSentinel)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; });
    keeper.start();

    now_ms = 2000;
    keeper.stop();

    auto m = decodeMountLease(backend->get(layout.mountKey(srid))->bytes);
    /// Terminal body stamps the lease already-expired (so a same-server reopen reclaims immediately)
    /// AND folds the watermark farewell into it (min_active = UINT64_MAX).
    EXPECT_LE(m.expires_at_ms, now_ms);
    EXPECT_EQ(m.min_active, std::numeric_limits<uint64_t>::max());
}

TEST(CasHeartbeat, ForeignTouchMakesRenewThrow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; });
    keeper.start();

    /// A foreign incarnation overwrites the slot: the single-writer contract fails closed on renew.
    const HeadResult h = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(h.exists);
    MountLease foreign;
    foreign.server_uuid = uuid;
    foreign.writer_epoch = 9;
    foreign.seq = 99;
    backend->putOverwrite(layout.mountKey(srid), encodeMountLease(foreign), h.token);
    EXPECT_ANY_THROW(keeper.renewOnce());
}

/// Mount-slot writer audit (the P1 "foreign writer" instrument): every mount-slot WRITE and every
/// OBSERVED foreign/conflicting body becomes an event, carrying the conflicting body's identity —
/// the payload the chronic "touched by a foreign writer" collisions need to be diagnosable.
TEST(CasMountAudit, ClaimReleaseAndForeignConflictEmitEvents)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };

    const uint64_t now_ms = 1'000'000;
    /// mint for uuid 1 -> one mount_claim
    ASSERT_EQ(claimMount(*backend, layout, "a", UInt128{1}, 1, now_ms, /*ttl_ms=*/10'000, sink).kind,
              MountClaimResult::Claimed);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountClaim);
    EXPECT_EQ(seen[0].detail.at("srid"), "a");
    EXPECT_EQ(seen[0].detail.at("branch"), "mint");

    /// a FOREIGN uuid claiming a live slot -> mount_conflict carrying the current holder's identity
    seen.clear();
    (void)claimMount(*backend, layout, "a", UInt128{2}, 1, now_ms, /*ttl_ms=*/10'000, sink);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("srid"), "a");
    /// The conflict must carry the ORIGINAL holder's identity (uuid 1, the minter) — not the
    /// foreign claimer's (uuid 2).
    EXPECT_EQ(seen.back().detail.at("holder_uuid"), u128ToHex(UInt128{1}));
    EXPECT_NE(seen.back().detail.at("holder_uuid"), u128ToHex(UInt128{2}));
}

/// The MountLeaseKeeper wiring: `start` adopting an already-claimed slot emits mount_claim, `stop`
/// (the farewell write) emits mount_release.
TEST(CasMountAudit, KeeperAdoptEmitsClaimAndTerminateEmitsRelease)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; }, sink);
    keeper.start();

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountClaim);
    EXPECT_EQ(seen[0].detail.at("branch"), "adopt");

    seen.clear();
    now_ms = 2000;
    keeper.stop();

    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountRelease);
    EXPECT_EQ(seen[0].detail.at("branch"), "farewell");
}

/// Keeper-level foreign-conflict refusal: the mount slot is already held by a FOREIGN uuid (X) when
/// a keeper for a DIFFERENT uuid (Y) tries to claim it. This must fail closed, emit a MountConflict
/// carrying X's identity, AND — since the mount-audit sink is not yet installed at first-open — name
/// X in the thrown exception's message text (the only identity carrier in err.log at that point).
TEST(CasMountAudit, KeeperForeignConflictRefusesAndNamesHolder)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid_x(0x1111);
    const UInt128 uuid_y(0x2222);
    uint64_t now_ms = 1000;

    /// Foreign holder X claims the slot first.
    ASSERT_EQ(claimMount(*backend, layout, srid, uuid_x, /*epoch=*/1, now_ms, /*ttl_ms=*/100).kind,
              MountClaimResult::Claimed);

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    MountLeaseKeeper keeper(backend, layout, srid, uuid_y, /*writer_epoch=*/1, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; }, sink);

    bool threw = false;
    try
    {
        keeper.start();
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        /// The enriched refusal message must name the OBSERVED holder (X), not the caller (Y).
        EXPECT_NE(e.message().find(u128ToHex(uuid_x)), String::npos) << e.message();
    }
    EXPECT_TRUE(threw);

    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("holder_uuid"), u128ToHex(uuid_x));
}

/// `Store::open` can fail before/inside `doStart` (e.g. a foreign-conflict refusal, see
/// `KeeperForeignConflictRefusesAndNamesHolder` above) — the keeper is destroyed without ever having
/// claimed anything. Teardown must not throw "release before start"; there is nothing to release. A
/// stop AFTER a successful start still performs the farewell (covered by
/// `StopStampsExpiredAndFarewellSentinel` above); a genuinely-started DOUBLE terminate stays loud.
TEST(CasHeartbeat, StopBeforeStartIsQuietNoOp)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    uint64_t now_ms = 1000;
    MountLeaseKeeper keeper(backend, layout, "a", UInt128{1}, /*writer_epoch=*/1, std::chrono::milliseconds(10'000),
                            [&] { return now_ms; }, [] { return uint64_t{0}; }, [] { return uint64_t{0}; });
    /// start() never called.
    EXPECT_NO_THROW(keeper.stop());
    EXPECT_NO_THROW(keeper.stop());
}

/// "A fence costs an epoch" at the keeper layer: the GC fenced our fresh lease before we adopted it
/// (the lease expired mid-open — e.g. a slow first beat). This must fail closed with a TYPED,
/// recoverable `MountFencedException`, distinct from the generic "touched by a foreign writer"
/// `LOGICAL_ERROR` — the open path (Task 4) tells "re-open with a fresh epoch" apart from "fail hard"
/// by this code, not by parsing message text.
TEST(CasMountAudit, KeeperAdoptRefusesFencedSelfWithTypedError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;

    /// mint (uuid, epoch 9), then fence it in place (what computeHeartbeatFloor does on expiry):
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);
    {
        auto got = backend->get(layout.mountKey(srid));
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        ASSERT_EQ(backend->putOverwrite(layout.mountKey(srid), encodeMountLease(fenced), got->token).outcome,
                  PutOutcome::Done);
    }

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    /// A keeper for the SAME (uuid, epoch) tries to adopt the now-fenced slot.
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; }, sink);

    bool threw = false;
    try
    {
        keeper.start();
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
/// GC's fence-out). The keeper must re-read and recognize this as its OWN incarnation being fenced —
/// a recoverable `MountFencedException`, not the generic single-writer-violation text.
TEST(CasHeartbeat, RenewOverFencedOwnSlotIsClassifiedNotForeign)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; }, [] { return uint64_t{9}; }, sink);
    keeper.start();
    seen.clear();

    /// Mid-run: the GC fences our own (uuid, epoch) mount slot in place (as `computeHeartbeatFloor`
    /// does on an expired lease), preserving the whole body — a token-guarded putOverwrite, exactly
    /// as the GC's own fence-out does it.
    {
        const auto got = backend->get(layout.mountKey(srid));
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        ASSERT_EQ(backend->putOverwrite(layout.mountKey(srid), encodeMountLease(fenced), got->token).outcome,
                  PutOutcome::Done);
    }

    /// The renewal must classify the fence honestly — not "foreign writer":
    try
    {
        keeper.renewOnce();
        FAIL() << "renewOnce over a fenced slot must throw";
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
