#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

#include <chrono>
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
    EXPECT_EQ(layout.serverRootWatermarkKey("replica-a"), "p/gc/server-roots/replica-a/watermark");

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
    MountLeaseKeeper k(b, l, "r", UInt128(1), 7, std::chrono::milliseconds(100), [&] { return now; });
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
    MountLeaseKeeper k(b, l, "r", UInt128(1), /*epoch*/ 7, std::chrono::milliseconds(100), [&] { return now; });
    EXPECT_NO_THROW(k.start());     // adopts our own live (uuid=1,epoch=7) mount — NOT a double-start
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);

    // A keeper for the SAME uuid but a DIFFERENT live epoch must fail closed (superseded/double-start):
    MountLeaseKeeper k2(b, l, "r", UInt128(1), /*epoch*/ 8, std::chrono::milliseconds(100), [&] { return now; });
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
    auto a = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100)});
    ASSERT_NE(a, nullptr);
    const uint64_t e1 = a->writerEpoch();

    /// A restart of the SAME server (same uuid) must NOT abort: it waits out the stale lease (<= ~300ms)
    /// and reclaims the mount, coming up with a strictly higher durable writer_epoch.
    StorePtr a2;
    EXPECT_NO_THROW(
        a2 = Store::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
            .mount_lease_ttl_ms = std::chrono::milliseconds(300),
            .mount_renew_period = std::chrono::milliseconds(100)}));
    ASSERT_NE(a2, nullptr);
    EXPECT_GT(a2->writerEpoch(), e1);
}
