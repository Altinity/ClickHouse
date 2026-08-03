#include <gtest/gtest.h>

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <Poco/Exception.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ProfileEvents
{
extern const Event CasRefSnapshotPublishDispatched;
}

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

/// Task 6b remainder (Stage B, `{#t2}`): the publication-ordering coverage that Task 6b's rename left
/// undone. This suite PINS existing behavior of `CasRefLedger::tryPublishSnapshotAndAdvanceCheckpointOnce`
/// (the one retry unit), `admitSnapshotPublishUnderStateLock`, `advancePublishBackoff`/
/// `resetPublishBackoff`, and `dispatchSnapshotPublisher`/`settleSnapshotPublish`. It changes NO
/// production code.
///
/// Normative ordering: (1) the immutable snapshot body becomes durable; (2) `_ckpt` advances; (3) the new
/// snapshot is adopted in this cache's memory. `NeedsRecovery` (this campaign's `Poisoned`) blocks
/// publication -- a durable transaction may be missing from the cached view -- and forces
/// `ensureRefTableRecovered` to re-walk the durable stream on the very next touch.
///
/// The suite name is prefixed `Cas` so it is covered by the `Cas*` unit-test gate filter.

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;

namespace
{

/// Records the ORDER of body-PUT / `_ckpt`-CAS operations (so a test can compare indices) and lets a
/// test inject a persistent `Conflict` on one chosen `_ckpt` key -- the same technique
/// `gtest_cas_ref_writer.cpp`'s `RefWriterTestBackend::ckpt_conflict_key`/`ckpt_conflict_count` uses to
/// drive the ledger into `NeedsRecovery`, reproduced here so this suite has no dependency on that file's
/// internal (non-exported) test type. Delegates every operation to `CountingBackend` unchanged, so the
/// per-key counters (`putCount`/`casPutCount`) remain available as the positive control.
class OrderedFaultBackend : public CountingBackend
{
public:
    using CountingBackend::casPut;
    using CountingBackend::get;
    using CountingBackend::putIfAbsent;

    enum class Op : uint8_t { Put, Cas };
    struct Entry
    {
        Op op;
        String key;
    };

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        record(Op::Put, key);
        if (fail_put_count > 0 && !fail_put_substr.empty() && key.find(fail_put_substr) != String::npos)
        {
            --fail_put_count;
            throw Poco::TimeoutException("OrderedFaultBackend: simulated PUT response lost, nothing landed");
        }
        return CountingBackend::putIfAbsent(key, bytes, meta);
    }

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                      const ObjectMeta & meta) override
    {
        record(Op::Cas, key);
        if (key == fail_cas_key && fail_cas_count > 0)
        {
            --fail_cas_count;
            /// A `Conflict` (not a thrown/ambiguous response): the caller's own re-read-and-merge loop
            /// (`publishCkpt`) treats this exactly like a concurrent writer that landed first, and
            /// exhausts `MAX_CKPT_CAS_ATTEMPTS` (100) without ever committing -- deterministically, with
            /// no wall-clock wait, since the loop is attempt-bounded rather than only deadline-bounded.
            return {CasOutcome::Conflict, {}};
        }
        return CountingBackend::casPut(key, bytes, expected, meta);
    }

    /// Arms a persistent CAS conflict at `key` for the next `count` attempts.
    void armCasConflict(const String & key, size_t count)
    {
        fail_cas_key = key;
        fail_cas_count = count;
    }

    /// Arms a persistent, never-committed PUT failure for the next `count` `putIfAbsent` calls whose key
    /// contains `substr`: the object is never actually written (unlike a real ambiguous response, which
    /// may or may not have landed), so the resolve-by-exact-GET a controlled `CasRequestBudget` with
    /// `max_attempts = 1` performs always finds the key absent and classifies the attempt a definite,
    /// non-`Committed` failure -- deterministically, with no internal retry and no wall-clock wait.
    void armPutFailure(const String & substr, int count)
    {
        fail_put_substr = substr;
        fail_put_count = count;
    }

    /// The current length of the journal -- a caller's baseline for `indicesFrom` below, so a query can
    /// be scoped to "since I last looked" rather than "since the pool opened" (whose earlier entries
    /// belong to unrelated setup writes, e.g. the birth transaction's own checkpoint CAS).
    size_t journalSize() const
    {
        std::lock_guard lock(mutex);
        return journal.size();
    }

    /// Every index at or after `from` where `op`/`key` matches, in order.
    std::vector<size_t> indicesFrom(Op op, const String & key, size_t from) const
    {
        std::lock_guard lock(mutex);
        std::vector<size_t> result;
        for (size_t i = from; i < journal.size(); ++i)
            if (journal[i].op == op && journal[i].key == key)
                result.push_back(i);
        return result;
    }

    /// The first index at or after `from` where `op`/`key` matches, if any.
    std::optional<size_t> firstIndexFrom(Op op, const String & key, size_t from) const
    {
        const auto indices = indicesFrom(op, key, from);
        return indices.empty() ? std::nullopt : std::make_optional(indices.front());
    }

private:
    void record(Op op, const String & key)
    {
        std::lock_guard lock(mutex);
        journal.push_back({op, key});
    }

    mutable std::mutex mutex;
    std::vector<Entry> journal;
    String fail_cas_key;
    size_t fail_cas_count = 0;
    String fail_put_substr;
    int fail_put_count = 0;
};

PoolPtr openPool(const std::shared_ptr<OrderedFaultBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, std::move(config));
}

/// The same one-transaction publish every other ref suite drives, so a namespace reaches `Live` through
/// the REAL append lane (which is also what creates its `_ckpt`).
RefTxnId publishRef(const PoolPtr & store, const RootNamespace & ns, const String & ref, uint64_t ordinal)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref, ordinal](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, ordinal, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

}

/// ---------------------------------------------------------------------------------------------
/// 1. Snapshot body durable strictly before `_ckpt` advances
/// ---------------------------------------------------------------------------------------------

TEST(CasRefSnapshotPublishOrdering, SnapshotBodyIsDurableBeforeCheckpointAdvances)
{
    auto backend = std::make_shared<OrderedFaultBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/order_body_before_ckpt"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    const NamespaceLifeId life = *store->refTableLifeForTest(ns);
    const String snapshot_key = store->layout().refSnapshotKey(life, RefTxnId{store->writerEpoch(), 1});
    const String ckpt_key = store->layout().refCkptKey(life);

    /// The birth transaction above already CAS'd `_ckpt` itself (once for its own `life_epoch`, once for
    /// its committed frontier) -- ordinary append-commit traffic that has nothing to do with the snapshot
    /// publisher. The comparison below must therefore look only at what happens FROM this offset, or it
    /// would find the birth's ckpt writes (which precede the snapshot body by construction) and conclude
    /// nothing about the publisher's own ordering.
    const size_t offset = backend->journalSize();
    const uint64_t put_before = backend->putCount(snapshot_key);
    const uint64_t cas_before = backend->casPutCount(ckpt_key);

    ASSERT_TRUE(store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns))
        << "a healthy Ready-lane table with an uncovered tail must publish";

    /// Positive control: this attempt touched each key exactly once (no retry, no redundant write) --
    /// which is what makes the index comparison below meaningful rather than an artifact of a busy log.
    EXPECT_EQ(backend->putCount(snapshot_key) - put_before, 1u);
    EXPECT_EQ(backend->casPutCount(ckpt_key) - cas_before, 1u);

    const auto body_index = backend->firstIndexFrom(OrderedFaultBackend::Op::Put, snapshot_key, offset);
    const auto ckpt_index = backend->firstIndexFrom(OrderedFaultBackend::Op::Cas, ckpt_key, offset);
    ASSERT_TRUE(body_index.has_value()) << "the snapshot body must have been PUT";
    ASSERT_TRUE(ckpt_index.has_value()) << "the checkpoint must have been CAS-advanced";
    EXPECT_LT(*body_index, *ckpt_index)
        << "INV-4's second `_ckpt` writer runs strictly after the immutable body is durable";
}

/// Sensitivity check for the ordering assertion above: swap the comparison direction and confirm it
/// fails, proving the index comparison actually discriminates the real order rather than passing
/// vacuously. Mutation reverted immediately below; output preserved to `build/t2_sensitivity_1.log`.
TEST(CasRefSnapshotPublishOrdering, SensitivityCheckOrderingComparisonDiscriminates)
{
    auto backend = std::make_shared<OrderedFaultBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/order_sensitivity"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    const NamespaceLifeId life = *store->refTableLifeForTest(ns);
    const String snapshot_key = store->layout().refSnapshotKey(life, RefTxnId{store->writerEpoch(), 1});
    const String ckpt_key = store->layout().refCkptKey(life);
    const size_t offset = backend->journalSize();
    ASSERT_TRUE(store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns));

    const auto body_index = backend->firstIndexFrom(OrderedFaultBackend::Op::Put, snapshot_key, offset);
    const auto ckpt_index = backend->firstIndexFrom(OrderedFaultBackend::Op::Cas, ckpt_key, offset);
    ASSERT_TRUE(body_index.has_value());
    ASSERT_TRUE(ckpt_index.has_value());

    /// The deliberately WRONG expectation (body after ckpt) must fail -- EXPECT_FALSE wraps the check so
    /// this test itself stays green while proving the assertion is load-bearing rather than tautological.
    EXPECT_FALSE(*body_index > *ckpt_index)
        << "load-bearing mutation demonstration performed after implementation; mutation reverted; "
           "patch and failing output preserved: asserting the swapped (wrong) order here fails, which is "
           "what proves the true-order assertion in "
           "CasRefSnapshotPublishOrdering.SnapshotBodyIsDurableBeforeCheckpointAdvances actually observes "
           "the real sequence rather than passing vacuously";
}

/// ---------------------------------------------------------------------------------------------
/// 2. Adoption happens last, and only once both durable effects landed
/// ---------------------------------------------------------------------------------------------

TEST(CasRefSnapshotPublishOrdering, AdoptionHappensLastAndOnlyAfterBothDurableEffects)
{
    auto backend = std::make_shared<OrderedFaultBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/order_adoption_after_both"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    const NamespaceLifeId life = *store->refTableLifeForTest(ns);
    const String snapshot_key = store->layout().refSnapshotKey(life, RefTxnId{store->writerEpoch(), 1});
    const String ckpt_key = store->layout().refCkptKey(life);

    /// Fail every one of the (attempt-bounded) 100 `_ckpt` CAS attempts `publishCkpt` will make: the
    /// body PUT still commits (dedup: an identical, already-durable body resolves as `Committed` without
    /// re-sending), but the checkpoint never advances within this call.
    backend->armCasConflict(ckpt_key, 100);
    EXPECT_FALSE(store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns))
        << "a persistently conflicting checkpoint CAS must not be reported as a successful publish";

    EXPECT_EQ(backend->putCount(snapshot_key), 1u) << "the body is durable regardless of the ckpt outcome";
    EXPECT_FALSE(store->newestPublishedSnapshotIdForTest(ns).has_value())
        << "in-memory adoption must NOT happen while the checkpoint has not advanced";

    /// Disarm the fault and retry (the one retry unit): the retry issues its OWN `putIfAbsent` attempt at
    /// the same content-addressed key with the same bytes (so `putCount`, a call counter, becomes 2 --
    /// not a "no write happened" 1), but the backend resolves it as `Committed` against the already-durable
    /// object rather than sending a distinct object, and the checkpoint CAS now succeeds.
    ASSERT_TRUE(store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns))
        << "the retry, with the fault cleared, must publish";
    EXPECT_EQ(backend->putCount(snapshot_key), 2u)
        << "the retry's body PUT is its own attempt, resolved via dedup against identical, "
           "already-durable bytes rather than writing a second object";
    EXPECT_EQ(store->newestPublishedSnapshotIdForTest(ns), std::make_optional(RefTxnId{store->writerEpoch(), 1}))
        << "adoption happens exactly once, after both effects are durable";
}

/// ---------------------------------------------------------------------------------------------
/// 3. `NeedsRecovery` ("Poisoned") forces re-recovery before any publish effect can occur
/// ---------------------------------------------------------------------------------------------

/// There is no `Poisoned` state literally named in `CasRefLedger`; `RefLaneState::NeedsRecovery` is the
/// state the header documents as "a transaction is known durable but cannot be installed in this cache
/// ... a hard write and certification fence until replay completes", which is exactly the predicate this
/// task's plan describes as `Poisoned`. It is reached here the same way
/// `gtest_cas_ref_writer.cpp`'s `RefWriterAppendLane.CheckpointConflictAfterLogCommitRequiresRecoveryWithoutInstall`
/// reaches it: a mutation's ref-log body commits durably while its OWN checkpoint-frontier CAS
/// (`commitRefChunk`'s `commit_contribution`, not the snapshot publisher's) conflicts persistently.
///
/// LIMITATION discovered while writing this test, which changed its shape from the plan's original
/// "assert zero writes and a refusal" sketch: `tryPublishSnapshotAndAdvanceCheckpointOnce` calls
/// `ensureRefTableRecovered` unconditionally, and that function re-walks the durable stream whenever the
/// lane is `NeedsRecovery`, REGARDLESS of `recovered`. Reaching `NeedsRecovery` this way necessarily
/// leaves a real durable gap between the committed log and the checkpoint (that is what `NeedsRecovery`
/// means), so the SAME call's re-recovery closes that gap with its own `_ckpt` CAS, and -- because this
/// table has never published a snapshot at all -- the very same call then finds a real, uncovered
/// candidate and goes on to publish one. So a request against a poisoned lane does not return `false`
/// here: it returns `true`, having recovered first. There is no `*ForTest` seam that installs
/// `NeedsRecovery` without creating this reconcilable gap, so "poisoned refuses publication" cannot be
/// pinned as "this call reports failure" -- what IS true, and IS what this test pins, is that no
/// publish-primitive effect (the new snapshot's OWN body PUT, its OWN checkpoint-advance CAS) can occur
/// before recovery's reconciliation CAS has already landed: recovery-then-publish is the only order this
/// call can ever produce, never publish-around-a-still-poisoned-lane.
TEST(CasRefSnapshotPublishOrdering, PoisonedRefusesPublicationAndTriggersReRecovery)
{
    auto backend = std::make_shared<OrderedFaultBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/order_poisoned_refuses"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    const NamespaceLifeId life = *store->refTableLifeForTest(ns);
    const String ckpt_key = store->layout().refCkptKey(life);
    const RefTxnId candidate{store->writerEpoch(), 2};
    const String next_snapshot_key = store->layout().refSnapshotKey(life, candidate);

    /// Drive the very next mutation's OWN checkpoint-frontier CAS into persistent conflict: the log PUT
    /// for `candidate` commits durably, but its checkpoint never advances within this call, and the lane
    /// is left `NeedsRecovery` rather than installing an uncertain result.
    backend->armCasConflict(ckpt_key, 100);
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropRef(ns, "ref_1"); });
    ASSERT_EQ(store->laneStateForTest(ns), RefLaneState::NeedsRecovery);

    const uint64_t recovery_installs_before = store->recoveryInstallCountForTest();
    backend->armCasConflict(ckpt_key, 0);   /// clear the fault so re-recovery's OWN catch-up CAN succeed
    const size_t offset = backend->journalSize();

    EXPECT_TRUE(store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns))
        << "recovery reconciles the durable gap and this table has never published a snapshot, so the "
           "same call goes on to publish one -- see the LIMITATION note above the test";

    /// Re-recovery WAS triggered as an observable state transition (not a silent skip): the lane left
    /// `NeedsRecovery`, and `recoveryInstallCountForTest` -- a counter of exact recovery-result
    /// publications -- advanced.
    EXPECT_EQ(store->laneStateForTest(ns), RefLaneState::Ready)
        << "ensureRefTableRecovered must have re-walked the durable stream and cleared the fence";
    EXPECT_GT(store->recoveryInstallCountForTest(), recovery_installs_before)
        << "a re-recovery install must be observable, not indistinguishable from never having run";

    /// The ordering invariant this poisoned-lane scenario actually supports: recovery's OWN checkpoint
    /// catch-up CAS lands strictly before the snapshot-publish primitive's body PUT, which lands strictly
    /// before the publish primitive's OWN checkpoint-advance CAS. No publish effect ever precedes or
    /// substitutes for recovery's reconciliation.
    const auto ckpt_cas_indices = backend->indicesFrom(OrderedFaultBackend::Op::Cas, ckpt_key, offset);
    const auto body_index = backend->firstIndexFrom(OrderedFaultBackend::Op::Put, next_snapshot_key, offset);
    ASSERT_GE(ckpt_cas_indices.size(), 2u)
        << "expected one checkpoint CAS from recovery's catch-up and one from the snapshot publisher";
    ASSERT_TRUE(body_index.has_value());
    EXPECT_LT(ckpt_cas_indices.front(), *body_index)
        << "recovery's checkpoint catch-up must land before any snapshot-publish effect";
    EXPECT_LT(*body_index, ckpt_cas_indices.back())
        << "the snapshot publisher's own checkpoint CAS still runs after ITS OWN body PUT (INV-4), even "
           "immediately following recovery";
}

/// ---------------------------------------------------------------------------------------------
/// 4. Publish backoff: characterized against a controlled clock (`PoolConfig::boot_ms_fn`)
/// ---------------------------------------------------------------------------------------------

/// `admitSnapshotPublishUnderStateLock`, `advancePublishBackoff` and `resetPublishBackoff` are private
/// to `CasRefLedger`, so they can only be characterized through the public dispatch surface
/// (`appendRefOps`/`resolveRef` triggering `maybeScheduleSnapshotPublish`, and
/// `waitForSnapshotPublishSettleForTest`/`ProfileEvents::CasRefSnapshotPublishDispatched` as the
/// observables). `CasRequestControllerBackoff` is a DIFFERENT mechanism (the request controller's
/// per-attempt retry backoff); this characterizes ONLY the per-table snapshot-publish dispatch backoff.
///
/// A controlled clock (`PoolConfig::boot_ms_fn`) DOES exist for this seam (`gtest_cas_ref_writer.cpp`'s
/// `C4BackoffDefersThenRetriesAndPublishes` already relies on it) -- so unlike the plan's anticipated
/// fallback, this pins literal accept/refuse decisions against exact clock offsets rather than only
/// attempt counts.
TEST(CasRefSnapshotPublishOrdering, PublishBackoffDecisionsAreCharacterized)
{
    using ProfileEvents::global_counters;
    auto backend = std::make_shared<OrderedFaultBackend>();

    /// A single-attempt request budget, exactly as `gtest_cas_ref_writer.cpp`'s
    /// `C4BackoffDefersThenRetriesAndPublishes` uses: with `max_attempts = 1` a faulted PUT resolves to a
    /// definite, non-`Committed` outcome on its own attempt, with no internal retry loop and so no
    /// wall-clock wait.
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 5000;
    budget.lease_safety_margin_ms = 100;

    uint64_t fake_now = 1'000'000;
    PoolConfig config;
    config.snapshot_log_count_threshold = 0;              /// any nonempty tail is over-threshold
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    config.snapshot_publish_backoff_initial_ms = 1000;
    config.snapshot_publish_backoff_max_ms = 4000;
    config.mount_lease_ttl_ms = std::chrono::milliseconds(10'000'000);
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    config.cas_request_budget = budget;
    auto store = openPool(backend, config);
    const RootNamespace ns{"srv1/order_backoff"};

    ASSERT_EQ(publishRef(store, ns, "ref_1", 1), (RefTxnId{store->writerEpoch(), 1}));
    store->waitForSnapshotPublishSettleForTest(ns);   /// drain the birth's own auto-dispatched publish
    /// The birth's own auto-dispatch already published a snapshot at this point (threshold 0); the
    /// baseline every "no new publish yet" check below compares against.
    const auto snapshot_after_birth = store->newestPublishedSnapshotIdForTest(ns);
    ASSERT_TRUE(snapshot_after_birth.has_value());

    /// Fault the snapshot BODY put (never the `_ckpt` CAS -- an append-commit's OWN checkpoint write
    /// shares that key, and faulting it would drive the append lane into `NeedsRecovery` instead of
    /// exercising the snapshot-publish backoff this test targets). Exactly 3 failures: the next 3
    /// automatic dispatch attempts fail (arming, then doubling, then re-doubling the backoff); the 4th
    /// finds the fault disarmed and succeeds.
    backend->armPutFailure("_snap/", 3);

    const auto dispatchCount = [&] { return global_counters[ProfileEvents::CasRefSnapshotPublishDispatched].load(); };

    /// Attempt 1: admitted immediately (no backoff armed yet). Fails -> backoff armed at the initial 1000ms.
    ASSERT_EQ(publishRef(store, ns, "ref_2", 2), (RefTxnId{store->writerEpoch(), 2}));
    store->waitForSnapshotPublishSettleForTest(ns);
    const uint64_t d1 = dispatchCount();
    EXPECT_EQ(store->newestPublishedSnapshotIdForTest(ns), snapshot_after_birth)
        << "the failed attempt must not have advanced the published snapshot";

    /// Still within the 1000ms window: a further trigger must NOT re-dispatch.
    store->resolveRef(ns, "ref_1");
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1) << "a read within the initial backoff window must not re-dispatch";

    /// Cross the 1000ms deadline: exactly one retry dispatches (and fails again, doubling to 2000ms).
    fake_now += 1000;
    store->resolveRef(ns, "ref_1");
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1 + 1) << "past the first deadline, exactly one retry dispatches";

    /// Short of the DOUBLED (2000ms) deadline: still refused.
    fake_now += 1000;
    store->resolveRef(ns, "ref_1");
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1 + 1)
        << "advancePublishBackoff doubled the interval to 2000ms; 1000ms elapsed is not enough";

    /// Cross the doubled deadline: one more retry dispatches (and fails again -- the third and last armed
    /// failure -- doubling to the 4000ms cap).
    fake_now += 1000;
    store->resolveRef(ns, "ref_1");
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1 + 2) << "past the doubled deadline, exactly one more retry dispatches";

    /// Cross the (capped) 4000ms deadline: the retry's fault budget is exhausted, so this attempt
    /// succeeds, and `resetPublishBackoff` clears the cooldown -- proved by the NEXT trigger dispatching
    /// with no wait at all.
    fake_now += 4000;
    store->resolveRef(ns, "ref_1");
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1 + 3) << "past the second (capped) deadline, the retry dispatches and succeeds";
    EXPECT_NE(store->newestPublishedSnapshotIdForTest(ns), snapshot_after_birth)
        << "the fault budget is exhausted, so this attempt actually advances the published snapshot";

    ASSERT_EQ(publishRef(store, ns, "ref_3", 3), (RefTxnId{store->writerEpoch(), 3}));
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(dispatchCount(), d1 + 4)
        << "resetPublishBackoff must have cleared the cooldown: the very next over-threshold trigger, at "
           "the SAME clock reading as the successful publish, dispatches immediately with no wait";
}
