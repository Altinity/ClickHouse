#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Common/Exception.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

/// Task 1: ref-lane exception-safety. A queue leader that throws BEFORE carving its compatible batch
/// must not leave its own enqueued item stranded in `rt->pending`. If it does, a later leader (a woken
/// follower) carves the stranded item and runs its `build_ops` closure long after the original caller's
/// stack -- which the production `[&]` closures capture by reference -- has unwound: a use-after-free.
///
/// These tests drive the fault through the SAME pre-carve injection point production leaders pass
/// (`setRefPreCarveHookForTest`, invoked inside `flushRefBatch` immediately before the batch is carved).
/// The suite name is prefixed `RefWriter` so it is covered by the `RefWriter*` unit-test gate filter.

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;

namespace
{

PoolPtr openPoolForRefLane(const BackendPtr & backend)
{
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

}

/// A SOLO faulted caller must not leave its own item behind in the pending queue. Before the fix, the
/// leader's `appendRefOps` catch reset `leader_active` and rethrew but never completed / de-pended the
/// leader's own item, so it was stranded in `rt->pending` with `done == false` forever (nothing left to
/// carve it) -- the deterministic, sanitizer-independent shape of the stranded-item defect.
TEST(RefWriterLaneExceptionSafety, SoloLeaderThrowBeforeCarveDrainsOwnItem)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForRefLane(backend);
    const RootNamespace ns{"srv1/reflane_solo"};

    std::atomic<int> fault_armed{1};
    store->setRefPreCarveHookForTest([&]
    {
        if (fault_armed.exchange(0) == 1)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected pre-carve fault");
    });

    bool threw = false;
    try
    {
        store->appendRefOps(ns, MutationScope::ref("ref_solo"),
            [](const RefTableState &) -> std::vector<RefOp> { return {}; },
            RootMutationOrigin::Writer, RootMutationKind::Publish);
    }
    catch (const DB::Exception &)
    {
        threw = true;
    }
    store->setRefPreCarveHookForTest(nullptr);

    EXPECT_TRUE(threw) << "the faulted solo caller must observe the injected error";
    EXPECT_EQ(store->refQueuePendingForTest(ns), 0u)
        << "the leader's own item was left stranded in rt->pending after it threw before carving";
}

/// Two concurrent callers on one namespace. The first flush's leader throws before carving; a woken
/// follower then leads. Before the fix, the follower carved the faulted leader's STILL-pending item and
/// ran its `build_ops` closure -- the use-after-free window. This asserts, sanitizer-independently, that
/// the follower never invokes the faulted caller's closure, that the queue drains, and that the
/// non-faulted caller still completes.
TEST(RefWriterLaneExceptionSafety, FollowerNeverRunsStrandedLeaderClosure)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForRefLane(backend);
    const RootNamespace ns{"srv1/reflane_follower"};

    std::atomic<int> fault_armed{1};
    store->setRefPreCarveHookForTest([&]
    {
        /// Throw only on the FIRST leader flush, so the follower (or a re-drive) can proceed.
        if (fault_armed.exchange(0) == 1)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected pre-carve fault");
    });

    /// Set by the faulted caller's own closure iff a DIFFERENT thread (a follower leader) ever runs it --
    /// i.e. the stranded item was carved by someone other than its owner. This is the direct, portable
    /// signature of the use-after-free the fix prevents.
    std::atomic<std::thread::id> faulted_owner{};
    std::atomic<bool> faulted_closure_ran_on_follower{false};

    std::atomic<int> ok{0};
    auto caller = [&](int seq, bool is_faulted)
    {
        try
        {
            store->appendRefOps(ns, MutationScope::ref("ref_" + std::to_string(seq)),
                [&, is_faulted](const RefTableState &) -> std::vector<RefOp>
                {
                    if (is_faulted && std::this_thread::get_id() != faulted_owner.load())
                        faulted_closure_ran_on_follower.store(true);
                    return {};
                },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
            ok.fetch_add(1);
        }
        catch (const DB::Exception &)
        {
            /// The faulted caller may see the injected error; that is expected.
        }
    };

    /// Serialize the two callers so the fault deterministically lands on the FIRST one to lead: t1
    /// enqueues and becomes leader before t2 enters. t2 is released only once t1 is already pending.
    std::thread t1([&]
    {
        faulted_owner.store(std::this_thread::get_id());
        caller(1, /*is_faulted=*/true);
    });
    while (store->refQueuePendingForTest(ns) < 1)
        std::this_thread::yield();
    std::thread t2([&] { caller(2, /*is_faulted=*/false); });

    t1.join();
    t2.join();
    store->setRefPreCarveHookForTest(nullptr);

    EXPECT_FALSE(faulted_closure_ran_on_follower.load())
        << "a follower leader carved and ran the stranded faulted caller's build_ops closure (use-after-free)";
    EXPECT_EQ(store->refQueuePendingForTest(ns), 0u) << "an item was stranded in rt->pending";
    EXPECT_GE(ok.load(), 1) << "the non-faulted caller must complete cleanly";
}

/// codex stage-1 review (Important): an allocation exception at the PRE-TENURE point -- the first
/// allocation that builds the leader's responsibility set, BEFORE `leader_active` is published -- must
/// not permanently strand the append-lane baton. Before the fix the throwing allocation fired AFTER
/// `leader_active = true` (and after the queue mutex was released), leaving the baton held with no live
/// leader and the caller's item stuck in `pending`: every later writer on the namespace would wait
/// forever at the leader-election cv, and shutdown draining could only time out. This drives the fault
/// through the dedicated pre-tenure seam and asserts, deterministically (no hang), that the lane is left
/// idle: the item is un-enqueued and the baton is un-taken.
TEST(RefWriterLaneExceptionSafety, PreTenureAllocFailureReleasesBaton)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForRefLane(backend);
    const RootNamespace ns{"srv1/reflane_pretenure"};

    std::atomic<int> fault_armed{1};
    store->setRefPreTenureHookForTest([&]
    {
        if (fault_armed.exchange(0) == 1)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected pre-tenure fault");
    });

    bool threw = false;
    try
    {
        store->appendRefOps(ns, MutationScope::ref("ref_pretenure"),
            [](const RefTableState &) -> std::vector<RefOp> { return {}; },
            RootMutationOrigin::Writer, RootMutationKind::Publish);
    }
    catch (const DB::Exception &)
    {
        threw = true;
    }
    store->setRefPreTenureHookForTest(nullptr);

    EXPECT_TRUE(threw) << "the faulted caller must observe the injected error";
    EXPECT_EQ(store->refQueuePendingForTest(ns), 0u)
        << "a pre-tenure allocation failure left the caller's item stranded in rt->pending";
    EXPECT_FALSE(store->refLeaderActiveForTest(ns))
        << "a pre-tenure allocation failure left the append-lane baton held with no live leader";
}
