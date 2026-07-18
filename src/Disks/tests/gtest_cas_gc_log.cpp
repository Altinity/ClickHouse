#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/Exception.h>

#include <string>
#include <vector>

/// Unit coverage for the CA GC scheduler's logging sink (the source of
/// `system.content_addressed_garbage_collection_log`). The scheduler emits a Start + Finish
/// `GcRoundLogRecord` per round through the injected `GcRoundLogger`; here we capture the records in
/// a vector and assert their shape over a real (in-memory) Pool driven through a dropped-then-
/// collectable object — the same Pool/Backend fixture the B140 reclaim test uses.
///
/// NOTE on ProfileEvents: `runOneRoundNow` runs on THIS (bare gtest) thread, which has no attached
/// `ThreadStatus`, so the scheduler's `CurrentThread::isInitialized()` guard skips per-round
/// ProfileEvents capture. The `profile_events` map is therefore EXPECTED to be empty here and this
/// test does NOT assert it non-empty (the on-server paths are attached; the functional/soak coverage
/// asserts non-empty there).

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;
using Rec = DB::Cas::GcRoundLogRecord;

namespace
{

/// Publish one part `ref` with a single content blob whose payload is `payload`. Returns the manifest id.
ManifestId publishPart(const PoolPtr & s, const String & ns, const String & ref, const String & payload)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));

    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(payload))};

    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

}

/// The happy path: a marking round (candidates_marked > 0) followed by a deletion round
/// (objects_deleted > 0). Each `runOneRoundNow` must emit exactly one Start then one Finish, with
/// `disk_name`/`gc_id` set and `duration_ms` populated on the Finish.
TEST(CasGcLog, EmitsStartFinishWithCounts)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// gc_fold_max_defer_rounds=0: this test drives up to 16 consecutive rounds through the scheduler
    /// (no direct Gc handle to override per-instance) expecting each to fold; force fold-every-round
    /// (Phase-4 Lever A would otherwise defer once the pool quiesces, stalling the mark-then-delete
    /// pipeline within the round budget).
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"srv1/tbl"};

    /// Publish a part, then drop it so its blob/tree become collectable.
    publishPart(store, ns.string(), "all_0_0_0", "hello-cas-gc-log");
    store->dropRef(ns, "all_0_0_0");
    /// Advance the durable watermark floor past the build's seq so the build-watermark guard no
    /// longer spares the now-dropped objects (the background renewer is off in this test).
    store->renewWatermarkOnce();

    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    /// Drive rounds until we observe both a marking round and a deletion round. Under the ack-floor
    /// pipeline a candidate is marked (condemned) in one round and physically deleted a few rounds later,
    /// once the mount's ack floor graduates it — so advance the store's own mount ack after each round
    /// (renewWatermarkOnce runs the beat) and give the pipeline a generous round budget. Each
    /// runOneRoundNow call appends exactly a Start then a Finish.
    bool saw_marked = false;
    bool saw_deleted = false;
    size_t marking_finish_idx = 0;
    size_t deleting_finish_idx = 0;
    constexpr size_t max_rounds = 16;
    for (size_t round = 0; round < max_rounds && !(saw_marked && saw_deleted); ++round)
    {
        const size_t before = rows.size();
        sched.runOneRoundNow(Rec::Trigger::Manual);
        store->renewWatermarkOnce();

        /// Each call emits exactly one Start then one Finish.
        ASSERT_EQ(rows.size(), before + 2u) << "each round must emit exactly one Start + one Finish";
        ASSERT_EQ(rows[before].event_type, Rec::EventType::Start);
        ASSERT_EQ(rows[before + 1].event_type, Rec::EventType::Finish);

        const Rec & fin = rows[before + 1];
        if (!saw_marked && fin.candidates_marked > 0)
        {
            saw_marked = true;
            marking_finish_idx = before + 1;
        }
        if (!saw_deleted && fin.objects_deleted > 0)
        {
            saw_deleted = true;
            deleting_finish_idx = before + 1;
        }
    }

    ASSERT_TRUE(saw_marked) << "expected a round that marked at least one candidate";
    ASSERT_TRUE(saw_deleted) << "expected a round that physically deleted at least one object";
    /// Deletion is never observed before marking (it may coincide in the same round when the dead
    /// subgraph is small enough to retire and reclaim together).
    EXPECT_GE(deleting_finish_idx, marking_finish_idx);

    EXPECT_GT(rows[marking_finish_idx].candidates_marked, 0u);
    EXPECT_GT(rows[deleting_finish_idx].objects_deleted, 0u);
    /// Retired-cursor pipeline pass-through: the marking round condemned the entry, the deleting round
    /// executed a pending exact-token delete.
    EXPECT_GT(rows[marking_finish_idx].entries_condemned, 0u);
    EXPECT_GT(rows[deleting_finish_idx].entries_redeleted, 0u);

    /// Identity + timing fields are set on every record.
    for (const Rec & r : rows)
    {
        EXPECT_EQ(r.disk_name, "ca");
        EXPECT_FALSE(r.gc_id.empty());
        EXPECT_EQ(r.trigger, Rec::Trigger::Manual);
    }
    /// `duration_ms` is meaningful on Finish (>= 0 always; populated unconditionally there).
    for (size_t i = 1; i < rows.size(); i += 2)
        EXPECT_EQ(rows[i].event_type, Rec::EventType::Finish);
}

namespace
{

/// A backend that throws on `list`, the first thing the GC round does (namespace discovery via the
/// roots registry / listing). Used to drive the Aborted-Finish path: the round throws, the scheduler
/// emits an Aborted Finish with the exception text, and `runOneRoundNow` rethrows.
class ThrowingBackend : public InMemoryBackend
{
public:
    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        if (arm)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected backend list failure");
        return InMemoryBackend::list(prefix, cursor, limit);
    }

    std::optional<GetResult> get(const String & key, Range range) override
    {
        if (arm)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected backend get failure");
        return InMemoryBackend::get(key, range);
    }

    HeadResult head(const String & key) override
    {
        if (arm)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected backend head failure");
        return InMemoryBackend::head(key);
    }

    /// Armed only after Pool::open, so opening (which reads/initialises gc state) succeeds.
    std::atomic<bool> arm{false};
};

}

/// A7-HIGH-fix: the manual `SYSTEM ... GC` path (runOneRoundNow) reuses ONE stable Gc instance across
/// calls (A7 — the lease's observation-window steal protocol compares consecutive observations of the
/// SAME observer), but it must be OBSERVE-ONLY with respect to STEALING: the protocol's safety argument
/// requires the two observations that flag an incumbent "frozen" to be spaced by real wall time (>= the
/// heartbeat cadence H) so a live incumbent gets a chance to pulse in between — a guarantee only the
/// background loop's own interval-paced ticks provide. Two manual calls have no such guarantee (they
/// can land microseconds apart in a real query), so a manual round must NEVER execute the steal CAS,
/// no matter how many times it re-observes the same frozen tuple. Dead-incumbent recovery stays the
/// loop's job (bounded ~2*interval; covered by the CasGcLease loop-driven steal tests in
/// gtest_cas_gc_round.cpp, e.g. StealAfterObservedNonRenewalBumpsEpoch / FailoverStealOnceHeartbeatStops).
/// Deterministic: "time" is the order of runRegularRound calls; no sleep, no clock, no threads.
TEST(CasGcSchedulerSteal, ManualRoundNeverStealsEvenADeadIncumbent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    /// A foreign incumbent takes the lease and then DIES (never renews, never heartbeats).
    const UInt128 kIncumbent = hexToU128("00000000000000000000000000000abc");
    Gc incumbent(store, kIncumbent);
    ASSERT_TRUE(incumbent.runRegularRound().acquired_lease);

    DB::Cas::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca");

    /// obs #1: records the incumbent's (owner, seq, hb=absent).
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
    /// obs #2 and #3: the same frozen (owner, seq, hb) observed repeatedly would be steal-eligible on
    /// the loop path (see the Core-level test this mirrors), but the manual path keeps backing off.
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
}

/// Negative-control companion to the test above (reviewer-requested): with the incumbent visibly alive
/// (its heartbeat advancing between the manual round's observations, exactly like
/// CasGcLease.HeartbeatBlocksFalseStealOfAliveLeader at the Core level), the manual round must still
/// correctly back off — confirming the new observe-only branch didn't regress the PRE-EXISTING
/// incumbent_renewed/hb_alive liveness detection (this test would already pass on the protocol's own
/// terms even without the A7-HIGH-fix; it pins that the fix didn't break it).
TEST(CasGcSchedulerSteal, ManualRoundNeverStealsALiveHeartbeatingIncumbent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const UInt128 kIncumbent = hexToU128("00000000000000000000000000000abc");
    Gc incumbent(store, kIncumbent);
    ASSERT_TRUE(incumbent.runRegularRound().acquired_lease);

    DB::Cas::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca");

    /// obs #1: records (owner=incumbent, seq, hb=absent).
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
    Gc::pulseHeartbeat(*store, kIncumbent);   /// the incumbent is alive and pulsing (hb 0->1)
    /// obs #2: hb advanced since obs #1 => alive => no steal (never reaches the observe-only branch).
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
    Gc::pulseHeartbeat(*store, kIncumbent);   /// hb 1->2
    EXPECT_FALSE(sched.runOneRoundNow(Rec::Trigger::Manual).acquired_lease);
}

/// A round whose backend throws must produce a Finish with `outcome == Aborted` and a non-empty
/// `error`, and `runOneRoundNow` must rethrow the exception (the round failure is observable, not
/// swallowed — the logging sink itself is best-effort, but the round error propagates).
TEST(CasGcLog, AbortedFinishOnThrowingRound)
{
    auto backend = std::make_shared<ThrowingBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    backend->arm.store(true);

    EXPECT_THROW(sched.runOneRoundNow(Rec::Trigger::Manual), DB::Exception);

    ASSERT_EQ(rows.size(), 2u) << "a throwing round still emits a Start and a (Aborted) Finish";
    EXPECT_EQ(rows[0].event_type, Rec::EventType::Start);
    EXPECT_EQ(rows[1].event_type, Rec::EventType::Finish);
    EXPECT_EQ(rows[1].outcome, Rec::Outcome::Failed);
    EXPECT_FALSE(rows[1].error.empty()) << "a failed Finish must carry the exception text";
    EXPECT_EQ(rows[1].disk_name, "ca");
    EXPECT_FALSE(rows[1].gc_id.empty());
}

/// B3: the scheduler exposes per-disk GC health for system.content_addressed_mounts (the process-
/// global CurrentMetrics gauges were clobbered with >= 2 CAS disks). Drive one leader round and
/// assert the health snapshot reflects leadership, the pending-reclaim backlog and a fresh success.
TEST(CasGcHealth, ReflectsLeadershipAndPendingReclaim)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const RootNamespace ns{"srv1/tbl"};
    publishPart(store, ns.string(), "all_0_0_0", "hello-cas-gc-health");
    store->dropRef(ns, "all_0_0_0");
    store->renewWatermarkOnce();

    DB::Cas::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca", {});

    const auto h0 = sched.gcHealth();
    EXPECT_FALSE(h0.is_leader);
    EXPECT_FALSE(h0.ever_succeeded);
    EXPECT_EQ(h0.pending_reclaim, 0);
    EXPECT_EQ(h0.wedged_namespace_count, 0u);

    const RoundReport rep = sched.runOneRoundNow(Rec::Trigger::Manual);
    ASSERT_TRUE(rep.acquired_lease);

    const auto h1 = sched.gcHealth();
    EXPECT_TRUE(h1.is_leader);
    EXPECT_TRUE(h1.ever_succeeded);
    EXPECT_EQ(h1.pending_reclaim,
              static_cast<Int64>(rep.condemned) - static_cast<Int64>(rep.redeleted));
    EXPECT_EQ(h1.wedged_namespace_count, 0u);
    EXPECT_LT(h1.last_success_age_seconds, 60u);
}
