#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

#include <chrono>
#include <limits>
#include <map>
#include <string>

using namespace DB::Cas;

TEST(CasServerRootId, ValidationAcceptsCleanPathsRejectsBad)
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

TEST(CasServerRoot, KeysAndCodecsRoundTrip)
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

TEST(CasServerRootClaim, OwnerStickyAndForeignFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    EXPECT_NO_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(1)));     // fresh empty root → claim
    EXPECT_NO_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(1)));     // same uuid → ok
    EXPECT_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(2)), DB::Exception);  // foreign → fail closed
}

TEST(CasServerRootEpoch, AllocatorIsMonotoneAndSurvivesMountConcept)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("r");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    const uint64_t e1 = allocateWriterEpoch(*b, l, "r");
    const uint64_t e2 = allocateWriterEpoch(*b, l, "r");
    EXPECT_GE(e1, 1u);                                             // 0 is a reserved sentinel
    EXPECT_GT(e2, e1);                                             // strictly increasing

    /// Deleting the (separate) mount object must NOT reset the epoch. No mount has been written in
    /// Task 4, so deleteExact of a non-existent mount is a NotFound no-op that touches nothing.
    const auto del = b->deleteExact(l.mountKey("r"), b->head(l.mountKey("r")).token);
    EXPECT_EQ(del.kind, DeleteOutcome::Kind::NotFound);
    EXPECT_GT(allocateWriterEpoch(*b, l, "r"), e2);
}

TEST(CasServerRootClaim, MissingOwnerOverNonEmptyRootIsCorrupted)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// Simulate existing data without an owner (identity lost): plant a key under roots/<srid>/.
    b->putIfAbsent(l.serverRootDataPrefix("r") + "some-data", "x");
    EXPECT_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(1)), DB::Exception);
}

TEST(CasMountLease, AbsentClaimThenRenewBumpsSeq)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    auto r = claimMount(*b, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    MountLeaseKeeper k(b, l, "r", UInt128(1), 7, std::chrono::milliseconds(100), [&] { return now; },
                       [] { return uint64_t{0}; });
    k.start();
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).seq, 1u);
    k.renewOnce();
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).seq, 2u);
}

TEST(CasMountLease, SameUuidLiveFailsForeignFailsExpiredReclaims)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100);    // A live until 1100
    // same uuid, lease still live → double-start guard:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(1), 8, 1050, 100).kind, MountClaimResult::LiveDoubleStart);
    // foreign uuid, even after expiry → fail closed:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(2), 1, 1200, 100).kind, MountClaimResult::ForeignOwner);
    // same uuid after expiry → reclaim:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(1), 9, 1200, 100).kind, MountClaimResult::Claimed);
}

TEST(CasMountMessage, DoubleStartTextHasIdentityAndRemediation)
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
    EXPECT_NE(msg.find("server_root_id"), std::string::npos);
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
    /// Clock-skew caveat + manual mount-object delete escape hatch.
    EXPECT_NE(msg.find("CLOCK SKEW"), std::string::npos);
    EXPECT_NE(msg.find("NTP"), std::string::npos);
    EXPECT_NE(msg.find("manually delete the mount"), std::string::npos);
    EXPECT_NE(msg.find("gc/server-roots/replica-a/mount"), std::string::npos);
}

TEST(CasMountAwaitExpiry, PastExpiryReclaimsImmediatelyNoSleep)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A prior incarnation (uuid=1, epoch=7) claimed a lease live until 1100.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1200;                 // already past 1100 → the stale lease is dead
    int sleeps = 0;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 25, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(sleeps, 0);                                             // decided on the first attempt
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 8u);   // reclaimed as us
}

TEST(CasMountAwaitExpiry, FutureExpiryReclaimsAfterClockAdvances)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;                 // lease live until 1100, holder does NOT renew
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 50, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    const auto body = decodeMountLease(b->get(l.mountKey("r"))->bytes);
    EXPECT_EQ(body.writer_epoch, 8u);
    EXPECT_EQ(body.seq, 2u);                                         // reclaim continues seq (prev 1 + 1)
}

TEST(CasMountAwaitExpiry, LiveRenewingTwinTimesOutAsDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    auto now_fn = [&] { return now; };
    /// Each poll: time advances AND the live holder (uuid=1, epoch=7) renews its own lease.
    auto sleep_fn = [&](uint64_t ms)
    {
        now += ms;
        ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, now, 100).kind, MountClaimResult::Claimed);
    };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 20, /*margin*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);   // still the holder's
}

TEST(CasMountAwaitExpiry, ForeignUuidFailsClosedImmediately)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A foreign server (uuid=2) holds the mount.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(2), 1, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    int sleeps = 0;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 25, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::ForeignOwner);
    EXPECT_EQ(sleeps, 0);                                            // never waits across UUIDs
}

TEST(CasMountAwaitExpiry, SkewedFarFutureExpiryIsCappedAtTtlPlusMargin)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A prior incarnation stamped a far-future expiry (killer clock ahead): live until 1000 + 100000,
    /// but the holder is dead (never renews). The wait must be capped at ~ttl + margin, not block to
    /// the absurd expiry, and fail closed (LiveDoubleStart) rather than reclaim a still-live-looking lease.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100000).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 20, /*margin*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_LE(now, 1000u + 100u + 20u + 20u);                        // bounded ~ start + ttl + margin (+ one poll)
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);   // not reclaimed
}

TEST(CasMountLease, KeeperStartAdoptsOurOwnClaimNotDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    uint64_t now = 1000;
    // The normal flow: claimMount writes the live mount under (uuid=1, epoch=7), THEN keeper.start().
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), /*epoch*/ 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    MountLeaseKeeper k(b, l, "r", UInt128(1), /*epoch*/ 7, std::chrono::milliseconds(100), [&] { return now; },
                       [] { return uint64_t{0}; });
    EXPECT_NO_THROW(k.start());     // adopts our own live (uuid=1,epoch=7) mount — NOT a double-start
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);

    // A keeper for the SAME uuid but a DIFFERENT live epoch must fail closed (superseded/double-start):
    MountLeaseKeeper k2(b, l, "r", UInt128(1), /*epoch*/ 8, std::chrono::milliseconds(100), [&] { return now; },
                        [] { return uint64_t{0}; });
    EXPECT_ANY_THROW(k2.start());
}

TEST(CasMountFence, SupersededWriterRefusedNoS3Read)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto store = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "r", .root_shards = 1});

    /// Permissive default: a Store that has NOT armed the fence allows mutations.
    EXPECT_TRUE(store->mayMutate());

    /// Latching loss: once the renewer trips the fence it stays lost (purely local — no S3 read).
    store->tripMountLost();
    EXPECT_FALSE(store->mayMutate());

    /// A real mutate entrypoint that funnels through mutateShard now fails closed at the gate, BEFORE
    /// the mutate lambda runs (so this is the ABORTED gate throw, not a FILE_DOESNT_EXIST from inside).
    const RootNamespace ns{"srv1/tbl"};
    EXPECT_THROW(store->dropRef(ns, "any_ref"), DB::Exception);
}

TEST(CasMountStartup, SecondServerSameRootFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1});
    /// A second server (different uuid) on the SAME server_root_id + same backend → fail closed
    /// (the owner gate rejects the foreign uuid before any mount/epoch mutation).
    EXPECT_THROW(
        Store::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "r", .root_shards = 1}),
        DB::Exception);
}

TEST(CasMountStartup, WriterEpochStrictlyIncreasesAcrossReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1});
    const uint64_t e1 = s1->writerEpoch();

    /// Simulate shutdown: the Store dtor stops the keeper, whose terminate() retires the lease
    /// (stamps it already-expired). The owner + the durable epoch object stay sticky.
    s1.reset();

    /// Same server reopen → reclaims the (now-expired, different-epoch) mount and allocates a strictly
    /// higher durable writer_epoch.
    auto s2 = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1});
    const uint64_t e2 = s2->writerEpoch();
    EXPECT_GT(e2, e1);
}

TEST(CasMountReadOnly, ForeignOwnedPoolOpensWithoutMutation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    /// Server A claims the pool (writable): owner = uuid(1), a durable epoch + a live mount lease.
    auto a = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1});

    /// Capture the control objects BEFORE the read-only open so we can prove it mutated nothing.
    const auto owner_before = b->get(l.ownerKey("r"));
    const auto mount_before = b->get(l.mountKey("r"));
    const auto epoch_before = b->get(l.epochKey("r"));
    ASSERT_TRUE(owner_before.has_value());
    ASSERT_TRUE(mount_before.has_value());
    ASSERT_TRUE(epoch_before.has_value());

    /// A READ-ONLY observer with a DIFFERENT server_id on the SAME backend/server_root_id must NOT
    /// throw — a read-only mount never participates in the owner/epoch/mount protocol, so a pool
    /// owned by another server_uuid is freely observable.
    StorePtr ro;
    EXPECT_NO_THROW(
        ro = Store::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "r",
            .root_shards = 1, .read_only = true}));
    EXPECT_NE(ro, nullptr);

    /// And it mutated nothing: owner still decodes to A's uuid, the mount body is still A's, and the
    /// raw bytes of owner/epoch/mount are byte-for-byte unchanged (no second owner, no re-claim).
    const auto owner_after = b->get(l.ownerKey("r"));
    const auto mount_after = b->get(l.mountKey("r"));
    const auto epoch_after = b->get(l.epochKey("r"));
    ASSERT_TRUE(owner_after.has_value());
    ASSERT_TRUE(mount_after.has_value());
    ASSERT_TRUE(epoch_after.has_value());

    EXPECT_EQ(decodeOwner(owner_after->bytes).server_uuid, UInt128(1));
    EXPECT_EQ(decodeMountLease(mount_after->bytes).server_uuid, UInt128(1));

    EXPECT_EQ(owner_after->bytes, owner_before->bytes);
    EXPECT_EQ(mount_after->bytes, mount_before->bytes);
    EXPECT_EQ(epoch_after->bytes, epoch_before->bytes);
}

TEST(CasMountStartup, StaleSelfMountReclaimedAfterWait)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Server A opens writable with a SHORT lease TTL and KEEPS its Store alive with NO background
    /// renewer (background_watermark defaults false) — i.e. it simulates a crashed process: the mount
    /// lease survives with a future expires_at_ms but is never renewed.
    /// This test's short lease TTL is far below the CasRequestBudget defaults (RFC
    /// cas-s3-timeout-retry-control §required-timeout-model requires attempt_timeout + safety_margin <
    /// lease TTL), so it also scales down cas_request_budget to fit — the budget itself is not
    /// exercised here, only Store::open's validateCasRequestBudget startup gate.
    const CasRequestBudget tiny_budget{
        .attempt_timeout_ms = 50, .operation_deadline_ms = 50, .max_attempts = 1, .lease_safety_margin_ms = 50};
    auto a = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100),
        .cas_request_budget = tiny_budget});
    ASSERT_NE(a, nullptr);
    const uint64_t e1 = a->writerEpoch();

    /// A restart of the SAME server (same uuid) must NOT abort: it waits out the stale lease (<= ~300ms)
    /// and reclaims the mount, coming up with a strictly higher durable writer_epoch.
    StorePtr a2;
    EXPECT_NO_THROW(
        a2 = Store::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
            .mount_lease_ttl_ms = std::chrono::milliseconds(300),
            .mount_renew_period = std::chrono::milliseconds(100),
            .cas_request_budget = tiny_budget}));
    ASSERT_NE(a2, nullptr);
    EXPECT_GT(a2->writerEpoch(), e1);
}

TEST(CasMountLease, BodyCarriesFloorAndFence)
{
    MountLease m;
    m.server_uuid = UInt128(0xAB);
    m.writer_epoch = 7;
    m.hostname = "h";
    m.pid = 42;
    m.started_at_ms = 1000;
    m.seq = 3;
    m.expires_at_ms = 2000;
    m.min_active = 5;
    m.gc_fenced = true;
    const MountLease d = decodeMountLease(encodeMountLease(m));
    EXPECT_EQ(d.min_active, 5u);
    EXPECT_TRUE(d.gc_fenced);
    EXPECT_EQ(d.writer_epoch, 7u);
}

TEST(CasMountLease, RetiredSentinelRoundTrips)
{
    MountLease m;
    m.min_active = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(decodeMountLease(encodeMountLease(m)).min_active,
              std::numeric_limits<uint64_t>::max());
}

/// ---- Task 7: GC heartbeat classification with token-guarded fence-out ----

namespace
{
/// A fixed, fake "now" — no real clocks in these tests. Lease timestamps are chosen relative to it.
constexpr uint64_t kNowMs = 1'000'000;
constexpr uint64_t kSkewMarginMs = 5'000;

/// Seed one mount body under mountKey(srid) via the on-storage codec (`encodeMountLease` +
/// `putIfAbsent`) — the same interface the keeper writes through.
MountLease seedMount(
    Backend & b, const Layout & l, const String & srid,
    uint64_t expires_at_ms, bool gc_fenced, uint64_t min_active, uint64_t seq = 1)
{
    MountLease m;
    m.server_uuid = UInt128(srid.back());   // distinct per srid; content is irrelevant to the gate
    m.writer_epoch = 1;
    m.hostname = "h-" + srid;
    m.pid = 100;
    m.started_at_ms = kNowMs;
    m.seq = seq;
    m.expires_at_ms = expires_at_ms;
    m.min_active = min_active;
    m.gc_fenced = gc_fenced;
    b.putIfAbsent(l.mountKey(srid), encodeMountLease(m));
    return m;
}
}

TEST(CasHeartbeatFloor, ClassifiesAndFencesOut)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    /// two live mounts — both count into `live`.
    seedMount(*b, l, "s1", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active*/ 0);
    seedMount(*b, l, "s2", /*expires*/ kNowMs + 60'000, /*fenced*/ false, /*min_active*/ 0);
    /// expired-unfenced — must be fenced-out by the call.
    seedMount(*b, l, "s3", /*expires*/ kNowMs - 60'000, /*fenced*/ false, /*min_active*/ 0);
    /// already-fenced — excluded, body byte-identical after the call (no PUT).
    seedMount(*b, l, "s4", /*expires*/ kNowMs - 60'000, /*fenced*/ true, /*min_active*/ 0);
    /// terminated (min_active == UINT64_MAX) with expired-looking timestamps — excluded, not fenced.
    seedMount(*b, l, "s5", /*expires*/ kNowMs - 60'000, /*fenced*/ false,
              /*min_active*/ std::numeric_limits<uint64_t>::max());

    const auto s3_before = b->get(l.mountKey("s3"));
    const auto s4_before = b->get(l.mountKey("s4"));
    ASSERT_TRUE(s3_before.has_value());
    ASSERT_TRUE(s4_before.has_value());

    const HeartbeatFloor floor = computeHeartbeatFloor(*b, l, kNowMs, kSkewMarginMs);

    EXPECT_EQ(floor.live, 2u);
    EXPECT_EQ(floor.terminated, 1u);
    EXPECT_EQ(floor.fenced_now, 1u);
    EXPECT_EQ(floor.already_fenced, 1u);

    /// The expired-unfenced body was fenced: gc_fenced set, seq bumped, the rest of the body preserved.
    const auto s3_after = b->get(l.mountKey("s3"));
    ASSERT_TRUE(s3_after.has_value());
    const MountLease s3_prev = decodeMountLease(s3_before->bytes);
    const MountLease s3_now = decodeMountLease(s3_after->bytes);
    EXPECT_TRUE(s3_now.gc_fenced);
    EXPECT_EQ(s3_now.seq, s3_prev.seq + 1);
    EXPECT_EQ(s3_now.server_uuid, s3_prev.server_uuid);
    EXPECT_EQ(s3_now.writer_epoch, s3_prev.writer_epoch);
    EXPECT_EQ(s3_now.hostname, s3_prev.hostname);
    EXPECT_EQ(s3_now.expires_at_ms, s3_prev.expires_at_ms);

    /// The already-fenced body was not touched (no PUT).
    const auto s4_after = b->get(l.mountKey("s4"));
    ASSERT_TRUE(s4_after.has_value());
    EXPECT_EQ(s4_after->bytes, s4_before->bytes);
}

namespace
{
/// A delegating backend whose `putOverwrite` of the target mount key first performs an inner renewal
/// (a real, token-correct overwrite that pushes expiry far into the future) and THEN delegates — so
/// the caller's fence-out overwrite lands on a stale token and returns PreconditionFailed. The inner
/// renewal runs exactly once (`renewed`), modelling a holder that renews concurrently in the window
/// between the function's GET and its fence-out PUT.
class RenewOnFenceBackend : public InMemoryBackend
{
public:
    RenewOnFenceBackend(String target_key_, uint64_t renewed_expires_ms_)
        : target_key(std::move(target_key_)), renewed_expires_ms(renewed_expires_ms_)
    {
    }

    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                           const ObjectMeta & meta = {}) override
    {
        if (key == target_key && !renewed)
        {
            renewed = true;
            /// The holder renews under the real current token: fresh far-future expiry.
            const auto got = InMemoryBackend::get(key);
            MountLease m = decodeMountLease(got->bytes);
            m.seq += 1;
            m.expires_at_ms = renewed_expires_ms;
            const PutResult renew = InMemoryBackend::putOverwrite(key, encodeMountLease(m), got->token);
            EXPECT_EQ(renew.outcome, PutOutcome::Done);
        }
        return InMemoryBackend::putOverwrite(key, bytes, expected, meta);
    }

private:
    String target_key;
    uint64_t renewed_expires_ms;
    bool renewed = false;
};
}

TEST(CasHeartbeatFloor, FenceOutLosesTokenRaceReclassifiesLive)
{
    Layout l("p");
    auto b = std::make_shared<RenewOnFenceBackend>(
        l.mountKey("s1"), /*renewed_expires*/ kNowMs + 120'000);

    /// One expired-unfenced mount. The function GETs it (expired), tries to fence it out, the
    /// decorator renews it concurrently under the real token, the PUT hits PreconditionFailed, the
    /// function re-GETs and reclassifies it as live — never fenced.
    seedMount(*b, l, "s1", /*expires*/ kNowMs - 60'000, /*fenced*/ false, /*min_active*/ 0);

    const HeartbeatFloor floor = computeHeartbeatFloor(*b, l, kNowMs, kSkewMarginMs);

    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(floor.live, 1u);

    const auto after = b->get(l.mountKey("s1"));
    ASSERT_TRUE(after.has_value());
    EXPECT_FALSE(decodeMountLease(after->bytes).gc_fenced);
}

TEST(CasHeartbeatFloor, EmptyPrefixYieldsNoLiveMounts)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");

    const HeartbeatFloor floor = computeHeartbeatFloor(*b, l, kNowMs, kSkewMarginMs);

    EXPECT_EQ(floor.live, 0u);
    EXPECT_EQ(floor.terminated, 0u);
    EXPECT_EQ(floor.fenced_now, 0u);
    EXPECT_EQ(floor.already_fenced, 0u);
}

/// ---- Task 1 (Phase 2): `listMounts` — read-only mount-slot enumeration for introspection ----

TEST(CasListMounts, ClassifiesEveryStateReadOnly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const uint64_t now_ms = 1'000'000;
    const uint64_t ttl_ms = 10'000;

    /// live: fresh claim for srid "a"
    ASSERT_EQ(claimMount(*backend, layout, "a", UInt128{1}, /*writer_epoch=*/1, now_ms, ttl_ms).kind,
              MountClaimResult::Claimed);
    /// expired: claim for "b" whose lease ran out long before now_ms
    ASSERT_EQ(claimMount(*backend, layout, "b", UInt128{2}, 1, now_ms - 100'000, ttl_ms).kind,
              MountClaimResult::Claimed);
    /// corrupt: garbage bytes in "c"'s mount slot
    backend->putIfAbsent(layout.mountKey("c"), "garbage-not-a-proto", {});

    auto mounts = listMounts(*backend, layout, now_ms, /*skew_margin_ms=*/ttl_ms / 2);
    ASSERT_EQ(mounts.size(), 3u);
    std::map<String, String> by_srid;
    for (const auto & m : mounts)
        by_srid[m.srid] = m.state;
    EXPECT_EQ(by_srid["a"], "live");
    EXPECT_EQ(by_srid["b"], "expired");
    EXPECT_EQ(by_srid["c"], "corrupt");

    /// READ-ONLY guarantee: "b" is expired but must NOT be fenced by listMounts
    /// (computeHeartbeatFloor would stamp gc_fenced=true; the introspection view must not).
    auto again = listMounts(*backend, layout, now_ms, ttl_ms / 2);
    for (const auto & m : again)
        if (m.srid == "b")
        {
            EXPECT_FALSE(m.lease.gc_fenced);
            EXPECT_EQ(m.state, "expired");
        }
}

/// A `srid` may itself contain `/` (e.g. `shard-01/replica-a` — legal per
/// `CasServerRootId.ValidationAcceptsCleanPathsRejectsBad`). Slicing the key by the last `/` before
/// the `/mount` suffix (as opposed to by `serverRootsPrefix()` length) truncates it to `replica-a`.
TEST(CasListMounts, NestedSridIsNotTruncated)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const uint64_t now_ms = 1'000'000;
    const uint64_t ttl_ms = 10'000;

    ASSERT_EQ(claimMount(*backend, layout, "shard-01/replica-a", UInt128{1}, /*writer_epoch=*/1, now_ms, ttl_ms).kind,
              MountClaimResult::Claimed);

    auto mounts = listMounts(*backend, layout, now_ms, /*skew_margin_ms=*/ttl_ms / 2);
    ASSERT_EQ(mounts.size(), 1u);
    EXPECT_EQ(mounts[0].srid, "shard-01/replica-a");
    EXPECT_EQ(mounts[0].state, "live");
}

/// "A fence costs an epoch": a same-(uuid, epoch) re-claim must NOT refresh a `gc_fenced` body in
/// place — that would resurrect a fenced incarnation. It is terminal for THIS epoch; only a
/// DIFFERENT (fresh) epoch may reclaim the slot.
TEST(CasClaimMount, SameEpochFencedIsNotRefreshable)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    /// mint for (uuid 1, epoch 1), then fence it in place (what computeHeartbeatFloor does):
    ASSERT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, 1, 1000, 10'000).kind,
              MountClaimResult::Claimed);
    {
        auto got = backend->get(layout.mountKey("a"));
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        ASSERT_EQ(backend->putOverwrite(layout.mountKey("a"), encodeMountLease(fenced), got->token).outcome,
                  PutOutcome::Done);
    }
    /// Same (uuid, epoch) re-claim must NOT refresh a fenced body — a fence costs an epoch:
    const auto r = claimMount(*backend, layout, "a", DB::UInt128{1}, 1, 2000, 10'000);
    EXPECT_EQ(r.kind, MountClaimResult::FencedSelf);
    /// The body on the backend is still the fenced one (no write happened):
    EXPECT_TRUE(decodeMountLease(backend->get(layout.mountKey("a"))->bytes).gc_fenced);
    /// A DIFFERENT epoch reclaims immediately (existing branch, unchanged):
    EXPECT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, 2, 2000, 10'000).kind,
              MountClaimResult::Claimed);
}
