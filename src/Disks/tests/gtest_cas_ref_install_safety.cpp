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

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

namespace
{

PoolPtr openPool(const BackendPtr & backend)
{
    /// A fresh pool with no residue, mirroring `gtest_cas_ref_chunked_flush.cpp`'s `openPool`.
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// As `openPool`, but with a SINGLE-attempt request budget, which is what makes one ambiguous `PUT`
/// conclusive: with retries allowed the controller's resolve-before-reissue would either re-`PUT` (the
/// object never landed) or prove the object durable (it did) and report `Committed`, and neither of the
/// wedge arms under test would ever be reached. Same budget shape as
/// `gtest_cas_ref_chunked_flush.cpp`'s `runChunkFailureCase`, including the short timeouts so there is
/// no inter-attempt sleep to serve.
PoolPtr openPoolSingleAttempt(const BackendPtr & backend)
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    PoolConfig cfg{.pool_prefix = "p", .server_root_id = "test"};
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 100;
    budget.lease_safety_margin_ms = 100;
    cfg.cas_request_budget = budget;
    return Pool::open(backend, cfg);
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

/// Task 4 (spec §A1, site 3). An `Unresolved` PUT must ALWAYS leave the lane wedged with the exact
/// {id, key, bytes} of the in-doubt object -- the wedge is the only record that can ever resolve it, and
/// on this path the object may already be durable. Building the wedge AFTER the PUT copies two `String`s
/// and could therefore fail on allocation, recording NEITHER the transaction nor the wedge: strictly
/// worse than a wedge, because the next append then mints a fresh id and proceeds against a state that
/// is missing a landed transaction. It is now preconstructed before the PUT and installed by a
/// non-throwing move.
///
/// `Mode::Unresolved` deliberately lands NOTHING, so this test also pins the other half of the wedge
/// contract: an ambiguous outcome wedges even when the object turns out never to have existed. The tail
/// counter must NOT advance -- an unproven transaction is not a recorded one.
TEST(CasRefInstallSafety, UnresolvedAlwaysRecordsTheWedge)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/unresolved_wedge"};

    /// Scoped to THIS namespace's ref log, so nothing else the part publish writes (the manifest, the
    /// pool's own metadata) can consume the single fault.
    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_count = 1;

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishEmptyPart(store, ns, "part_a"); });

    EXPECT_TRUE(store->refLaneWedgedForTest(ns)) << "an Unresolved PUT must always leave a wedge";
    const String wedged_key = store->wedgedKeyForTest(ns);
    EXPECT_FALSE(wedged_key.empty()) << "the wedge must retain the in-doubt object's key";
    EXPECT_TRUE(wedged_key.starts_with(store->layout().refsNamespacePrefix(ns) + "_log/"))
        << "the wedged key must be this namespace's ref-log object, not some other key: " << wedged_key;
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), 0u)
        << "an UNPROVEN transaction must not be recorded as applied";
}

/// Task 5 (spec §A1, site 2). Resolving a wedge is a post-durable install too: the resolving GET PROVES
/// the object landed, so the transaction MUST be recorded -- and recording it must be inseparable from
/// clearing the wedge. It was not: the apply and the `materializeCommitted` fold sat between them
/// WITHOUT the ordinary commit arm's swallow, so a fold failure left the transaction applied and the
/// wedge still set, and the next resolution re-applied the same transaction and DOUBLE-bumped the tail
/// counters. The candidate is now built before the GET and installed by a `noexcept` swap that clears
/// the wedge in the same allocation-free region, with the fold outside it and swallowing.
///
/// Drives the real thing end to end (no seam beyond the `LandedThenLost` backend mode): a drop whose
/// object landed but whose acknowledgement -- and whose immediate verification read -- were both lost,
/// then a second append whose flush resolves it. The tail counter is the "exactly once" witness: it is
/// bumped once per install, so a re-applied transaction shows up as one extra.
TEST(CasRefInstallSafety, WedgeResolutionInstallsExactlyOnce)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/wedge_resolution"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");
    /// No snapshot publish can interfere and reset these: the thresholds are 256 logs / 1 MiB and this
    /// whole test is a handful of tiny transactions, so every delta below is exact.
    const size_t tail_after_seed = store->tailSinceSnapshotCountForTest(ns);

    /// Drop "x" through a PUT that LANDS and then loses its response, plus the one-shot lost read that
    /// keeps the controller's own resolve-before-reissue from settling it inside the same attempt. One
    /// attempt, so the lane wedges over an object that is genuinely durable -- the only way in.
    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::LandedThenLost;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });

    ASSERT_TRUE(store->refLaneWedgedForTest(ns)) << "the lost-response drop must wedge the lane";
    ASSERT_FALSE(store->wedgedKeyForTest(ns).empty());
    ASSERT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_after_seed)
        << "the wedged transaction is durable but not yet PROVEN, so it must not be recorded yet";

    /// A second append into the same table: its flush resolves the wedge first (+1 install) and then
    /// commits its own transaction (+1). Nothing else can add a transaction in between.
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    store->dropRef(ns, "y");

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a wedge proven durable must be cleared";
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_after_seed + 2)
        << "the resolved transaction must be recorded EXACTLY once: +1 for it and +1 for the append that "
           "resolved it (a double-apply would show as +3)";
    /// Both drops took effect -- the wedged one via the resolution install, which is what proves that
    /// install happened at all rather than the wedge merely being discarded.
    EXPECT_FALSE(store->resolveRef(ns, "x", /*allow_stale=*/false).has_value())
        << "the wedged drop was proven durable, so its removal must be visible in the cached state";
    EXPECT_FALSE(store->resolveRef(ns, "y", /*allow_stale=*/false).has_value());
}

/// Negative control, part 1 of 2: does `DENY_ALLOCATIONS_IN_SCOPE` actually fire on an allocation in
/// THIS binary? Deliberately split by build type instead of always asserting death, because the two
/// build flavours turn the guard's `LOGICAL_ERROR` into different observable outcomes:
/// `DEBUG_OR_SANITIZER_BUILD` aborts at Exception construction (`Exception.cpp:74-92`), while a build
/// with only `MEMORY_TRACKER_DEBUG_CHECKS` — e.g. a plain Debug build, which does NOT define the
/// former — merely throws. An earlier version of this control asserted death unconditionally and
/// failed in the latter: the guard had fired correctly, the throw was simply caught by the ref lane's
/// own error handling instead of killing the process. Same split shape as
/// `CasGcStateFormat`/`CasGcStateFormatDeathTest`.
#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasRefInstallSafetyDeathTest, DenyGuardStopsAnAllocation)
{
    EXPECT_DEATH(
        {
            DENY_ALLOCATIONS_IN_SCOPE;
            volatile auto * p = new char[64];
            (void)p;
        },
        "");
}
#elif defined(MEMORY_TRACKER_DEBUG_CHECKS)
TEST(CasRefInstallSafety, DenyGuardStopsAnAllocation)
{
    EXPECT_ANY_THROW({
        DENY_ALLOCATIONS_IN_SCOPE;
        volatile auto * p = new char[64];
        (void)p;
    });
}
#endif

/// Negative control, part 2 of 2: the region the guard protects is actually ENTERED, and the guard is
/// armed at that exact point. A probe that only reads flags proves both without allocating, so unlike
/// part 1 this assertion is immune to how a build type renders a `LOGICAL_ERROR`. Together the two
/// parts give what a single death test was meant to give, and a failure now names WHICH half broke:
/// "the guard does not fire" versus "the install region is never reached / not armed".
TEST(CasRefInstallSafety, InstallRegionProbeIsInvokedAndTheGuardIsArmed)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/install_probe_diag"};

    bool probe_ran = false;
    [[maybe_unused]] bool guard_armed_when_probe_ran = false;
    store->setInstallRegionProbeForTest([&]
    {
        probe_ran = true;
#if defined(MEMORY_TRACKER_DEBUG_CHECKS)
        guard_armed_when_probe_ran = memory_tracker_always_throw_logical_error_on_allocation;
#endif
    });

    publishEmptyPart(store, ns, "part_a");
    store->setInstallRegionProbeForTest(nullptr);

    EXPECT_TRUE(probe_ran) << "the install-region probe was never invoked";
#if defined(MEMORY_TRACKER_DEBUG_CHECKS)
    EXPECT_TRUE(guard_armed_when_probe_ran) << "the probe ran but DENY_ALLOCATIONS_IN_SCOPE was not armed";
#endif
}
