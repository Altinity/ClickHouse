#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

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
