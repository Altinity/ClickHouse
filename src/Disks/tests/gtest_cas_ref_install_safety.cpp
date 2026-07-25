#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/MemoryTracker.h>
#include <Common/ProfileEvents.h>

#include <atomic>
#include <cstddef>
#include <exception>
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
extern const int CORRUPTED_DATA;
extern const int LOGICAL_ERROR;
extern const int MEMORY_LIMIT_EXCEEDED;
}

namespace ProfileEvents
{
extern const Event CasRefApplyPoisoned;
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

/// Installs a ONE-SHOT throwing probe into the post-durable install regions (spec §A2): the next region
/// entered throws, every later one runs normally -- which is what lets a terminality test drive a
/// SUCCESSFUL flush after the poison.
///
/// The exception is built HERE, outside the region, and the probe only rethrows it: constructing a
/// `DB::Exception` inside the region would allocate and trip `DENY_ALLOCATIONS_IN_SCOPE`, so the test
/// would be exercising the guard instead of the poison. `MEMORY_LIMIT_EXCEEDED` (what a real tracked
/// allocation failure raises) and deliberately NOT `LOGICAL_ERROR`, which aborts at construction in
/// debug/sanitizer builds.
///
/// With §A1 landed this seam is the ONLY way to reach the `Poisoned` transition at all: all three
/// install regions are allocation-free and therefore cannot throw on their own.
void armOneShotInstallFailure(const PoolPtr & store)
{
    auto planned = std::make_exception_ptr(DB::Exception(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED,
        "simulated allocation failure inside the post-durable install region"));
    auto fired = std::make_shared<std::atomic<bool>>(false);
    store->setInstallRegionProbeForTest([planned, fired]
    {
        if (fired->exchange(true))
            return;
        /// The throw itself allocates its exception object through `malloc`, which the memory tracker
        /// does not see, so it would not trip the guard anyway -- re-allowing allocations for the
        /// duration of the throw makes that a stated property of the test rather than a bet on a libc++
        /// implementation detail.
        ALLOW_ALLOCATIONS_IN_SCOPE;
        std::rethrow_exception(planned);
    });
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

/// ===================================================================================
/// Task 7 (spec §A2): the apply-pending poison state machine.
///
/// §A1 made all three post-durable install regions allocation-free, so "durable but unapplied" should
/// be unreachable. `RefApplyState` is the cheap marker that makes it VISIBLE if it happens anyway, and
/// it is the predicate the relink confirm will read as its rule 4. It is an ASSERT LAYER, not a fence:
/// none of the tests below expect a poisoned table to refuse anything -- one of them positively pins
/// the opposite (a later flush still commits), because that is the honest description of what this
/// task ships and of why §A1 had to land first.
/// ===================================================================================

/// `Clean -> ApplyPending -> Clean` on the ordinary commit path. The mid-flight observation is what
/// makes this more than a tautology: the marker is read from INSIDE the post-durable seam, i.e. after
/// the object is durable and before the runtime records it -- exactly the window the marker names. The
/// seam takes no lock the accessor needs (`ref_queue_mutex` is not held across a flush), so reading it
/// there cannot deadlock.
TEST(CasRefInstallSafety, ApplyStateArmsBeforeTheDurablePutAndClearsOnInstall)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/apply_state_commit"};

    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "a table that has never written anything owes no apply";

    /// Fired on the calling thread by the flush leader, which is this thread; `atomic` regardless, as in
    /// `PostDurableInstallIsAllocationFree` above, so no assertion here reads as depending on that.
    std::atomic<size_t> observations{0};
    std::atomic<size_t> pending_observations{0};
    store->setCarveHookForTest([&](CasRefLedger::CarvePhaseForTest phase)
    {
        if (phase != CasRefLedger::CarvePhaseForTest::PostDurableInstall)
            return;
        observations.fetch_add(1);
        if (store->applyStateForTest(ns) == RefApplyState::ApplyPending)
            pending_observations.fetch_add(1);
    });

    publishEmptyPart(store, ns, "part_a");
    store->setCarveHookForTest(nullptr);

    EXPECT_GT(observations.load(), 0u) << "the post-durable seam must be reached by an ordinary commit";
    EXPECT_EQ(pending_observations.load(), observations.load())
        << "every durable-but-not-yet-installed transaction must be marked ApplyPending";
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "a completed install owes no apply: the marker must be back to Clean";
}

/// `Clean -> ApplyPending`, and the deliberate NON-clear at install region 3. An `Unresolved` outcome is
/// exactly "an object that may be durable and is not applied", so a wedged lane's steady state IS the
/// pending state -- clearing it there would erase the very case the marker exists to name.
TEST(CasRefInstallSafety, UnresolvedWedgeLeavesTheApplyMarkerPending)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_wedge"};

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_count = 1;

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishEmptyPart(store, ns, "part_a"); });

    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::ApplyPending)
        << "a wedged lane may hold a durable transaction the runtime has not recorded";
}

/// `ApplyPending -> Clean` at install region 2: the resolving GET proves the wedged object durable and
/// the same allocation-free region that records the transaction clears the marker.
TEST(CasRefInstallSafety, WedgeResolutionClearsTheApplyMarker)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_unwedge"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    /// The one mode that wedges over a GENUINELY durable object (see `ChunkFaultBackend`): the write
    /// lands, its acknowledgement is lost, and the controller's own verifying read is lost too.
    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::LandedThenLost;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    ASSERT_EQ(store->applyStateForTest(ns), RefApplyState::ApplyPending);

    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    store->dropRef(ns, "y");

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "the wedged transaction was proven durable AND installed, so nothing is owed any more";
}

/// `ApplyPending -> Clean` on the conclusive `putIfAbsentControlled` throw. The ref-log key is
/// write-once, so a DIFFERENT object at it proves OUR bytes never landed: no apply is owed, and the
/// lane stays usable rather than wedged.
TEST(CasRefInstallSafety, ConclusivePutRejectionClearsTheApplyMarker)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_conflict"};

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::ForeignConflict;
    backend->fault_count = 1;

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { publishEmptyPart(store, ns, "part_a"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a proven conflict is conclusive and must not wedge";
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "a conclusively rejected PUT put nothing durable in place, so no apply is owed";
}

/// `ApplyPending -> Clean` on `DefiniteFailure` -- the outcome whose whole meaning is "proven never
/// applied". Needs S3 error classification: that is the only exception family
/// `classifyConditionalWriteResult` will ever call definite (everything else is fail-safe Unresolved).
TEST(CasRefInstallSafety, DefiniteFailureClearsTheApplyMarker)
{
#if !USE_AWS_S3
    GTEST_SKIP() << "DefiniteFailure classification requires S3 error types (USE_AWS_S3 off)";
#else
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_definite"};

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Definite;
    backend->fault_count = 1;

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { publishEmptyPart(store, ns, "part_a"); });

    EXPECT_FALSE(store->refLaneWedgedForTest(ns)) << "a definite failure is a safe gap and must not wedge";
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "a definitively rejected PUT is proven non-durable, so no apply is owed";
#endif
}

/// `ApplyPending -> Clean` on the only wedge resolution that is CONCLUSIVELY NEGATIVE: foreign bytes at
/// the wedged (write-once) key prove our body never landed there. `resolveByExactGet` never reports a
/// plain "absent" verdict -- absent or unreadable is `Unresolved`, since another attempt may still be
/// legal -- so this arm is the whole of "a resolution that proves the key is not ours".
///
/// Release-only, with the death twin below: the anomaly reaction raises `LOGICAL_ERROR`, which aborts
/// the process in debug/sanitizer builds instead of behaving like a catchable exception (same split as
/// `CasAnomalyPolicy.ForeignBytesAtWedgeKeyTripFenceAndRemount`, whose fence/audit assertions this test
/// deliberately does not repeat). The marker is cleared BEFORE that reaction, so the ordering is not
/// what makes this release-only -- the abort is.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CasRefInstallSafety, WedgeResolutionProvenForeignClearsTheApplyMarker)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_foreign_wedge"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_EQ(store->applyStateForTest(ns), RefApplyState::ApplyPending);

    /// Out of band, a foreign writer lands DIFFERENT bytes at the exact wedged key. The fault mode is
    /// off first so this write is not itself intercepted.
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_FALSE(wedged_key.empty());
    ASSERT_EQ(backend->putIfAbsent(wedged_key, "a-different-object").outcome, PutOutcome::Done);

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&] { store->dropRef(ns, "y"); });

    EXPECT_TRUE(store->refLaneWedgedForTest(ns))
        << "foreign interference keeps the wedge for inspection (fail closed)";
    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Clean)
        << "our transaction provably never landed at that write-once key, so no apply is owed -- this is "
           "the one state where a wedged lane is legitimately not ApplyPending";
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CasRefInstallSafetyDeathTest, WedgeResolutionProvenForeignAborts)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_foreign_wedge"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });

    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    const String wedged_key = store->wedgedKeyForTest(ns);
    ASSERT_FALSE(wedged_key.empty());
    ASSERT_EQ(backend->putIfAbsent(wedged_key, "a-different-object").outcome, PutOutcome::Done);

    /// The release build's marker assertion cannot be made here -- there is no post-abort state to
    /// inspect. This twin only pins that the anomaly still aborts, so the `#ifndef` above is not
    /// silently skipping a test that would otherwise pass.
    EXPECT_DEATH({ store->dropRef(ns, "y"); }, "");
}
#endif

/// `ApplyPending -> Poisoned` at install region 1 (`commitRefChunk`'s candidate install). Unreachable
/// in production with §A1 landed -- the region allocates nothing -- so the probe seam is what simulates
/// the OLD post-durable failure. The transaction IS durable at that point, and the runtime does not
/// record it: exactly the state the marker exists to name.
TEST(CasRefInstallSafety, PostDurableInstallFailurePoisonsTheTable)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/apply_state_poison"};

    publishEmptyPart(store, ns, "x");
    ASSERT_EQ(store->applyStateForTest(ns), RefApplyState::Clean);

    const uint64_t poisoned_before = ProfileEvents::global_counters[ProfileEvents::CasRefApplyPoisoned].load();
    armOneShotInstallFailure(store);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { store->dropRef(ns, "x"); });
    store->setInstallRegionProbeForTest(nullptr);

    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Poisoned)
        << "an install that failed AFTER its object was durable must be visible, not silent";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefApplyPoisoned].load() - poisoned_before, 1u)
        << "the transition to Poisoned must be exported exactly once";
}

/// `ApplyPending -> Poisoned` at install region 2 (the wedge-resolution install). Same class of failure
/// one region over: the resolving GET already PROVED the object durable, so an install that does not
/// complete there leaves the same missing transaction -- and the wedge survives, because the swap that
/// would have cleared it is in the same region that threw.
TEST(CasRefInstallSafety, WedgeResolutionInstallFailurePoisonsTheTable)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_poison_unwedge"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::LandedThenLost;
    backend->fault_count = 1;
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));

    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::None;
    armOneShotInstallFailure(store);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { store->dropRef(ns, "y"); });
    store->setInstallRegionProbeForTest(nullptr);

    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Poisoned);
    EXPECT_TRUE(store->refLaneWedgedForTest(ns))
        << "the install and the unwedge are one region: neither happened";
}

/// `ApplyPending -> Poisoned` at install region 3 (the wedge install on an `Unresolved` outcome). The
/// object's durability is unproven here, but losing the wedge is strictly WORSE than being wedged --
/// nothing records that a possibly-durable transaction exists, and the next append mints a fresh id
/// against a state that may be missing it -- so this region poisons too.
TEST(CasRefInstallSafety, WedgeInstallFailurePoisonsTheTable)
{
    auto backend = std::make_shared<DB::Cas::tests::ChunkFaultBackend>();
    auto store = openPoolSingleAttempt(backend);
    const RootNamespace ns{"srv1/apply_state_poison_wedge"};

    publishEmptyPart(store, ns, "x");

    backend->fault_substr = store->layout().refsNamespacePrefix(ns) + "_log/";
    backend->mode = DB::Cas::tests::ChunkFaultBackend::Mode::Unresolved;
    backend->fault_count = 1;
    armOneShotInstallFailure(store);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { store->dropRef(ns, "x"); });
    store->setInstallRegionProbeForTest(nullptr);

    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Poisoned);
    EXPECT_FALSE(store->refLaneWedgedForTest(ns))
        << "the wedge install threw, so the record that would have resolved the in-doubt object is gone "
           "-- which is precisely why this region poisons despite durability being unproven";
}

/// The negative transition: `Poisoned` is TERMINAL for the runtime. A later flush that commits and
/// installs perfectly must NOT clear it -- the earlier durable transaction is still missing from this
/// cached state, and a marker that a successful flush could wash away would report health that does not
/// exist. Enforced by construction: both clearing transitions are CASes whose expected value is
/// `Clean`/`ApplyPending`, so no code path can store over a poison; only a fresh recovery, which means a
/// REPLACED runtime, starts over at `Clean`.
///
/// It also pins the honest scope of §A2: a poisoned table keeps ACCEPTING work. That is intentional --
/// this is an assert layer, not a fence (see `RefApplyState`), and safety comes from §A1 having made
/// the poison unreachable in the first place.
TEST(CasRefInstallSafety, PoisonedSurvivesALaterSuccessfulFlush)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/apply_state_poison_terminal"};

    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    const uint64_t poisoned_before = ProfileEvents::global_counters[ProfileEvents::CasRefApplyPoisoned].load();
    armOneShotInstallFailure(store);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED, [&] { store->dropRef(ns, "x"); });
    store->setInstallRegionProbeForTest(nullptr);
    ASSERT_EQ(store->applyStateForTest(ns), RefApplyState::Poisoned);

    /// A perfectly ordinary, fully successful append afterwards.
    store->dropRef(ns, "y");
    EXPECT_FALSE(store->resolveRef(ns, "y", /*allow_stale=*/false).has_value())
        << "the later flush must really have committed AND installed -- otherwise the assertion below "
           "would pass for the wrong reason";

    EXPECT_EQ(store->applyStateForTest(ns), RefApplyState::Poisoned)
        << "a later successful flush must never clear a poison: the transaction stranded by the failed "
           "install is still missing from this runtime's state";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasRefApplyPoisoned].load() - poisoned_before, 1u)
        << "the event counts TRANSITIONS, so the successful flush must not have added another";
}
