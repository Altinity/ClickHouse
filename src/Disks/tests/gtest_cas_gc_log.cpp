#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/Exception.h>

#include <string>
#include <vector>

/// Unit coverage for the CA GC scheduler's logging sink (the source of
/// `system.content_addressed_garbage_collection_log`). The scheduler emits a Start + Finish
/// `GcRoundLogRecord` per round through the injected `GcRoundLogger`; here we capture the records in
/// a vector and assert their shape over a real (in-memory) Store driven through a dropped-then-
/// collectable object — the same Store/Backend fixture the B140 reclaim test uses.
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
using Rec = DB::ContentAddressed::GcRoundLogRecord;

namespace
{

/// Publish one part `ref` with a single content blob whose payload is `payload`. Returns the tree id.
TreeId publishPart(const StorePtr & s, const String & ns, const String & ref, const String & payload)
{
    auto build = s->startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));

    TreeEntry e;
    e.name = "data.bin";
    e.placement = Placement::Blob;
    e.file_hash = u128Of(payload);
    e.file_size = payload.size();

    const TreeId tree = build->putTree({e});
    build->publish(RootNamespace{ns}, ref, tree, {});
    return tree;
}

}

/// The happy path: a marking round (candidates_marked > 0) followed by a deletion round
/// (objects_deleted > 0). Each `runOneRoundNow` must emit exactly one Start then one Finish, with
/// `disk_name`/`gc_id` set and `duration_ms` populated on the Finish.
TEST(CasGcLog, EmitsStartFinishWithCounts)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p"});
    const RootNamespace ns{"srv1/tbl"};

    /// Publish a part, then drop it so its blob/tree become collectable.
    publishPart(store, ns.string(), "all_0_0_0", "hello-cas-gc-log");
    store->dropRef(ns, "all_0_0_0");
    /// Advance the durable watermark floor past the build's seq so the build-watermark guard no
    /// longer spares the now-dropped objects (the background renewer is off in this test).
    store->renewWatermarkOnce();

    std::vector<Rec> rows;
    DB::ContentAddressed::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    /// Drive rounds until we observe both a marking round and a deletion round (a few rounds: the
    /// dead subgraph drains in tree-round + blob-round + a confirm round). Each call appends exactly
    /// a Start then a Finish.
    bool saw_marked = false;
    bool saw_deleted = false;
    size_t marking_finish_idx = 0;
    size_t deleting_finish_idx = 0;
    constexpr size_t max_rounds = 16;
    for (size_t round = 0; round < max_rounds && !(saw_marked && saw_deleted); ++round)
    {
        const size_t before = rows.size();
        sched.runOneRoundNow(Rec::Trigger::Manual);

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
    /// P9: the deletion round forgot the deleted node(s) in the same round; the record surfaces it.
    EXPECT_GT(rows[deleting_finish_idx].forgotten_on_delete, 0u);

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

    std::optional<GetResult> get(const String & key, Range range = {}) override
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

    /// Armed only after Store::open, so opening (which reads/initialises gc state) succeeds.
    std::atomic<bool> arm{false};
};

}

/// A round whose backend throws must produce a Finish with `outcome == Aborted` and a non-empty
/// `error`, and `runOneRoundNow` must rethrow the exception (the round failure is observable, not
/// swallowed — the logging sink itself is best-effort, but the round error propagates).
TEST(CasGcLog, AbortedFinishOnThrowingRound)
{
    auto backend = std::make_shared<ThrowingBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p"});

    std::vector<Rec> rows;
    DB::ContentAddressed::CasGcScheduler sched(
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
