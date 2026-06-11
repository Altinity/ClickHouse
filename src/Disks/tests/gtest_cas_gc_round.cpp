#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <map>
#include <mutex>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int FILE_DOESNT_EXIST;
}

using namespace DB::Cas;
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
                      Token * out_token = nullptr) override
    {
        {
            std::lock_guard lock(fail_mutex);
            if (fail_at != 0 && key == fail_key && ++seen == fail_at)
            {
                fail_at = 0;                                     /// one-shot
                return CasOutcome::Conflict;
            }
        }
        return InMemoryBackend::casPut(key, bytes, expected, out_token);
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

TEST(CasGcFold, ConcurrentDivergentSnapAborts)
{
    /// Write-once generations: if the gen-1 object already exists with DIFFERENT bytes, a competing
    /// fold saw different journals - this round must abort gracefully (ABORTED) leaving the OLD
    /// generation authoritative; the next round re-derives from the then-authoritative state.
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    publishPart(s, "srv1/tbl", "part_1", "payload");
    ASSERT_EQ(b->putIfAbsent(s->layout().gcSnapKey(1, 0), "not-our-fold"), PutOutcome::Done);

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { gc.runRegularRound(); });
    EXPECT_EQ(readState(*b, *s).snap_generation, 0u);            /// old generation stays authoritative
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
    EXPECT_EQ(rep.candidates, 1u);                               /// T zeroed by the Remove

    const GcSnap snap = readSnap(*b, *s, 1, 0);
    EXPECT_FALSE(snap.isExpanded(tree_hash));                    /// expansion skipped
    EXPECT_FALSE(snap.isKnown(ObjectKind::Blob, u128Of("payload")));  /// no child edges added
    EXPECT_EQ(snap.inDegree(ObjectKind::Tree, tree_hash), 0u);
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
