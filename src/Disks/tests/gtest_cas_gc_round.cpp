#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.h>
#include <Disks/tests/cas_test_helpers.h>

#include <limits>

#include <map>
#include <mutex>
#include <set>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int FILE_DOESNT_EXIST;
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::displaceObjectToken;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;

/// Per-step regular-round tests (M-C3). This file starts with the LEASE (Task 5); the
/// fold/retire/fence/recheck/cascade/trim step tests land here in Tasks 6-12.
///
/// The lease steal window is observation-based and deterministic (see CasGc.h): a contender
/// becomes steal-eligible when it observes the SAME (owner, seq) across two of its own
/// consecutive round attempts. No test below sleeps or reads a clock — "time" is simply the
/// order of runRegularRound calls.

namespace
{

StorePtr openTestStore(std::shared_ptr<InMemoryBackend> & out_backend)
{
    out_backend = std::make_shared<InMemoryBackend>();
    return Store::open(out_backend, PoolConfig{.pool_prefix = "p"});
}

GcState readState(InMemoryBackend & b, const Store & s)
{
    const auto got = b.get(s.layout().gcStateKey());
    if (!got)
    {
        ADD_FAILURE() << "gc/state absent";
        return {};
    }
    return decodeGcState(got->bytes);
}

/// ---- fold (Task 6) fixtures ----

/// Counts GETs per key (precedent: TinyPageBackend in gtest_cas_retire_view.cpp). Used to prove
/// once-per-tree expansion and cursor-honoring incremental folds: reset after the publishes, then
/// count the GC's own GETs of a specific tree object during runRegularRound.
class CountingGetBackend : public InMemoryBackend
{
public:
    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++get_counts[key];
        }
        return InMemoryBackend::get(key, range);
    }

    uint64_t getCount(const String & key) const
    {
        std::lock_guard lock(count_mutex);
        const auto it = get_counts.find(key);
        return it == get_counts.end() ? 0 : it->second;
    }

    void resetCounts()
    {
        std::lock_guard lock(count_mutex);
        get_counts.clear();
    }

private:
    mutable std::mutex count_mutex;
    std::map<String, uint64_t> get_counts;
};

/// Injects a one-shot Conflict on the N-th casPut to one key (counted from arming). failNextCasPut
/// cannot target the fold's gc/state CAS specifically - the lease CAS to the same key comes first -
/// so this counts per key: N=2 hits the fold CAS (lease create/renew is the 1st).
class FailNthCasPutBackend : public InMemoryBackend
{
public:
    void failNthCasPut(const String & key, uint64_t n)
    {
        std::lock_guard lock(fail_mutex);
        fail_key = key;
        fail_at = n;
        seen = 0;
    }

    CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                      Token * out_token = nullptr, const ObjectMeta & meta = {}) override
    {
        {
            std::lock_guard lock(fail_mutex);
            if (fail_at != 0 && key == fail_key && ++seen == fail_at)
            {
                fail_at = 0;                                     /// one-shot
                return CasOutcome::Conflict;
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, out_token, meta);
    }

private:
    std::mutex fail_mutex;
    String fail_key;
    uint64_t fail_at = 0;
    uint64_t seen = 0;
};

/// Publish one ref through the real Build: tree {"f" -> blob of `payload`}. Returns the tree id.
TreeId publishPart(const StorePtr & s, const String & ns, const String & ref, const String & payload)
{
    auto build = s->startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    TreeEntry entry;
    entry.name = "f";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of(payload);
    entry.file_size = payload.size();
    const TreeId tree = build->putTree({entry});
    build->publish(RootNamespace{ns}, ref, tree, {});
    return tree;
}

/// Delete a tree object out-of-band (as a completed prior GC round would have after a drop).
void rawDeleteTree(InMemoryBackend & b, const Store & s, const TreeId & tree)
{
    const String key = s.layout().treeKey(tree);
    const auto head = b.head(key);
    ASSERT_TRUE(head.exists) << "tree object absent: " << key;
    ASSERT_EQ(b.deleteExact(key, head.token).kind, DeleteOutcome::Kind::Deleted);
}

GcSnap readSnap(InMemoryBackend & b, const Store & s, uint64_t generation, uint64_t snap_shard)
{
    const auto got = b.get(s.layout().gcSnapKey(generation, snap_shard));
    if (!got)
    {
        ADD_FAILURE() << "gc/snap " << generation << "/" << snap_shard << " absent";
        return {};
    }
    return decodeGcSnap(got->bytes);
}

}

TEST(CasGcLease, FreshPoolAcquiresAndRenews)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, hexToU128("00000000000000000000000000000001"));

    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st1 = readState(*b, *s);
    EXPECT_EQ(st1.lease.owner, hexToU128("00000000000000000000000000000001"));
    const uint64_t seq1 = st1.lease.seq;
    EXPECT_GE(seq1, 1u);

    EXPECT_TRUE(gc.runRegularRound().acquired_lease);            /// renew
    const GcState st2 = readState(*b, *s);
    EXPECT_EQ(st2.lease.owner, hexToU128("00000000000000000000000000000001"));
    EXPECT_GT(st2.lease.seq, seq1);                              /// seq strictly advanced
    EXPECT_EQ(st2.fence_seq, st1.fence_seq);                     /// renewal is NOT a new epoch
}

TEST(CasGcLease, ContenderBacksOffWhileIncumbentRenews)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// first sight: record observation
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);           /// incumbent renews (seq advances)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// gc2 sees a NEW seq => incumbent alive
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// alive again - never steals while renewing
    EXPECT_EQ(readState(*b, *s).lease.owner, hexToU128("0000000000000000000000000000000a"));
}

TEST(CasGcLease, StealAfterObservedNonRenewalBumpsEpoch)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    const GcState st0 = readState(*b, *s);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// observation recorded; gc1 then DIES
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);           /// same (owner, seq) observed twice => steal
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, hexToU128("0000000000000000000000000000000b"));
    EXPECT_GT(st.lease.seq, st0.lease.seq);
    EXPECT_EQ(st.fence_seq, st0.fence_seq + 1);                  /// steal bumps the leadership epoch
}

TEST(CasGcLease, HeartbeatBlocksFalseStealOfAliveLeader)
{
    /// B160: a slow-but-alive incumbent whose lease.seq is frozen for its (long) round must NOT be
    /// stolen from, because its advisory heartbeat keeps advancing. Without the heartbeat this is
    /// exactly StealAfterObservedNonRenewalBumpsEpoch above (a steal); WITH it, no steal.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const UInt128 leader = hexToU128("0000000000000000000000000000000a");
    Gc gc1(s, leader);
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);          /// gc1 leads (seq frozen for its round)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// gc2 observes (gc/hb absent yet)

    Gc::pulseHeartbeat(*s, leader);                             /// gc1 mid-round but heartbeating (hb 0->1)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// hb advanced => alive => NO steal
    Gc::pulseHeartbeat(*s, leader);                             /// hb 1->2
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// still no steal while heartbeating
    EXPECT_EQ(readState(*b, *s).lease.owner, leader);           /// gc1 still owns the lease
}

TEST(CasGcLease, FailoverStealOnceHeartbeatStops)
{
    /// B160: once the incumbent stops heartbeating (it died), a follower observing the now-frozen
    /// heartbeat steals — automatic failover is preserved.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const UInt128 leader = hexToU128("0000000000000000000000000000000a");
    Gc gc1(s, leader);
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// obs #1
    Gc::pulseHeartbeat(*s, leader);                             /// one last pulse (hb 0->1)
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);         /// hb advanced => no steal; records hb=1
    /// gc1 now DEAD: no renew, no further pulse. hb stays at 1 == gc2's last observation.
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);          /// hb frozen + seq frozen => STEAL
    EXPECT_EQ(readState(*b, *s).lease.owner, hexToU128("0000000000000000000000000000000b"));
}

TEST(CasGcLease, DeadIncumbentThenRevivedIncumbentWinsRace)
{
    /// A stalled incumbent that revives and renews BEFORE the contender's second look resets the
    /// contender's window: gc2's second observation sees a NEW seq => NOT steal-eligible => backs
    /// off; the owner stays gc1. (The variant where the contender goes for the steal CAS with a
    /// stale token and loses is ConcurrentStealLosesCas below.)
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// obs #1
    /// gc2 is now one observation away from steal-eligibility. gc1 revives and renews:
    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// new seq seen => window resets
    EXPECT_EQ(readState(*b, *s).lease.owner, hexToU128("0000000000000000000000000000000a"));
}

TEST(CasGcLease, ConcurrentStealLosesCas)
{
    /// The CAS-race horn: gc2 is steal-eligible and goes for the CAS, but gc/state moved under it
    /// (injected one-shot conflict). It must back off (never acquired=true off a lost CAS, never
    /// "retry the steal blindly") and the owner on storage must be unperturbed. Because the
    /// injected conflict left the object literally unchanged (still stalled), gc2's NEXT round is
    /// steal-eligible again and succeeds.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    const GcState st0 = readState(*b, *s);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// obs #1; gc1 stalls now
    b->failNextCasPut(s->layout().gcStateKey());                 /// inject: gc2's steal CAS conflicts
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// steal attempt loses the CAS => back off
    const GcState st1 = readState(*b, *s);
    EXPECT_EQ(st1.lease.owner, hexToU128("0000000000000000000000000000000a"));   /// unchanged
    EXPECT_EQ(st1.lease.seq, st0.lease.seq);                                     /// nothing clobbered
    /// next attempt: still the same (owner, seq) on storage => steal-eligible again => succeeds now
    EXPECT_TRUE(gc2.runRegularRound().acquired_lease);
    EXPECT_EQ(readState(*b, *s).lease.owner, hexToU128("0000000000000000000000000000000b"));
}

TEST(CasGcLease, CreateConflictReReadsWithinTheBound)
{
    /// The create-Conflict branch: a fresh pool where the create-if-absent CAS conflicts (one-shot
    /// injection). The contender re-reads and falls through within its bounded (2) CAS attempts —
    /// here the re-read still finds the key absent (the injected conflict wrote nothing), so the
    /// second attempt creates and acquires. A real lost create race (the re-read finding a foreign
    /// owner) degrades into the observe-and-back-off branch covered above.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, hexToU128("0000000000000000000000000000000c"));

    b->failNextCasPut(s->layout().gcStateKey());
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, hexToU128("0000000000000000000000000000000c"));
    EXPECT_EQ(st.lease.seq, 1u);
}

TEST(CasGcLease, CtorFailsClosedOnBadArguments)
{
    /// Guards: a null store and gc_id == 0 (reserved for "lease never held") are caller bugs.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { Gc(nullptr, hexToU128("00000000000000000000000000000001")); });
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&] { Gc(s, DB::UInt128(0)); });
}

TEST(CasGcLease, IncumbentRenewConflictRetriesOnceAndAcquires)
{
    /// The incumbent's own renew CAS conflicts (one-shot injection). The documented behavior:
    /// re-read sees our own ownership => the renew is retried ONCE within the bounded (2) CAS
    /// attempts => acquired. The invariant under test: never acquired=true without a Committed
    /// CAS - the state on storage must carry the seq the SECOND (committed) attempt wrote.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc(s, hexToU128("0000000000000000000000000000000d"));

    ASSERT_TRUE(gc.runRegularRound().acquired_lease);            /// create: seq 1
    b->failNextCasPut(s->layout().gcStateKey());                 /// inject: the renew CAS conflicts
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);            /// re-read (still us) => retried once
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.lease.owner, hexToU128("0000000000000000000000000000000d"));
    EXPECT_EQ(st.lease.seq, 2u);                                 /// the committed retry's seq
}

TEST(CasGcLease, VanishedStateAfterObservationFailsClosed)
{
    /// gc/state is never legally deleted - absent AFTER a recorded observation proves an
    /// out-of-model deletion. Recreating a default state would reset round/fence_seq/cursors;
    /// the lease protocol must fail closed (CORRUPTED_DATA) instead.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    ASSERT_TRUE(gc1.runRegularRound().acquired_lease);
    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);          /// gc2 records an observation

    const auto head = b->head(s->layout().gcStateKey());         /// out-of-model wipe (raw delete)
    ASSERT_TRUE(head.exists);
    ASSERT_EQ(b->deleteExact(s->layout().gcStateKey(), head.token).kind, DeleteOutcome::Kind::Deleted);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });
}

/// ---- R1 FOLD (Task 6) ----
///
/// The fold merges journal records in (folded_cursor, shard_version] into the snap shards at a NEW
/// write-once generation, durable BEFORE the cursors advance by the gc/state CAS (spec section 7 R1;
/// the model's GFold). Candidates for retire are derived STATELESSLY from the durable snap
/// (zeroInDegreeKnown - the GRetire guard), never from in-memory fold transitions.

TEST(CasGcFold, FreshUploadsAreNeverCandidates)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    const UInt128 tree_hash = hexToU128(tree.string());

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);                               /// everything pinned

    /// snap durable at generation 1; cursor advanced:
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.snap_generation, 1u);
    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, tree_hash), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, u128Of("payload")), 1u);
    EXPECT_TRUE(snap.isExpanded(tree_hash));
    EXPECT_TRUE(snap.zeroInDegreeKnown().empty());

    const String cursor_key = "srv1/tbl/" + std::to_string(shardOfForTest("part_1", s->poolMeta().root_shards));
    ASSERT_TRUE(st.folded_cursor.contains(cursor_key));
    EXPECT_GT(st.folded_cursor.at(cursor_key), 0u);
}

TEST(CasGcFold, DropZeroesTreeButChildStaysPinned)
{
    /// publish part_1 (T -> B); dropRef; one fold sees both the '+' and the '-': T zeroes
    /// (candidate), B stays pinned by T's tree edge (the strip happens only at cascade, after
    /// the delete - Task 10). The model's GFold rem only removes the root edge.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    const UInt128 tree_hash = hexToU128(tree.string());
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 1u);                               /// T only, never B

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, tree_hash), 0u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, u128Of("payload")), 1u);   /// pinned by T's edge
    const auto cands = snap.zeroInDegreeKnown();
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].kind, ObjectKind::Tree);
    EXPECT_EQ(cands[0].hash, tree_hash);
}

TEST(CasGcFold, RepublishSameRefIsLastOpWins)
{
    /// The Task-3 Critical carryover at fold level: a republish of an existing ref produces
    /// consecutive journal Adds for the same (root_shard, part_name) with DIFFERENT trees and NO
    /// Remove between. addRootEdge is last-op-wins and returns the displaced old target - the fold
    /// must collect it. T1 zeroes (candidate via the durable snap), T2 lives at in-degree 1; T1's
    /// child edges stay (T1 was expanded when its Add folded; the strip is cascade's job).
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId t1 = publishPart(s, "srv1/tbl", "part_1", "one");
    const TreeId t2 = publishPart(s, "srv1/tbl", "part_1", "two");      /// same ref, new tree
    const UInt128 t1_hash = hexToU128(t1.string());
    const UInt128 t2_hash = hexToU128(t2.string());

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 1u);

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, t1_hash), 0u);     /// displaced by last-op-wins
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, t2_hash), 1u);     /// alive
    EXPECT_TRUE(snap.isExpanded(t1_hash));                       /// expanded when its Add folded
    EXPECT_TRUE(snap.isExpanded(t2_hash));
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, u128Of("one")), 1u);   /// still pinned by T1's edge
    const auto cands = snap.zeroInDegreeKnown();
    ASSERT_EQ(cands.size(), 1u);
    EXPECT_EQ(cands[0].kind, ObjectKind::Tree);
    EXPECT_EQ(cands[0].hash, t1_hash);
}

TEST(CasGcFold, ExpansionIsOncePerTree)
{
    /// Two refs (different names, same content tree T): the fold reads the tree object exactly
    /// ONCE - the second '+' to T sees the expansion marker. Counted between explicit markers
    /// (reset after the publishes, before the round), so the publishes' own GETs do not pollute.
    auto b = std::make_shared<CountingGetBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});

    auto build = s->startBuild({});
    build->putBlob(idOf("shared"), BlobSource::fromString("shared"));
    TreeEntry entry;
    entry.name = "f";
    entry.placement = Placement::Blob;
    entry.file_hash = u128Of("shared");
    entry.file_size = 6;
    const TreeId tree = build->putTree({entry});
    build->publish(RootNamespace{"srv1/tbl"}, "part_a", tree, {});
    build->publish(RootNamespace{"srv1/tbl"}, "part_b", tree, {});

    b->resetCounts();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);
    EXPECT_EQ(b->getCount(s->layout().treeKey(tree)), 1u);       /// read once, marker after

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(tree.string())), 2u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Blob, u128Of("shared")), 1u);    /// ONE tree edge, not two
    EXPECT_TRUE(snap.isExpanded(hexToU128(tree.string())));
}

TEST(CasGcFold, IncrementalSecondFoldOnlyNewRecords)
{
    /// Round 1 folds part_1; part_2 publishes after; round 2 must fold ONLY part_2's records
    /// (cursor honored - part_1's tree object is NOT re-read; its edges arrive via the loaded
    /// generation-1 snap). In-degrees are cumulative across generations.
    auto b = std::make_shared<CountingGetBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId t1 = publishPart(s, "srv1/tbl", "part_1", "one");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(readState(*b, *s).snap_generation, 1u);

    const TreeId t2 = publishPart(s, "srv1/tbl", "part_2", "two");
    b->resetCounts();
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);
    EXPECT_EQ(b->getCount(s->layout().treeKey(t1)), 0u);         /// already-folded record skipped
    EXPECT_EQ(b->getCount(s->layout().treeKey(t2)), 1u);

    EXPECT_EQ(readState(*b, *s).snap_generation, 2u);
    const GcSnap snap = readSnap(*b, *s, 2, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t1.string())), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t2.string())), 1u);
}

TEST(CasGcFold, DurableSnapBeforeCursorAdvance)
{
    /// Crash-safety seam: the new-generation snap goes durable FIRST; only then does the gc/state
    /// CAS advance snap_generation + cursors. Inject a Conflict on exactly the fold's gc/state CAS
    /// (the 2nd casPut to gcStateKey - the lease create is the 1st): the round throws ABORTED, the
    /// gen-1 snap object EXISTS (orphaned, harmless), gc/state still carries the OLD generation and
    /// cursors - a crash here loses nothing. The rerun converges: putIfAbsent of gen 1 hits
    /// PreconditionFailed, byte-equality proves it is OUR replay, and the round lands.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 2);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });

    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.snap_generation, 0u);                           /// cursor side did NOT advance
    EXPECT_TRUE(st.folded_cursor.empty());
    EXPECT_TRUE(b->get(s->layout().gcSnapKey(1, 0)).has_value());    /// snap durable first

    const RoundReport rep = gc.runRegularRound();                /// replay: re-folds and lands
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);
    const GcState st2 = readState(*b, *s);
    EXPECT_EQ(st2.snap_generation, 1u);
    EXPECT_FALSE(st2.folded_cursor.empty());
}

TEST(CasGcFold, ForeignDivergentGenerationIsProbedPast)
{
    /// Write-once generations: a generation key occupied by DIFFERENT bytes (a diverged competing
    /// fold's leftover) can never be reused, so aborting on it forever would wedge GC. The fold
    /// probes UPWARD: abandon gen 1, land at gen 2. The gc/state pointer is authoritative -
    /// generations need not be dense.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    ASSERT_EQ(b->putIfAbsent(s->layout().gcSnapKey(1, 0), "not-our-fold"), PutOutcome::Done);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);
    EXPECT_EQ(readState(*b, *s).snap_generation, 2u);            /// gen 1 abandoned, gen 2 ours
    const GcSnap snap = readSnap(*b, *s, 2, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(tree.string())), 1u);
}

TEST(CasGcFold, GenerationProbeRecoversAfterLostCursorCas)
{
    /// The wedge horn: a fold writes the gen-1 snaps but loses the gc/state CAS; a NEW journal
    /// record then arrives. Every later fold covers a LARGER range, so its gen-1 bytes can never
    /// match the orphan again - a fixed write generation of snap_generation+1 would ABORT forever
    /// (regular GC permanently dead; the folded range only grows). The probe-upward persist must
    /// land at gen 2 instead.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId t1 = publishPart(s, "srv1/tbl", "part_1", "one");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 2);               /// the fold's cursor CAS loses
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });
    ASSERT_TRUE(b->get(s->layout().gcSnapKey(1, 0)).has_value());    /// the gen-1 orphan

    const TreeId t2 = publishPart(s, "srv1/tbl", "part_2", "two");   /// the folded range grows

    const RoundReport rep = gc.runRegularRound();                /// must NOT wedge on gen 1
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.snap_generation, 2u);
    EXPECT_FALSE(st.folded_cursor.empty());
    const GcSnap snap = readSnap(*b, *s, 2, 0);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t1.string())), 1u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t2.string())), 1u);
}

TEST(CasGcFold, SnapShardsOtherThanOneIsNotImplemented)
{
    /// Last-op-wins displacement is INTRA-shard only: with the displaced and displacing trees in
    /// different snap shards a republish would leak the old root edge forever. Until cross-shard
    /// displacement is designed, a pool whose gc/state carries snap_shards != 1 must be refused
    /// (NOT_IMPLEMENTED, fail closed), never silently mis-folded.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    GcState injected;
    injected.snap_shards = 2;
    ASSERT_EQ(b->putIfAbsent(s->layout().gcStateKey(), encodeGcState(injected)), PutOutcome::Done);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    EXPECT_FALSE(gc.runRegularRound().acquired_lease);           /// first sight of the never-held lease
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { gc.runRegularRound(); });   /// steal, then refuse
}

TEST(CasGcFold, AbsentTreeWithLaterRemoveSkipsExpansion)
{
    /// readTree-absent DECISION, skip branch: at the Add's journal position the ref was live, but
    /// the tree object is legitimately gone NOW (a later Remove + an already completed prior round's
    /// delete). The lookahead finds the later Remove for the same ref in the SAME shard's journal =>
    /// skip expansion (no edges for a gone tree - over-count-only preserved; marker stays unset).
    /// Staged before the delete pipeline exists by removing the tree object out-of-band.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    const UInt128 tree_hash = hexToU128(tree.string());
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    rawDeleteTree(*b, *s, tree);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();                /// must NOT throw
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);                               /// T zeroed but is ABSENT => retire skips it

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_FALSE(snap.isExpanded(tree_hash));                    /// expansion skipped
    EXPECT_FALSE(snap.isKnown(ObjectKind::Blob, u128Of("payload")));  /// no child edges added
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, tree_hash), 0u);
}

TEST(CasGcFold, AbsentTreeWithLaterDisplacingAddSkipsExpansion)
{
    /// readTree-absent DECISION, displacing-Add branch: a republish (last-op-wins, the COMMON
    /// mutation path) displaces the old tree WITHOUT a Remove - journal [Add(p1->T1), Add(p1->T2)].
    /// Another leader can legally have deleted the displaced T1 before a stale leader folds the
    /// first Add, so the lookahead must accept a later Add for the same ref as displacement proof
    /// too; only a Remove would make this a false corruption-shaped alarm for a legal race.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId t1 = publishPart(s, "srv1/tbl", "part_1", "one");
    const TreeId t2 = publishPart(s, "srv1/tbl", "part_1", "two");   /// displacing republish
    rawDeleteTree(*b, *s, t1);                                   /// as a completed competing round could

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();                /// must NOT throw
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);                               /// the displaced T1 is ABSENT => retire skips it

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_FALSE(snap.isExpanded(hexToU128(t1.string())));       /// expansion skipped
    EXPECT_FALSE(snap.isKnown(ObjectKind::Blob, u128Of("one"))); /// no child edges for the gone tree
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t1.string())), 0u);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, hexToU128(t2.string())), 1u);   /// alive
    EXPECT_TRUE(snap.isExpanded(hexToU128(t2.string())));
}

TEST(CasGcFold, AbsentTreeWithoutLaterRemoveFailsClosed)
{
    /// readTree-absent DECISION, fail-closed branch: no later Remove => the manifest claims a LIVE
    /// ref to a missing tree (INV-NO-DANGLE surfaced) => the exception propagates; nothing durable
    /// moved (the fold aborts before the snap/cursor persist).
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    rawDeleteTree(*b, *s, tree);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { gc.runRegularRound(); });

    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.snap_generation, 0u);
    EXPECT_TRUE(st.folded_cursor.empty());
    EXPECT_FALSE(b->get(s->layout().gcSnapKey(1, 0)).has_value());   /// nothing durable was written
}

/// ---- R2 RETIRE (Task 7) ----
///
/// RETIRE observes each candidate's CURRENT incarnation token with one HEAD and writes the round's
/// retire sets to gc/retired/<round>.<fence_seq>/<snap_shard> (append-by-unique-path: one object
/// per path, written once), then CASes gc/state.round = round. The sets are durable BEFORE the
/// round number advances (INV-MONOTONE-GC: a writer whose RetireView refreshes at the new round is
/// guaranteed to see the entries); the round CAS is the durable "retire phase complete" marker.
/// Candidates are derived STATELESSLY from the durable snap (zeroInDegreeKnown - the model's
/// GRetire guard `present /\ everEdged /\ InDeg = 0`). Retired != dead: the entries are the
/// writer-facing "resurrect, don't reuse" barrier (spec section 7 R2).

TEST(CasGcRetire, ObservesCurrentTokenDeletesExactAndDropsEntries)
{
    /// The full R2->R4 chain for a truly-unreachable tree: retire observes the CURRENT token, the
    /// recheck's single delete site removes exactly that incarnation, the outcome log records it,
    /// and the retired set DROPS on the confirmed outcome (the writer-facing barrier ends with the
    /// round - a deleted incarnation cannot return, INV-NO-RETURN).
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload-1");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const Token pre_round_token = b->head(s->layout().treeKey(tree)).token;

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_GE(rep.candidates, 1u);            /// the tree zeroed (the blob stays pinned by the tree edge)
    EXPECT_EQ(rep.deleted, 1u);
    EXPECT_EQ(rep.spared + rep.absent + rep.replaced, 0u);

    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 1u);

    /// the tree object is GONE - deleted at exactly the observed token;
    EXPECT_FALSE(b->head(s->layout().treeKey(tree)).exists);

    /// the outcome log is the durable confirmation, carrying the observed token:
    const auto outcome_obj = b->get(s->layout().outcomesKey(1, st.fence_seq, 0));
    ASSERT_TRUE(outcome_obj.has_value());
    const OutcomeLog log = decodeOutcomeLog(outcome_obj->bytes);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].kind, ObjectKind::Tree);
    EXPECT_EQ(log.entries[0].hash, hexToU128(tree.string()));
    EXPECT_EQ(log.entries[0].token, pre_round_token);          /// observed, never fabricated
    EXPECT_EQ(log.entries[0].outcome, OutcomeKind::Deleted);

    /// the retired set dropped on the confirmed outcome (entries drop on outcomes, spec section 7):
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());
    auto s2 = Store::open(b, PoolConfig{.pool_prefix = "p"});
    EXPECT_FALSE(s2->retireView().isCondemnedToken(ObjectKind::Tree, hexToU128(tree.string()), pre_round_token));

    /// fence_version[1] served its recheck and was erased (gc/state must not grow forever):
    EXPECT_FALSE(st.fence_version.contains(1));
}

TEST(CasGcRetire, AbsentCandidateIsSkippedNotFabricated)
{
    /// The candidate's object is already gone (a completed prior round's delete - staged here by
    /// removing the tree out-of-band): retire records NOTHING for it. There is no token to condemn
    /// and fabricating one would be a fail-open delete ticket. The round still advances (an
    /// all-skipped round completed its retire phase; the recheck has nothing to do for an absent
    /// candidate) and no empty retired object is written.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    rawDeleteTree(*b, *s, tree);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();             /// must NOT throw
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 0u);                            /// no entry written

    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 1u);                                  /// the round still completed
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());   /// no empty objects
}

TEST(CasGcRetire, DeletedCandidateDoesNotReappear)
{
    /// A round is terminal for its candidates: round 1 deletes the unreachable tree (entries drop
    /// on confirmed outcomes), so round 2 - with no new journal activity - re-derives the same
    /// zero-in-degree known node from the durable snap but SKIPS it at retire (HEAD-absent: no
    /// token to condemn). No retired object, no outcomes, no error; the round still advances.
    /// (The model's no-dup guard `¬∃ retired (h, tokOf)` is realized by the synchronous
    /// drop-on-outcome: the entry is gone the moment its outcome confirmed.)
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep1 = gc.runRegularRound();
    EXPECT_EQ(rep1.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().treeKey(tree)).exists);

    /// Round 2: the CASCADE freed the tree's child blob in round 1 (strip), so round 2 retires and
    /// deletes IT - the LIVE-RECLAIM bound made concrete (a dropped part's tree goes in round 1,
    /// its exclusively-owned blobs in round 2). The deleted TREE itself never reappears (HEAD-
    /// absent => skipped; the model's no-dup guard realized by the synchronous drop-on-outcome).
    const RoundReport rep2 = gc.runRegularRound();            /// no new journal activity in between
    EXPECT_TRUE(rep2.acquired_lease);
    EXPECT_EQ(rep2.candidates, 1u);                           /// the cascade-freed blob
    EXPECT_EQ(rep2.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().blobKey(BlobId{u128ToHex(u128Of("payload"))})).exists);

    /// Round 3: nothing left - the dead subgraph is fully reclaimed and nothing reappears.
    const RoundReport rep3 = gc.runRegularRound();
    EXPECT_TRUE(rep3.acquired_lease);
    EXPECT_EQ(rep3.candidates, 0u);
    EXPECT_EQ(rep3.deleted + rep3.absent + rep3.replaced + rep3.spared, 0u);

    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 3u);
    EXPECT_FALSE(b->get(s->layout().retiredKey(3, st.fence_seq, 0)).has_value());
    EXPECT_FALSE(b->get(s->layout().outcomesKey(3, st.fence_seq, 0)).has_value());
}

TEST(CasGcRetire, RetireSetsDurableBeforeRoundCas)
{
    /// The ordering seam (INV-MONOTONE-GC): the retire sets go durable FIRST; only then does the
    /// gc/state CAS advance .round. Inject a Conflict on exactly the retire CAS (3rd casPut to
    /// gcStateKey: lease create = 1st, fold cursor CAS = 2nd): the round throws ABORTED with the
    /// retired set already durable but .round NOT advanced - a crash here leaves only re-derivable
    /// durable state. The rerun is the TRUE same-round replay: it re-derives the same set,
    /// putIfAbsent hits PreconditionFailed, byte-equality proves the set is OURS (adopt, not
    /// abort), and the round CAS lands.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 3);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });

    GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 0u);                                  /// the marker did NOT advance...
    const auto retired_obj = b->get(s->layout().retiredKey(1, st.fence_seq, 0));
    ASSERT_TRUE(retired_obj.has_value());                     /// ...but the set is already durable

    const RoundReport rep = gc.runRegularRound();             /// replay: adopts the set, completes
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 1u);                            /// the adopted durable set's entry
    EXPECT_EQ(rep.deleted, 1u);                               /// ...and the round ran to the delete
    st = readState(*b, *s);
    EXPECT_EQ(st.round, 1u);
    /// the replayed round completed: tree gone, retired set dropped, outcome log durable
    EXPECT_FALSE(b->head(s->layout().treeKey(tree)).exists);
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());
    const OutcomeLog log = decodeOutcomeLog(b->get(s->layout().outcomesKey(1, st.fence_seq, 0))->bytes);
    ASSERT_EQ(log.entries.size(), 1u);
    EXPECT_EQ(log.entries[0].hash, hexToU128(tree.string()));
    EXPECT_EQ(log.entries[0].outcome, OutcomeKind::Deleted);
}

/// Fires a one-shot hook just BEFORE the first fence CAS lands on the target shard key (the body
/// carries a "fence_round" the manifest did not yet have - we trigger on any casPut to the key
/// once armed). The hook runs raw backend ops only (base-class methods), simulating an interleaved
/// actor in the fence window: a racing publish (horn 1), a resurrect (412 horn), or a zombie
/// delete landing (absent horn).
class OnFenceHookBackend : public InMemoryBackend
{
public:
    CasOutcome casPut(const String & key, const String & bytes,
                      const std::optional<Token> & expected, Token * out_token = nullptr, const ObjectMeta & meta = {}) override
    {
        if (!fired && key == armed_key && ++count == fire_at)
        {
            fired = true;
            hook();
        }
        return InMemoryBackend::casPut(key, bytes, expected, out_token, meta);
    }

    void armOnCasPut(String key, std::function<void()> hook_)
    {
        armOnNthCasPut(std::move(key), 1, std::move(hook_));
    }

    /// Fire just before the nth casPut to the key lands (per-round casPut order on a shard key:
    /// fence = 1st, trim = 2nd when the shard has trimmable records).
    void armOnNthCasPut(String key, size_t nth, std::function<void()> hook_)
    {
        armed_key = std::move(key);
        hook = std::move(hook_);
        fire_at = nth;
        count = 0;
        fired = false;
    }

private:
    String armed_key;
    std::function<void()> hook;
    size_t fire_at = 1;
    size_t count = 0;
    bool fired = true;
};

TEST(CasGcRecheck, SparedWhenPublishRacesTheFence)
{
    /// HORN 1 of the no-return argument, mechanically: a publish committing in the fence window
    /// lands its journal record at or below the fence's committed version (the fence CAS conflicts
    /// and retries on the fresh manifest), so the RECHECK's fold-through-fence sees it, in-degree
    /// returns above zero, and the entry is SPARED - no delete, the object survives.
    auto b = std::make_shared<OnFenceHookBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const String shard_key = s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard);
    b->armOnCasPut(shard_key, [&]
    {
        /// the racing publish, raw (exactly the manifest CAS a writer's publish performs):
        const auto got = b->InMemoryBackend::get(shard_key);
        ASSERT_TRUE(got.has_value());
        RootShard root = decodeRootShard(got->bytes);
        ++root.shard_version;
        root.refs["part_1"] = RefPayload{.tree_id = hexToU128(tree.string()), .tree_size = 0, .mutable_files = {}};
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Add, .ref_name = "part_1",
            .tree_id = hexToU128(tree.string()), .at_version = root.shard_version});
        ASSERT_EQ(b->InMemoryBackend::casPut(shard_key, encodeRootShard(root), got->token, nullptr),
                  CasOutcome::Committed);
    });

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.spared, 1u);
    EXPECT_EQ(rep.deleted, 0u);
    EXPECT_TRUE(b->head(s->layout().treeKey(tree)).exists);   /// the object SURVIVED
    /// resolvable again - the racing publish re-pinned it:
    EXPECT_TRUE(s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1").has_value());
}

TEST(CasGcRecheck, ReplacedWhenResurrectionWins)
{
    /// The 412 horn: a writer resurrects the condemned incarnation (fresh tag, new token) in the
    /// fence window. The delete carries the OLD observed token, hits TokenMismatch, and the
    /// outcome is REPLACED - the 412-save health metric; no data loss, the new incarnation lives.
    auto b = std::make_shared<OnFenceHookBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const String tree_key = s->layout().treeKey(tree);
    const Token t0 = b->head(tree_key).token;
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    Token t1;
    b->armOnCasPut(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard), [&]
    {
        t1 = displaceObjectToken(*b, tree_key, ObjectKind::Tree);   /// the racing resurrect
    });

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_EQ(rep.replaced, 1u);
    EXPECT_EQ(rep.deleted, 0u);
    EXPECT_TRUE(b->head(tree_key).exists);                    /// the NEW incarnation survived
    EXPECT_EQ(b->head(tree_key).token, t1);
    EXPECT_NE(t1, t0);
}

TEST(CasGcRecheck, AbsentWhenAlreadyGone)
{
    /// A prior crashed round's delete landed (staged: a zombie delete fires in the fence window).
    /// The recheck's delete sees NotFound => outcome ABSENT - no error, the round completes.
    auto b = std::make_shared<OnFenceHookBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const String tree_key = s->layout().treeKey(tree);
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    b->armOnCasPut(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard), [&]
    {
        const Token current = b->InMemoryBackend::head(tree_key).token;
        ASSERT_EQ(b->InMemoryBackend::deleteExact(tree_key, current).kind, DeleteOutcome::Kind::Deleted);
    });

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_EQ(rep.absent, 1u);
    EXPECT_EQ(rep.deleted, 0u);
    const GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 1u);                                  /// the round completed normally
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());
}

TEST(CasGcRetire, DivergedRetiredSetFailsClosed)
{
    /// A retired-set key occupied by content that does not DECODE as a retired set: a decodable
    /// divergent occupant at our (round, fence_seq) is our own crashed prior attempt and is
    /// ADOPTED (RetireReplayAdoptsOwnCrashedAttempt below), but corrupt bytes must never be
    /// adopted as a delete input - fail closed (ABORTED), never overwrite, and the round marker
    /// must not advance.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    /// fresh pool: the executed round is 1, fence_seq 0
    ASSERT_EQ(b->putIfAbsent(s->layout().retiredKey(1, 0, 0), "not-our-retire"), PutOutcome::Done);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });
    EXPECT_EQ(readState(*b, *s).round, 0u);                   /// the marker never advanced
}

TEST(CasGcRetire, BlobHeaderUnderflowFailsClosed)
{
    /// retiredLogicalSize unit rows: blobs subtract the pool's fixed blob_header_len (the size in a
    /// retire entry is GC bookkeeping over PAYLOAD bytes); a blob OBJECT smaller than the fixed
    /// header is corrupt => CORRUPTED_DATA (fail closed, never a wrapped-around size); trees/packs
    /// account whole-object. Direct unit rows because a BLOB candidate cannot be constructed
    /// through a real round before Task 9 lands: a blob stays pinned by its parent tree's edge
    /// until the cascade strips it.
    EXPECT_EQ(retiredLogicalSize(ObjectKind::Blob, 300, 256), 44u);
    EXPECT_EQ(retiredLogicalSize(ObjectKind::Blob, 256, 256), 0u);     /// empty payload is legal
    EXPECT_EQ(retiredLogicalSize(ObjectKind::Tree, 100, 256), 100u);   /// whole-object
    EXPECT_EQ(retiredLogicalSize(ObjectKind::Pack, 100, 256), 100u);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { retiredLogicalSize(ObjectKind::Blob, 100, 256); });
}

TEST(CasGcRetire, RetireUsesFoldCommittedStateWithoutReread)
{
    /// H1 (review of ce7df279563): retire must operate on the EXACT (state, token) the fold's CAS
    /// committed - threaded through, never re-read. A post-fold re-read opens a zombie window: a
    /// lease steal landing between the fold CAS and the re-read hands the stale leader the THIEF's
    /// state (bumped fence_seq), letting it write stale-snap retire sets into the thief's epoch
    /// paths - defeating fence_seq isolation. Pin the contract by counting gc/state GETs across
    /// one full round: exactly ONE (the lease-acquire read), no post-fold re-read.
    auto b = std::make_shared<CountingGetBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    b->resetCounts();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_TRUE(rep.acquired_lease);
    EXPECT_EQ(rep.candidates, 1u);
    EXPECT_EQ(b->getCount(s->layout().gcStateKey()), 1u);     /// the lease-acquire read ONLY
    EXPECT_EQ(readState(*b, *s).round, 1u);                   /// the round still completed
}

/// ---- R3 FENCE (Task 8) ----
///
/// The FENCE writes fence_round := max(fence_round, round) into every PRESENT root-shard manifest
/// through the verified mutateShard CAS loop, and records each shard's committed shard_version in
/// gc/state.fence_version[round] (spec section 7 R3; the model's GFenceShard). The fence is
/// MONOTONE - a stale leader's lower round is absorbed by the max, never lowers it
/// (INV-MONOTONE-GC) - and the recorded committed version is the C++ equivalent of the model's
/// fencePos[s]: the manifest CAS totally orders the fence against publishes on that shard, which
/// is exactly what the no-return argument rests on.

/// Captures the durable gc/state bytes just BEFORE the nth casPut to a key lands. Used to observe
/// fence_version[round] at the recheck's pre-erase point: the recheck erases fence_version[<=round]
/// in the round's FINAL gc/state CAS (per-round casPut order on gc/state: lease, fold, retire,
/// fence, recheck-erase), so post-round state no longer carries the vector the recheck used.
class CaptureStateBackend : public InMemoryBackend
{
public:
    CasOutcome casPut(const String & key, const String & bytes,
                      const std::optional<Token> & expected, Token * out_token = nullptr, const ObjectMeta & meta = {}) override
    {
        if (key == capture_key && ++count == capture_nth)
            if (const auto got = InMemoryBackend::get(key))
                captured = got->bytes;
        return InMemoryBackend::casPut(key, bytes, expected, out_token, meta);
    }

    void armCapture(String key, size_t nth)
    {
        capture_key = std::move(key);
        capture_nth = nth;
        count = 0;
        captured.reset();
    }

    std::optional<String> captured;

private:
    String capture_key;
    size_t capture_nth = 0;
    size_t count = 0;
};

TEST(CasGcFence, FencesAllShardsMintingAbsentOnes)
{
    /// R3 fences EVERY root shard of every registered namespace - present or ABSENT (spec section 7
    /// R3, decision 2026-06-12). For an absent shard the create-if-absent CAS MINTS a fence-only
    /// manifest: the create race against a first publish into that shard is the required total
    /// order. Fencing only present shards would leave a first publish into an absent shard
    /// unordered against the fence (the absent-shard hole).
    auto b = std::make_shared<CaptureStateBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");
    publishPart(s, "srv1/tbl", "part_2", "payload-2");           /// likely a different root shard
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->armCapture(s->layout().gcStateKey(), 5);                  /// pre-erase point (the recheck CAS)
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_EQ(readState(*b, *s).round, 1u);
    ASSERT_TRUE(b->captured.has_value());
    const GcState st = decodeGcState(*b->captured);              /// gc/state as the recheck saw it
    ASSERT_TRUE(st.fence_version.contains(1));

    /// ALL root_shards manifests now exist, each fenced at round 1 with its committed version
    /// recorded; the minted (previously absent) ones carry empty refs and an empty journal:
    for (uint64_t shard = 0; shard < s->poolMeta().root_shards; ++shard)
    {
        const auto manifest = b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard));
        ASSERT_TRUE(manifest.has_value()) << "shard " << shard << " was not minted by the fence";
        const RootShard root = decodeRootShard(manifest->bytes);
        EXPECT_EQ(root.fence_round, 1u);
        const String key = "srv1/tbl/" + std::to_string(shard);
        ASSERT_TRUE(st.fence_version.at(1).contains(key)) << key;
        /// post-round version >= the fence commit (the trim may bump once more on shards that had
        /// journal records); the captured fence_version is the recheck's fold-through point.
        EXPECT_GE(root.shard_version, st.fence_version.at(1).at(key));
        EXPECT_LE(root.shard_version, st.fence_version.at(1).at(key) + 1);
        /// a MINTED manifest (the fence's create was its first commit) is fence-only:
        if (root.shard_version == 1)
        {
            EXPECT_TRUE(root.refs.empty());
            EXPECT_TRUE(root.journal.empty());
        }
    }

    /// the vector covers all shards + the registry entry:
    EXPECT_EQ(st.fence_version.at(1).size(), s->poolMeta().root_shards + 1);
    EXPECT_TRUE(st.fence_version.at(1).contains("_registry"));
}

TEST(CasGcFence, RegistryFencedAndRecorded)
{
    /// The namespace registry is fenced FIRST (the ordering point for namespace creation): after a
    /// round its fence_round equals the round and the committed registry_version is recorded under
    /// the reserved "_registry" key.
    auto b = std::make_shared<CaptureStateBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->armCapture(s->layout().gcStateKey(), 5);                  /// pre-erase point (the recheck CAS)
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const auto got = b->get(s->layout().rootsRegistryKey());
    ASSERT_TRUE(got.has_value());
    const RootsRegistry registry = decodeRootsRegistry(got->bytes);
    EXPECT_EQ(registry.fence_round, 1u);
    EXPECT_TRUE(registry.namespaces.contains("srv1/tbl"));

    ASSERT_TRUE(b->captured.has_value());
    const GcState st = decodeGcState(*b->captured);
    ASSERT_TRUE(st.fence_version.contains(1));
    EXPECT_EQ(st.fence_version.at(1).at("_registry"), registry.registry_version);
}

/// CaptureStateBackend that additionally runs a one-shot hook just before the first casPut to the
/// armed key (after arming) - the fence's registry CAS attempt. Used to land a namespace
/// registration INSIDE the fence's registry-CAS window.
class HookOnCasCaptureBackend : public CaptureStateBackend
{
public:
    CasOutcome casPut(const String & key, const String & bytes,
                      const std::optional<Token> & expected, Token * out_token = nullptr, const ObjectMeta & meta = {}) override
    {
        if (!fired && key == hook_key)
        {
            fired = true;   /// before the hook - its own casPut on the same key must not re-fire
            hook();
        }
        return CaptureStateBackend::casPut(key, bytes, expected, out_token, meta);
    }

    void armHookOnCasPut(String key, std::function<void()> hook_)
    {
        hook_key = std::move(key);
        hook = std::move(hook_);
        fired = false;
    }

private:
    String hook_key;
    std::function<void()> hook;
    bool fired = true;
};

TEST(CasGcFence, FenceUniverseIsFenceTimeRegistryNotFoldTime)
{
    /// THE FENCE-UNIVERSE HOLE (found designing the model's registry encoding, fixed 2026-06-12):
    /// the shard-fence universe must come from the registry decoded in the COMMITTED registry-fence
    /// attempt, never from the fold-time universe. A namespace registered in the window between the
    /// fold's registry read and the registry-fence CAS falls between the two horns otherwise:
    /// registered BELOW the registry fence, so its writer never observes a fence_round floor
    /// (horn 2 misses), yet absent from the fold-time universe, so its shards are never minted or
    /// fenced and the recheck never folds their journals (horn 1 misses) - a stale-view publish
    /// into it could re-reference a condemned hash past the recheck and the exact-token delete
    /// would dangle a live ref.
    ///
    /// Stage the registration exactly in that window: the hook fires just before the fence's
    /// registry CAS attempt (the fold already read the registry), registers "late/ns" raw, and the
    /// fence's CAS then conflicts, re-reads (now seeing "late/ns"), and commits - the committed
    /// registry MUST be the universe the shard loop fences.
    auto b = std::make_shared<HookOnCasCaptureBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->armCapture(s->layout().gcStateKey(), 5);                  /// pre-erase point (the recheck CAS)
    b->armHookOnCasPut(s->layout().rootsRegistryKey(), [&]
    {
        DB::Cas::tests::registerNamespaceRaw(*b, s->layout(), RootNamespace{"late/ns"});
    });
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// the committed registry carries both namespaces and the round's fence:
    const auto got = b->get(s->layout().rootsRegistryKey());
    ASSERT_TRUE(got.has_value());
    const RootsRegistry registry = decodeRootsRegistry(got->bytes);
    EXPECT_EQ(registry.fence_round, 1u);
    EXPECT_TRUE(registry.namespaces.contains("late/ns"));

    ASSERT_TRUE(b->captured.has_value());
    const GcState st = decodeGcState(*b->captured);              /// gc/state as the recheck saw it
    ASSERT_TRUE(st.fence_version.contains(1));

    /// EVERY shard of the late namespace was minted fence-only at THIS round and its committed
    /// version recorded - so the recheck's fold-through-fence covers it and a later writer's gate
    /// sees the floor:
    for (uint64_t shard = 0; shard < s->poolMeta().root_shards; ++shard)
    {
        const auto manifest = b->get(s->layout().rootShardKey(RootNamespace{"late/ns"}, shard));
        ASSERT_TRUE(manifest.has_value()) << "late/ns shard " << shard << " was not minted by the fence";
        const RootShard root = decodeRootShard(manifest->bytes);
        EXPECT_EQ(root.fence_round, 1u);
        EXPECT_TRUE(root.refs.empty());
        EXPECT_TRUE(root.journal.empty());
        EXPECT_TRUE(st.fence_version.at(1).contains("late/ns/" + std::to_string(shard)))
            << "late/ns shard " << shard << " missing from fence_version[1]";
    }
}

TEST(CasGcDiscovery, UsesRegistryNotList)
{
    /// GC discovers namespaces FROM the registry, never LIST: a manifest written for an
    /// UNREGISTERED namespace (an out-of-model fixture artifact - production writers always
    /// register via W-REGISTER) is invisible to the round - not folded, not fenced.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    publishPart(s, "srv1/tbl", "part_1", "payload-1");           /// registered via Build::publish

    /// bypass registerNamespaceRaw deliberately: a manifest for a ghost namespace
    RootShard ghost;
    ghost.shard_version = 1;
    ghost.refs["ghost_part"] = RefPayload{.tree_id = u128Of("ghost-tree"), .tree_size = 0, .mutable_files = {}};
    ghost.journal.push_back(JournalRecord{
        .op = JournalRecord::Op::Add, .ref_name = "ghost_part", .tree_id = u128Of("ghost-tree"), .at_version = 1});
    b->casPut(s->layout().rootShardKey(RootNamespace{"ghost/ns"}, 0), encodeRootShard(ghost), std::nullopt);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    /// not folded: no edge targeting the ghost tree in the snap
    const GcState st = readState(*b, *s);
    const auto snap = decodeGcSnap(b->get(s->layout().gcSnapKey(st.snap_generation, 0))->bytes);
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, u128Of("ghost-tree")), 0u);
    EXPECT_FALSE(snap.isKnown(ObjectKind::Tree, u128Of("ghost-tree")));
    /// not fenced: the ghost manifest still carries fence_round 0
    const RootShard after = decodeRootShard(b->get(s->layout().rootShardKey(RootNamespace{"ghost/ns"}, 0))->bytes);
    EXPECT_EQ(after.fence_round, 0u);
}

TEST(CasGcFence, MonotoneNeverLowers)
{
    /// The model's monotone fence (GFenceShard / INV-MONOTONE-GC): a manifest already fenced at a
    /// HIGHER round (as a newer leader would leave behind) must never be lowered by a stale
    /// leader's lower round - the max absorbs it. A later round above it raises it again.
    auto b = std::make_shared<CaptureStateBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload");

    /// hand-set the manifest's fence_round to 5 via a raw CAS (read, edit, casPut back):
    const String shard_key = s->layout().rootShardKey(
        RootNamespace{"srv1/tbl"}, shardOfForTest("part_1", s->poolMeta().root_shards));
    const auto got = b->get(shard_key);
    ASSERT_TRUE(got.has_value());
    RootShard staged = decodeRootShard(got->bytes);
    staged.fence_round = 5;
    ++staged.shard_version;
    ASSERT_EQ(b->casPut(shard_key, encodeRootShard(staged), got->token), CasOutcome::Committed);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->armCapture(s->layout().gcStateKey(), 5);                  /// pre-erase point (the recheck CAS)
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);            /// round 1 < 5

    const RootShard after1 = decodeRootShard(b->get(shard_key)->bytes);
    EXPECT_EQ(after1.fence_round, 5u);                           /// NOT lowered to 1
    /// the recorded fence_version still records the commit (the version bump happened):
    ASSERT_TRUE(b->captured.has_value());
    const GcState st1 = decodeGcState(*b->captured);
    const String key = "srv1/tbl/" + std::to_string(shardOfForTest("part_1", s->poolMeta().root_shards));
    ASSERT_TRUE(st1.fence_version.contains(1));
    /// the recorded version is the fence's own commit; the post-round manifest may be one above it
    /// (the trim's bump on a shard that had journal records).
    EXPECT_GE(after1.shard_version, st1.fence_version.at(1).at(key));
    EXPECT_LE(after1.shard_version, st1.fence_version.at(1).at(key) + 1);

    /// rounds 2..5 keep absorbing into 5; round 6 raises it:
    for (int i = 0; i < 5; ++i)
        ASSERT_TRUE(gc.runRegularRound().acquired_lease);
    EXPECT_EQ(readState(*b, *s).round, 6u);
    EXPECT_EQ(decodeRootShard(b->get(shard_key)->bytes).fence_round, 6u);
}

TEST(CasGcFence, FenceBumpAppendsNoJournalRecord)
{
    /// The fence is a version bump with NO journal record - a fold no-op (the fold reads records,
    /// not versions) and harmless to INV-JOURNAL-COVERAGE (trim is gated on at_version <= cursor;
    /// a fence bump above the cursor leaves no record to trim). Pin: after a round, each fenced
    /// manifest carries ONLY the publish/drop records, its shard_version advanced by exactly 1
    /// over the pre-fence version (the fence's own CAS), and every journal at_version still sits
    /// at or below the PRE-fence shard_version.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    /// pre-round (= pre-fence) snapshot of every present manifest:
    std::map<uint64_t, RootShard> pre;
    for (uint64_t shard = 0; shard < s->poolMeta().root_shards; ++shard)
        if (const auto m = b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard)))
            pre.emplace(shard, decodeRootShard(m->bytes));
    ASSERT_FALSE(pre.empty());

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    for (const auto & [shard, before] : pre)
    {
        const RootShard after = decodeRootShard(
            b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard))->bytes);
        EXPECT_EQ(after.fence_round, 1u);
        /// fence bump (+1) and, on shards that had records, the trim's bump (+1) - and NEITHER
        /// appended a journal record: every surviving record predates the round (none here - the
        /// trim dropped all records at or below the durable cursor).
        EXPECT_EQ(after.shard_version, before.shard_version + 2);
        EXPECT_TRUE(after.journal.empty());
        for (const JournalRecord & record : after.journal)
            EXPECT_LE(record.at_version, before.shard_version);      /// no round-minted records
    }
}

TEST(CasGcFence, FenceCasConflictRetriesAndRecordsFinalCommit)
{
    /// Horn 1 retry semantics, pinned mechanically: an injected one-shot Conflict on the fence's
    /// manifest CAS forces mutateShard's re-read + retry on the FRESH manifest; the fence still
    /// lands and the recorded fence_version reflects the FINAL commit. (The TRUE interleaved horn -
    /// a real publish committing in the fence window, folded by the recheck and SPARED - is pinned
    /// mechanically by CasGcRecheck.SparedWhenPublishRacesTheFence via the on-fence hook.)
    auto b = std::make_shared<CaptureStateBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const String shard_key = s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard);
    b->failNextCasPut(shard_key);                                /// the fence CAS conflicts once

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->armCapture(s->layout().gcStateKey(), 5);                  /// pre-erase point (the recheck CAS)
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RootShard root = decodeRootShard(b->get(shard_key)->bytes);
    EXPECT_EQ(root.fence_round, 1u);                             /// the fence landed despite the conflict
    EXPECT_EQ(root.shard_version, 3u);                           /// publish = 1; the conflicted attempt
                                                                 /// committed nothing; the fence retry
                                                                 /// = 2; the trim = 3
    ASSERT_TRUE(b->captured.has_value());
    const GcState st = decodeGcState(*b->captured);
    ASSERT_TRUE(st.fence_version.contains(1));
    EXPECT_EQ(st.fence_version.at(1).at("srv1/tbl/" + std::to_string(shard)), 2u);   /// the FINAL commit
}

TEST(CasGcRetire, RetireReplayAdoptsOwnCrashedAttempt)
{
    /// H2 (review of ce7df279563): crash after the sets are durable but before the round CAS, then
    /// NEW journal activity before the replay => the replayed round derives DIFFERENT bytes for the
    /// SAME retiredKey(round, fence_seq, shard). With the fold-committed state threaded (H1) and
    /// fence_seq isolation, a divergent occupant at OUR path can only be OUR OWN crashed prior
    /// attempt: it must be ADOPTED (write-once preserved), never aborted - renewal never bumps
    /// fence_seq, so aborting would re-hit the same occupied path on EVERY later round of this
    /// leader (a permanent wedge). The candidate the occupant lacks is NOT lost: zeroInDegreeKnown
    /// is stateless, the following round re-derives and retires it.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId t1 = publishPart(s, "srv1/tbl", "part_1", "one");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 3);            /// lease=1, fold=2, retire=3
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });
    ASSERT_TRUE(b->get(s->layout().retiredKey(1, 0, 0)).has_value());   /// the crashed attempt's set

    /// new journal activity changes the would-be round-1 set (T2 also zeroes):
    const TreeId t2 = publishPart(s, "srv1/tbl", "part_2", "two");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_2");

    const RoundReport rep = gc.runRegularRound();             /// must NOT wedge
    EXPECT_TRUE(rep.acquired_lease);
    GcState st = readState(*b, *s);
    EXPECT_EQ(st.round, 1u);                                  /// the round landed
    EXPECT_EQ(rep.candidates, 1u);                            /// reports the ADOPTED durable truth
    /// ADOPTION PROOF: T2 had already zeroed before the rerun, so a non-adopting rerun would have
    /// processed TWO entries; the round-1 outcome log carrying exactly ONE (T1) proves the
    /// occupant set was adopted, write-once preserved. The round then COMPLETED: T1 deleted,
    /// retired set dropped on the confirmed outcome.
    const OutcomeLog log1 = decodeOutcomeLog(b->get(s->layout().outcomesKey(1, st.fence_seq, 0))->bytes);
    ASSERT_EQ(log1.entries.size(), 1u);
    EXPECT_EQ(log1.entries[0].hash, hexToU128(t1.string()));
    EXPECT_EQ(log1.entries[0].outcome, OutcomeKind::Deleted);
    EXPECT_FALSE(b->head(s->layout().treeKey(t1)).exists);
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());

    /// T2 is not lost - the FOLLOWING round re-derives, retires, and deletes it:
    const RoundReport rep2 = gc.runRegularRound();
    EXPECT_TRUE(rep2.acquired_lease);
    st = readState(*b, *s);
    EXPECT_EQ(st.round, 2u);
    const OutcomeLog log2 = decodeOutcomeLog(b->get(s->layout().outcomesKey(2, st.fence_seq, 0))->bytes);
    bool t2_deleted = false;
    for (const OutcomeEntry & entry : log2.entries)
        t2_deleted |= entry.hash == hexToU128(t2.string()) && entry.outcome == OutcomeKind::Deleted;
    EXPECT_TRUE(t2_deleted);
    EXPECT_FALSE(b->head(s->layout().treeKey(t2)).exists);
}

TEST(CasGcCascade, SharedChildSurvivesOneParentDeletion)
{
    /// B is referenced by TWO live trees; dropping one part deletes its tree, but the strip leaves
    /// B's in-degree at 1 (the other tree's edge) - B is never a candidate while any parent lives.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    /// two parts, different trees (different entry names), SAME blob payload => shared B:
    auto build = s->startBuild({});
    build->putBlob(idOf("shared"), BlobSource::fromString("shared"));
    TreeEntry e1; e1.name = "a.bin"; e1.placement = Placement::Blob; e1.file_hash = u128Of("shared"); e1.file_size = 6;
    TreeEntry e2; e2.name = "b.bin"; e2.placement = Placement::Blob; e2.file_hash = u128Of("shared"); e2.file_size = 6;
    const TreeId t1 = build->putTree({e1});
    const TreeId t2 = build->putTree({e2});
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", t1, RefPayload{});
    build->publish(RootNamespace{"srv1/tbl"}, "part_2", t2, RefPayload{});
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep1 = gc.runRegularRound();
    EXPECT_EQ(rep1.deleted, 1u);                              /// t1 only
    EXPECT_FALSE(b->head(s->layout().treeKey(t1)).exists);
    EXPECT_TRUE(b->head(s->layout().treeKey(t2)).exists);

    const RoundReport rep2 = gc.runRegularRound();
    EXPECT_EQ(rep2.candidates, 0u);                           /// B still pinned by t2
    EXPECT_TRUE(b->head(s->layout().blobKey(idOf("shared"))).exists);
    /// part_2 still reads end-to-end (INV-NO-DANGLE):
    auto resolved = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_2");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(s->readTree(resolved->tree_id).size(), 1u);
}

TEST(CasGcCascade, NeverCascadesOnReplaced)
{
    /// A resurrection winning the race (412 => Replaced) must NOT cascade: the new incarnation is
    /// payload-identical and pins its children; its own lifecycle handles them (spec section 7).
    auto b = std::make_shared<OnFenceHookBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const String tree_key = s->layout().treeKey(tree);
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    b->armOnCasPut(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard), [&]
    {
        displaceObjectToken(*b, tree_key, ObjectKind::Tree);  /// the racing resurrect
    });

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep1 = gc.runRegularRound();
    EXPECT_EQ(rep1.replaced, 1u);
    EXPECT_EQ(rep1.cascaded, 0u);                             /// THE claim: no strip on Replaced
    /// the new incarnation and its still-edge-pinned child both survive the 412 round:
    EXPECT_TRUE(b->head(tree_key).exists);
    EXPECT_TRUE(b->head(s->layout().blobKey(idOf("payload"))).exists);

    /// A bare resurrect with NO republish leaves the new incarnation just as unreachable: the next
    /// round legitimately retires it at its NEW token and deletes it (the 412-save matters when
    /// the resurrect is part of a publish - the gate's resurrect-then-publish raises in-degree and
    /// the recheck spares; here nobody published). The strip then runs on the CONFIRMED delete.
    const RoundReport rep2 = gc.runRegularRound();
    EXPECT_EQ(rep2.deleted, 1u);
    EXPECT_EQ(rep2.cascaded, 1u);
    EXPECT_FALSE(b->head(tree_key).exists);

    /// and the freed child goes one round later (LIVE-RECLAIM):
    const RoundReport rep3 = gc.runRegularRound();
    EXPECT_EQ(rep3.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().blobKey(idOf("payload"))).exists);
}

TEST(CasGcCascade, RecreateAfterDeletionRefoldsAfterStrip)
{
    /// The cascade-vs-recreate race (spec section 7; the model's SabotageCascadeRace negative
    /// control breaks exactly this): a re-create-and-publish of the SAME tree content lands at a
    /// shard_version ABOVE the deleting round's fence version, so its Add folds in the NEXT round
    /// on a snap whose marker the strip already CLEARED - the re-expansion re-pins the children.
    /// (Persisting the strip lazily, or folding the Add against a still-set marker, would leave
    /// the recreated live tree's children edge-less => a later wrong delete.)
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep1 = gc.runRegularRound();            /// deletes T, strips, frees B
    EXPECT_EQ(rep1.deleted, 1u);
    EXPECT_EQ(rep1.cascaded, 1u);

    /// the recreate: same content => same hashes; both objects re-uploaded fresh; the publish
    /// lands above round 1's fence version on the shard.
    const TreeId recreated = publishPart(s, "srv1/tbl", "part_1", "payload");
    ASSERT_EQ(recreated, tree);                               /// content-addressed identity

    const RoundReport rep2 = gc.runRegularRound();            /// folds the Add post-strip
    EXPECT_EQ(rep2.candidates, 0u);                           /// B re-pinned by the re-expansion
    EXPECT_EQ(rep2.deleted, 0u);

    /// the part reads end-to-end; nothing was wrongly deleted:
    auto resolved = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_TRUE(b->head(s->layout().blobKey(idOf("payload"))).exists);
    EXPECT_TRUE(b->head(s->layout().treeKey(tree)).exists);

    /// and a further idle round stays empty (the dead-then-recreated subgraph is stable):
    const RoundReport rep3 = gc.runRegularRound();
    EXPECT_EQ(rep3.candidates + rep3.deleted, 0u);
}

TEST(CasGcTrim, DropsRecordsAtOrBelowTheDurableCursor)
{
    /// INV-JOURNAL-COVERAGE end-to-end: after a round, every record the durable cursor covers is
    /// gone from the manifest journal; refs are untouched; only the GC's recordless version bumps
    /// remain on top.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    publishPart(s, "srv1/tbl", "part_1", "payload-1");        /// Add at v1
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");          /// Remove at v2
    publishPart(s, "srv1/tbl", "part_keep", "payload-2");     /// Add at v1 of ITS shard

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    for (uint64_t shard = 0; shard < s->poolMeta().root_shards; ++shard)
    {
        const RootShard root = decodeRootShard(
            b->get(s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard))->bytes);
        EXPECT_TRUE(root.journal.empty()) << "shard " << shard;   /// every record was folded => trimmed
    }
    /// refs untouched by the trim:
    EXPECT_TRUE(s->resolveRef(RootNamespace{"srv1/tbl"}, "part_keep").has_value());
    EXPECT_FALSE(s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1").has_value());
}

TEST(CasGcTrim, RecordAboveTheCursorSurvives)
{
    /// THE BOUNDARY, mechanically: a publish landing between the cascade's cursor CAS and the trim
    /// puts a record ABOVE the durable cursor - the trim's own CAS conflicts, re-reads, and must
    /// retain exactly that record while dropping the covered ones. (Trimming it would violate
    /// INV-JOURNAL-COVERAGE: the durable snap does not incorporate it yet.)
    auto b = std::make_shared<OnFenceHookBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);
    const String shard_key = s->layout().rootShardKey(RootNamespace{"srv1/tbl"}, shard);
    /// fire before the TRIM's casPut (the 2nd to this shard: fence = 1st):
    b->armOnNthCasPut(shard_key, 2, [&]
    {
        const auto got = b->InMemoryBackend::get(shard_key);
        ASSERT_TRUE(got.has_value());
        RootShard root = decodeRootShard(got->bytes);
        ++root.shard_version;
        root.refs["late_part"] = RefPayload{.tree_id = u128Of("late-tree"), .tree_size = 0, .mutable_files = {}};
        root.journal.push_back(JournalRecord{
            .op = JournalRecord::Op::Add, .ref_name = "late_part",
            .tree_id = u128Of("late-tree"), .at_version = root.shard_version});
        ASSERT_EQ(b->InMemoryBackend::casPut(shard_key, encodeRootShard(root), got->token, nullptr),
                  CasOutcome::Committed);
    });

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    const RootShard after = decodeRootShard(b->get(shard_key)->bytes);
    ASSERT_EQ(after.journal.size(), 1u);                      /// the covered records dropped...
    EXPECT_EQ(after.journal[0].ref_name, "late_part");        /// ...the uncovered one SURVIVED
    EXPECT_TRUE(after.refs.contains("late_part"));
}

TEST(CasGcResume, CompletesRoundAfterCrashBeforeFencePersist)
{
    /// Crash window: retire's round CAS landed (.round = 1, sets durable) but the fence's gc/state
    /// CAS lost (fence_version[1] never persisted). The next call RESUMES round 1 from durable
    /// state: sets present => incomplete; fence_version missing => re-fence (monotone max,
    /// idempotent); recheck deletes; cascade persists; sets drop. Only then does a fresh round run.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 4);            /// lease=1, fold=2, retire=3, FENCE=4
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });

    GcState st = readState(*b, *s);
    ASSERT_EQ(st.round, 1u);                                  /// retire completed...
    ASSERT_FALSE(st.fence_version.contains(1));               /// ...the fence vector did not
    ASSERT_TRUE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());

    const RoundReport resumed = gc.runRegularRound();         /// the RESUME
    EXPECT_TRUE(resumed.acquired_lease);
    EXPECT_EQ(resumed.round, 1u);                             /// the resumed round, not a new one
    EXPECT_EQ(resumed.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().treeKey(tree)).exists);
    st = readState(*b, *s);
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());   /// sets dropped

    const RoundReport next = gc.runRegularRound();            /// a fresh round follows
    EXPECT_EQ(next.round, 2u);
}

TEST(CasGcResume, AdoptsOutcomesAfterCrashBeforeCascadePersist)
{
    /// Crash window: the deletes LANDED and the outcome logs are durable, but the cascade's
    /// gc/state CAS lost - sets still present, strips unpersisted. The resume re-runs the recheck
    /// (the re-delete sees NotFound) and ADOPTS the existing outcome log as the durable truth
    /// (Deleted, not Absent), strips from it, persists, and drops the sets. The freed child is
    /// then reclaimed by the NEXT round - the cascade survives the crash.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    b->failNthCasPut(s->layout().gcStateKey(), 5);            /// the CASCADE persist CAS
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });

    GcState st = readState(*b, *s);
    ASSERT_EQ(st.round, 1u);
    ASSERT_TRUE(st.fence_version.contains(1));                /// fence persisted...
    ASSERT_FALSE(b->head(s->layout().treeKey(tree)).exists);  /// ...and the delete LANDED
    ASSERT_TRUE(b->get(s->layout().outcomesKey(1, st.fence_seq, 0)).has_value());
    ASSERT_TRUE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());   /// sets linger

    const RoundReport resumed = gc.runRegularRound();
    EXPECT_EQ(resumed.round, 1u);
    EXPECT_EQ(resumed.deleted, 1u);                           /// ADOPTED from the durable log
    EXPECT_EQ(resumed.absent, 0u);                            /// NOT re-tallied as Absent
    st = readState(*b, *s);
    EXPECT_FALSE(b->get(s->layout().retiredKey(1, st.fence_seq, 0)).has_value());
    EXPECT_FALSE(st.fence_version.contains(1));

    /// the strip survived the crash: the freed child goes next round (LIVE-RECLAIM):
    const RoundReport next = gc.runRegularRound();
    EXPECT_EQ(next.round, 2u);
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().blobKey(idOf("payload"))).exists);
}

TEST(CasGcScenario, ZombieDeleteAfterResurrectIs412)
{
    /// THE IN-FLIGHT DISJUNCTION + INV-NO-RETURN, with a genuinely held (in-flight) delete: the
    /// recheck SENDS the delete (the backend accepts but defers it - the model's inflight set);
    /// a writer then resurrects and republishes the same content; the zombie delete LANDS later
    /// carrying the OLD observed token and MUST 412 against the new incarnation. "A delete in
    /// flight implies its entry is held OR its token already displaced" - here the resurrect
    /// displaced it first, so the landing is forever harmless.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");
    const String tree_key = s->layout().treeKey(tree);
    const Token t0 = b->head(tree_key).token;

    b->setHoldDeletes(true);                                  /// every deleteExact is now in-flight
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    const RoundReport rep = gc.runRegularRound();
    EXPECT_EQ(rep.deleted, 1u);                               /// the SEND was accepted...
    EXPECT_TRUE(b->head(tree_key).exists);                    /// ...but nothing landed yet
    EXPECT_GT(b->pendingDeletes(), 0u);

    /// The writer republishes the same part. The held retired-set drop means the set is STILL
    /// listed, so the gate's fence-advanced refresh (manifest fence_round 1 > view round 0) sees T
    /// condemned at t0 and RESURRECTS (fresh tag => t1) before publishing - exactly the
    /// displacement the disjunction requires. (Reuse the original Store: a fresh open would run
    /// the capability probe, which CORRECTLY fails the pool while deletes are held.)
    auto build = s->startBuild({});
    build->putBlob(idOf("payload"), BlobSource::fromString("payload"));
    TreeEntry e; e.name = "f"; e.placement = Placement::Blob; e.file_hash = u128Of("payload"); e.file_size = 7;
    const TreeId again = build->putTree({e});
    ASSERT_EQ(again, tree);
    build->publish(RootNamespace{"srv1/tbl"}, "part_1", tree, RefPayload{});
    const Token t1 = b->head(tree_key).token;
    EXPECT_NE(t1, t0);                                        /// the gate-forced resurrect displaced t0

    /// Now every in-flight delete lands - token re-evaluated at LAND time:
    bool tree_delete_was_412 = false;
    while (b->pendingDeletes() > 0)
    {
        const DeleteOutcome landed = b->landPendingDelete(0);
        if (landed.kind == DeleteOutcome::Kind::TokenMismatch)
            tree_delete_was_412 = true;
    }
    EXPECT_TRUE(tree_delete_was_412);                         /// the zombie hit the displaced token
    EXPECT_TRUE(b->head(tree_key).exists);                    /// the LIVE incarnation survived
    EXPECT_EQ(b->head(tree_key).token, t1);

    /// INV-NO-DANGLE end-to-end:
    b->setHoldDeletes(false);
    auto resolved = s->resolveRef(RootNamespace{"srv1/tbl"}, "part_1");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(s->readTree(resolved->tree_id).size(), 1u);
}

TEST(CasGcScenario, SplitBrainLeadersOnlyDuplicateWork)
{
    /// A stalled leader's epoch is abandoned, never corrupted: gc1 crashes mid-round (after retire,
    /// before the fence persists); gc2 observes the stall twice and STEALS (fence_seq bumps - a new
    /// epoch whose retire/outcome paths never collide with gc1's). gc2's resume does NOT see gc1's
    /// orphan sets (different fence_seq - the documented epoch-orphan, conservative: it only
    /// condemns until M-F cleans it) and runs its own rounds; the dropped part's objects are
    /// deleted exactly once, by gc2. The revived gc1 backs off (foreign owner). No double delete,
    /// no loss, monotone state throughout.
    auto b = std::make_shared<FailNthCasPutBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    const TreeId tree = publishPart(s, "srv1/tbl", "part_1", "payload");
    s->dropRef(RootNamespace{"srv1/tbl"}, "part_1");

    Gc gc1(s, hexToU128("0000000000000000000000000000000a"));
    Gc gc2(s, hexToU128("0000000000000000000000000000000b"));

    b->failNthCasPut(s->layout().gcStateKey(), 4);            /// gc1 dies at the fence persist
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc1.runRegularRound(); });
    GcState st = readState(*b, *s);
    ASSERT_EQ(st.round, 1u);
    ASSERT_EQ(st.fence_seq, 0u);
    ASSERT_TRUE(b->get(s->layout().retiredKey(1, 0, 0)).has_value());   /// gc1's epoch-0 set

    EXPECT_FALSE(gc2.runRegularRound().acquired_lease);       /// observe #1 (gc1 now silent)
    const RoundReport stolen = gc2.runRegularRound();         /// observe #2 => steal => epoch 1
    EXPECT_TRUE(stolen.acquired_lease);
    st = readState(*b, *s);
    EXPECT_EQ(st.fence_seq, 1u);

    /// gc2's rounds collect the dropped part in ITS epoch (T still zero in the durable snap):
    RoundReport rep = stolen;
    for (int i = 0; rep.deleted == 0 && i < 3; ++i)
        rep = gc2.runRegularRound();
    EXPECT_EQ(rep.deleted, 1u);
    EXPECT_FALSE(b->head(s->layout().treeKey(tree)).exists);

    /// gc1 revives: foreign owner => back off; its orphan set still lingers (epoch isolation):
    EXPECT_FALSE(gc1.runRegularRound().acquired_lease);
    EXPECT_TRUE(b->get(s->layout().retiredKey(1, 0, 0)).has_value());
    /// and the lingering entry is CONSERVATIVE - it condemns t0, an incarnation that no longer
    /// exists; a delete for it would 412 or 404. No path ever deletes a live object twice.
}

TEST(CasGcRound, PreviewDeletesIsWriteFreeAndSubsetOfUnreachable)
{
    using namespace DB::Cas;
    using DB::Cas::tests::idOf;
    using DB::Cas::tests::u128Of;

    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(7);
    auto store = Store::open(backend, cfg);

    /// Publish one part, then drop it (ref gone; tree+blob await GC).
    {
        auto build = store->startBuild({});
        build->putBlob(idOf("ghost"), BlobSource::fromString("ghost"));
        TreeEntry e; e.name = "data.bin"; e.placement = Placement::Blob;
        e.file_hash = u128Of("ghost"); e.file_size = 5;
        auto tree = build->putTree({e});
        build->publish(RootNamespace{"uui/uuid-1"}, "all_1_1_0", tree, {});
    }
    store->dropRef(RootNamespace{"uui/uuid-1"}, "all_1_1_0");

    /// One real round folds the drop and persists the snap/state previewDeletes reads.
    Gc gc(store, DB::UInt128(123));
    gc.runRegularRound();

    auto countObjects = [&]() -> size_t
    {
        size_t n = 0; String cursor;
        while (true)
        {
            auto page = backend->list("pool", cursor, 1000);
            n += page.keys.size();
            if (page.next_cursor.empty()) break;
            cursor = page.next_cursor;
        }
        return n;
    };

    const size_t before = countObjects();
    const auto preview = gc.previewDeletes();
    EXPECT_EQ(countObjects(), before);   /// write-free

    /// Every previewed delete key must be unreachable per the independent fsck (the safety subset).
    const FsckReport report = runFsck(*store, /*detail=*/true);
    std::set<String> unreachable;
    for (const auto & o : report.objects)
        if (o.cls == FsckClass::Unreachable)
            unreachable.insert(o.key);
    for (const auto & p : preview)
        EXPECT_TRUE(unreachable.contains(p.key)) << "preview delete not unreachable: " << p.key;
}

/// ---- A1 fold: skip-unchanged persist (Task 5, Pillar A1) ----
///
/// When no new journal records exist in any shard (shard_version == folded_cursor for every shard),
/// the snap is UNCHANGED and already durable at state.snap_generation — the fold must NOT write a
/// new snap generation or advance the gc/state CAS. This keeps idle rounds (churn-free pools, churn-
/// free windows) completely write-free on the snap/state side.

TEST(CasGcFold, NoChurnRoundWritesNoNewSnap)
{
    auto b = std::make_shared<tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 1 folds the publish (snap PUT happens)
    const GcState st1 = readState(*b, *s);

    b->resetCounts();
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 2: no new records — no new snap
    const GcState st2 = readState(*b, *s);

    EXPECT_EQ(st2.snap_generation, st1.snap_generation);                              /// generation unchanged
    EXPECT_EQ(b->putCount(s->layout().gcSnapKey(st1.snap_generation + 1, 0)), 0u);    /// no probe write
}

/// ---- A1 resident-snap read-cache (Task 6, Pillar A1) ----
///
/// When the durable snap generation is unchanged between rounds, the fold skips the per-round
/// loadSnap GET and reuses the in-memory resident snap instead. The generation is WRITE-ONCE
/// (putIfAbsent + byte-equal adoption), so a matching generation guarantees identical bytes —
/// no HEAD/GET needed, no explicit invalidation required.

TEST(CasGcFold, NoChurnRoundReusesResidentSnapNoGet)
{
    auto b = std::make_shared<tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 1: folds, snap now resident
    const GcState st1 = readState(*b, *s);

    b->resetCounts();
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 2: no churn — reuse resident snap
    EXPECT_EQ(b->getCount(s->layout().gcSnapKey(st1.snap_generation, 0)), 0u);   /// snap NOT re-GET
}

/// ---- B167 per-server build watermark guard (Task 9) ----
///
/// protectedByLiveBuild reads the candidate's owner triple ("cas_owner" =
/// "<server_hex>:<epoch>:<build_seq>") and the owning server's watermark {epoch, min_active, seq},
/// protecting the object IFF the server is live this round, the epoch matches, build_seq >=
/// min_active, and the server is not retired.

TEST(CasGcWatermark, ParsesOwnerAndProtectsLiveBuild)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// The Store anchors its OWN server (0x01) watermark on open; we drive a DIFFERENT server (AB)
    /// directly so its watermark key is fresh and putIfAbsent below succeeds.
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "pool", .server_id = DB::UInt128(0x01)});
    Gc gc(store, DB::UInt128(7));

    /// server AB, epoch 9, min_active 5, seq 1.
    ASSERT_EQ(backend->putIfAbsent(
                  store->layout().serverWatermarkKey(u128ToHex(DB::UInt128(0xAB))),
                  encodeServerWatermark({DB::UInt128(0xAB), 9, 5, 1})),
              PutOutcome::Done);

    const ObjectMeta live{{"cas_owner", u128ToHex(DB::UInt128(0xAB)) + ":9:7"}};   /// seq 7 >= 5 -> protected
    const ObjectMeta done{{"cas_owner", u128ToHex(DB::UInt128(0xAB)) + ":9:3"}};   /// seq 3 < 5 -> unprotected
    const ObjectMeta stale{{"cas_owner", u128ToHex(DB::UInt128(0xAB)) + ":1:7"}};  /// wrong epoch -> unprotected

    gc.beginWatermarkRound();
    ASSERT_TRUE(gc.protectedByLiveBuild(live));     /// first-seen server -> live; 7 >= 5; epoch matches
    ASSERT_FALSE(gc.protectedByLiveBuild(done));
    ASSERT_FALSE(gc.protectedByLiveBuild(stale));
}
