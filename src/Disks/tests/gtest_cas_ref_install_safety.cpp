#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/MemoryTracker.h>

#include <atomic>
#include <cstddef>
#include <memory>

/// Task 3 (spec §A1, site 1): the region of `CasRefLedger::commitRefChunk` between "this chunk's
/// ref-log object is durable" and "the runtime records it".
///
/// Before the fix that region ran `applyRefLogTxn(rt->state, chunk_txn)`, which allocates (the COW
/// containers build an overlay) and can therefore throw `MEMORY_LIMIT_EXCEEDED`. A throw there left the
/// transaction durable but invisible to the writer -- and because a later transaction only needs
/// `greatest_applied < its own id` (contiguity is never checked), a snapshot published afterwards is
/// labelled with that LATER id, so recovery skips the stranded transaction permanently while GC, which
/// folds the ref logs themselves, still applies it. That divergence loses data (a stranded removal
/// leaves the writer holding a ref whose blobs GC deleted), which is why the install is now a
/// prepared-candidate swap: allocation-free, hence non-throwing, and enforced as such by
/// `DENY_ALLOCATIONS_IN_SCOPE`.
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

using namespace DB::Cas;

namespace
{

PoolPtr openPool(const BackendPtr & backend)
{
    /// A fresh pool with no residue, mirroring `gtest_cas_ref_chunked_flush.cpp`'s `openPool`.
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// A legal blob-free part: stage an empty manifest, precommit, promote -- enough to drive real
/// ref-log transactions through the append lane.
void publishEmptyPart(const PoolPtr & s, const RootNamespace & ns, const String & ref)
{
    PartWriteInfo info;
    info.intended_namespace = ns;
    info.intended_ref = ns.string() + "/" + ref;
    auto build = s->beginPartWrite(info);
    const ManifestId id = build->stageManifest({});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
}

}

/// The post-durable install seam exists, is reached by an ordinary commit, and every transaction that
/// reaches it is RECORDED: the tail counter advances exactly once per install, and the ref resolves.
/// The equality is the point -- it is the invariant the old code could break, since there the install
/// was an allocating apply that could throw between the durable `PUT` and the counter bump.
TEST(CasRefInstallSafety, PostDurableInstallIsAllocationFree)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/install_safety_seam"};

    /// Fired on the calling thread by the flush leader, which is this thread; `atomic` regardless, so
    /// the assertions below cannot be read as depending on that.
    std::atomic<size_t> installs{0};
    store->setCarveHookForTest([&installs](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase == CasRefLedger::CarvePhaseForTest::PostDurableInstall)
            installs.fetch_add(1);
    });

    publishEmptyPart(store, ns, "part_a");
    store->setCarveHookForTest(nullptr);

    const size_t seen = installs.load();
    EXPECT_GT(seen, 0u) << "the post-durable install seam must be reached by an ordinary commit";
    /// No snapshot publish can interfere: the thresholds are 256 logs / 1 MiB and this part is a
    /// handful of tiny transactions, so the tail counter still holds every one of them.
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), seen)
        << "every durable transaction that entered the install region must be recorded in the tail";
    EXPECT_TRUE(store->resolveRef(ns, "part_a", /*allow_stale=*/false).has_value());
}

/// The install region must contain no allocation. Under a debug build `DENY_ALLOCATIONS_IN_SCOPE` turns
/// any allocation there into a `LOGICAL_ERROR`, which aborts, so this negative control proves the guard
/// is ARMED and the region is actually entered -- otherwise a future edit could add an allocating
/// statement to it and nothing would notice. `threadsafe` death-test style: the parent process has CAS
/// background threads by the time this runs, and forking without exec would inherit their locks.
TEST(CasRefInstallSafetyDeathTest, AllocationInsideTheInstallRegionIsCaught)
{
#if defined(MEMORY_TRACKER_DEBUG_CHECKS)
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
            auto store = openPool(backend);
            const RootNamespace ns{"srv1/install_safety_probe"};
            store->setInstallRegionProbeForTest([] { volatile auto * p = new char[64]; (void)p; });
            publishEmptyPart(store, ns, "part_a");
        },
        "");
#else
    GTEST_SKIP() << "DENY_ALLOCATIONS_IN_SCOPE is a no-op in this build";
#endif
}

/// Isolation probe for the negative control above: does `DENY_ALLOCATIONS_IN_SCOPE` catch an
/// allocation AT ALL inside a gtest binary? Touches no CAS machinery, so a failure here means the
/// guard cannot be exercised from a unit test (and the control above is unimplementable as written),
/// while a pass narrows the problem to the install-region probe path.
TEST(CasRefInstallSafetyDeathTest, DenyGuardCatchesAPlainAllocation)
{
#if defined(MEMORY_TRACKER_DEBUG_CHECKS)
    EXPECT_DEATH(
        {
            DENY_ALLOCATIONS_IN_SCOPE;
            volatile auto * p = new char[64];
            (void)p;
        },
        "");
#else
    GTEST_SKIP() << "DENY_ALLOCATIONS_IN_SCOPE is a no-op in this build";
#endif
}
