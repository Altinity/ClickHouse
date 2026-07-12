#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <Poco/Exception.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

/// Task 10: the writer's ref persistence on the snapshot+log protocol (spec
/// docs/superpowers/specs/2026-07-11-cas-ref-table-snapshot-log-design.md). Covers the plan's Task 10
/// failing-test list: empty+birth recovery; snapshot+tail recovery; recovery restart on a vanished
/// object (converging on a newer snapshot); the append lane's wedge semantics (blocks the same table,
/// leaves other tables free, applies a later-observed-durable append before unwedging); invalid batch
/// entries failing in isolation; and the S3 request-cost contract (one create for a warm isolated
/// mutation, one create shared by a compatible batch).

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int FILE_DOESNT_EXIST;
extern const int CORRUPTED_DATA;
}

namespace ProfileEvents
{
extern const Event CasRefSweepDeferred;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::committedRow;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::minimalLiveSnapshot;
using DB::Cas::tests::namespaceBirthOp;
using DB::Cas::tests::publishCommittedOps;
using DB::Cas::tests::writeRefLogTxnRaw;
using DB::Cas::tests::writeRefSnapshotRaw;

namespace
{

StorePtr openStore(const BackendPtr & backend, CasRequestBudget budget = {})
{
    return Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .cas_request_budget = budget});
}

/// Task 11: like `openStore`, but the caller supplies (and owns) the rest of the config -- snapshot
/// thresholds, grace age, a fake `boot_ms_fn`, etc. `pool_prefix`/`server_root_id` are pinned so every
/// test in this file addresses the same pool shape.
StorePtr openStoreWithConfig(const BackendPtr & backend, PoolConfig config)
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Store::open(backend, std::move(config));
}

/// Mirrors gtest_cas_build.cpp's startBuildFor/publishOneBlobPart, minus the blob (an empty-entry
/// manifest is a legal, blob-free part -- the ref-writer tests only care about ref/manifest identity).
BuildPtr startBuildFor(const StorePtr & s, const RootNamespace & ns, const String & ref)
{
    BuildInfo info;
    info.intended_namespace = ns;
    info.intended_ref = ns.string() + "/" + ref;
    return s->startBuild(info);
}

ManifestId publishEmptyPart(const StorePtr & s, const RootNamespace & ns, const String & ref)
{
    auto build = startBuildFor(s, ns, ref);
    const ManifestId id = build->stageManifest({});
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

ManifestRef manifestRef(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

/// Task 11: an INDEPENDENT ground truth for "cache-replay equivalence" tests -- lists every `_log/`
/// key under `ns` directly off the backend (ignoring any snapshot), decodes and replays them in id
/// order via the SAME shared state machine the writer uses, and returns the resulting state. A
/// published snapshot's bytes must equal `encodeRefTableSnapshot(snapshotOf(replay-through-X, ns))`
/// for this oracle's replay truncated at `X`.
RefTableState independentFullReplayForTest(Backend & backend, const Layout & layout, const RootNamespace & ns,
                                            std::optional<RefTxnId> up_to = std::nullopt)
{
    std::vector<RefTxnId> ids;
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(layout.refsNamespacePrefix(ns), cursor, 1000);
        for (const ListedKey & lk : page.keys)
        {
            const auto parsed = layout.parseRefObjectKey(lk.key);
            if (parsed && parsed->ns == ns && parsed->kind == RefObjectKind::Log
                && (!up_to || !(*up_to < parsed->txn_id)))
                ids.push_back(parsed->txn_id);
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    std::sort(ids.begin(), ids.end());

    RefTableState state;
    for (const RefTxnId & id : ids)
    {
        const auto got = backend.get(layout.refLogKey(ns, id));
        applyRefLogTxn(state, decodeRefLogTxn(got->bytes, ns.string(), id));
    }
    return state;
}

/// The greatest `_snap/<id>.proto` key currently present for `ns`, found via a fresh LIST (independent
/// of the Store's own cached bookkeeping).
std::optional<RefTxnId> listGreatestSnapshotIdForTest(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    std::optional<RefTxnId> greatest;
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(layout.refsNamespacePrefix(ns), cursor, 1000);
        for (const ListedKey & lk : page.keys)
        {
            const auto parsed = layout.parseRefObjectKey(lk.key);
            if (parsed && parsed->ns == ns && parsed->kind == RefObjectKind::Snap
                && (!greatest || *greatest < parsed->txn_id))
                greatest = parsed->txn_id;
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return greatest;
}

/// A backend that can (a) force one `get()` on a chosen exact key to return absent exactly once
/// (simulating an object vanishing between a recovery LIST and its GET, with an optional side effect
/// fired at that exact moment -- e.g. publishing a covering newer snapshot, mirroring a concurrent GC
/// cleanup+republish race), and (b) force `putIfAbsent` on keys matching a chosen substring to throw an
/// ambiguous (Unresolved-classified) exception a bounded number of times, optionally still capturing
/// the (key, bytes) so a test can later "deliver" it -- simulating a request whose RESPONSE was lost
/// even though the write eventually landed server-side.
class RefWriterTestBackend : public CountingBackend
{
public:
    std::set<String> vanish_once_keys;
    std::function<void()> on_vanish_fire;

    String fault_key_substr;
    int fault_count = 0;
    std::optional<std::pair<String, String>> pending_delayed_write;

    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        const auto it = vanish_once_keys.find(key);
        if (it != vanish_once_keys.end())
        {
            vanish_once_keys.erase(it);
            if (on_vanish_fire)
            {
                auto fire = std::move(on_vanish_fire);
                on_vanish_fire = nullptr;
                fire();
            }
            return std::nullopt;
        }
        return CountingBackend::get(key, range);
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override
    {
        if (fault_count > 0 && !fault_key_substr.empty() && key.find(fault_key_substr) != String::npos)
        {
            --fault_count;
            pending_delayed_write = {key, bytes};
            throw Poco::TimeoutException("RefWriterTestBackend: simulated ambiguous result (response lost)");
        }
        {
            std::unique_lock lk(block_mutex);
            if (block_armed && key.find(block_substr) != String::npos)
            {
                block_entered = true;
                block_cv.notify_all();
                block_cv.wait(lk, [&] { return !block_armed; });
            }
        }
        const PutResult r = CountingBackend::putIfAbsent(key, bytes, meta);
        {
            std::lock_guard g(block_mutex);
            block_call_completed = true;
        }
        block_cv.notify_all();
        return r;
    }

    /// "Deliver" the earlier ambiguous write: the request DID eventually land server-side, the caller
    /// just never saw the ack. No-op if no fault has fired since the last delivery.
    void materializePendingDelayedWrite()
    {
        if (pending_delayed_write)
        {
            CountingBackend::putIfAbsent(pending_delayed_write->first, pending_delayed_write->second);
            pending_delayed_write.reset();
        }
    }

    /// Task 11: blocks EVERY `putIfAbsent()` whose key contains `armed_block_substr` until
    /// `releaseBlock()` is called, notifying `awaitBlockEntered()` the first time one is reached. Used
    /// to prove snapshot publication never holds up an unrelated concurrent append.
    void armPutBlock(const String & substr)
    {
        std::lock_guard g(block_mutex);
        block_substr = substr;
        block_armed = true;
        block_entered = false;
        block_call_completed = false;
    }
    void awaitBlockEntered()
    {
        std::unique_lock lk(block_mutex);
        block_cv.wait(lk, [&] { return block_entered; });
    }
    void releaseBlock()
    {
        {
            std::lock_guard g(block_mutex);
            block_armed = false;
        }
        block_cv.notify_all();
    }
    /// Blocks until the PREVIOUSLY-blocked `putIfAbsent` call has actually RETURNED (not merely been
    /// unblocked) -- i.e. its underlying `CountingBackend::putIfAbsent` has completed. Deterministic,
    /// sleep-free way to observe a detached background caller's own work finishing when the TEST no
    /// longer holds anything (e.g. a Store handle) that call would otherwise let it wait on.
    void awaitBlockedCallCompleted()
    {
        std::unique_lock lk(block_mutex);
        block_cv.wait(lk, [&] { return block_call_completed; });
    }

private:
    std::mutex block_mutex;
    std::condition_variable block_cv;
    String block_substr;
    bool block_armed = false;
    bool block_entered = false;
    bool block_call_completed = false;
};

}

/// ===================================================================================
/// Recovery (spec §Startup And Recovery / §Why One LIST Is Sufficient)
/// ===================================================================================

TEST(RefWriterRecovery, EmptyNamespaceRecoversToEmptyState)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/never_touched"};

    EXPECT_TRUE(store->listRefs(ns).empty());
    EXPECT_FALSE(store->resolveRef(ns, "anything").has_value());
    EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 0u);
}

/// A table born by a log tail alone (no snapshot yet): `namespace_birth` with nothing else is a legal
/// Live-but-empty table.
TEST(RefWriterRecovery, BirthOnlyLogNoSnapshotRecoversToEmptyLiveTable)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/birth_only"};

    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 1}, {namespaceBirthOp()}});

    auto store = openStore(backend);
    EXPECT_TRUE(store->listRefs(ns).empty());
}

/// Empty base + birth log recovery (spec unit test list): birth and the first precommit->promote span
/// TWO separate log transactions with no snapshot at all.
TEST(RefWriterRecovery, BirthPlusPrecommitPromoteAcrossTwoLogsNoSnapshot)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/birth_then_promote"};
    const ManifestRef m1 = manifestRef(1, 1, 1);

    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 1},
        {namespaceBirthOp(), publishCommittedOps("part_1", m1)[0]}});
    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 2},
        {publishCommittedOps("part_1", m1)[1]}});

    auto store = openStore(backend);
    const auto resolved = store->resolveRef(ns, "part_1");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->manifest_id.ref, m1);
    EXPECT_EQ(resolved->manifest_id.root_namespace, ns);

    const auto refs = store->listRefs(ns);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_TRUE(refs.contains("part_1"));
}

/// Latest snapshot plus tail recovery (spec unit test list): a snapshot covering ref "a", a tail that
/// drops "a" and publishes "b", and a STALE log at/below the snapshot id that must be ignored (its
/// content, if replayed, would corrupt the result -- proving the "ignore log keys at or below the
/// selected snapshot" rule).
TEST(RefWriterRecovery, SnapshotPlusTailRecovery)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/snap_tail"};
    const ManifestRef ma = manifestRef(1, 1, 1);
    const ManifestRef mb = manifestRef(1, 2, 1);

    /// A stale log BELOW the snapshot id would, if wrongly replayed, try to add "a" a second time
    /// (the snapshot already contains it) and throw -- proving it must be ignored, not merely benign.
    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 3},
        {namespaceBirthOp(), publishCommittedOps("a", ma)[0], publishCommittedOps("a", ma)[1]}});
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{1, 5}, {committedRow("a", ma)}));

    std::vector<RefOp> tail_ops;
    tail_ops.push_back([&] { RefOp op; op.kind = RefOpKind::OwnerTransition;
        op.old_binding = RefOwnerBinding{RefOwnerKind::Committed, "a", ma}; return op; }());
    tail_ops.push_back(publishCommittedOps("b", mb)[0]);
    tail_ops.push_back(publishCommittedOps("b", mb)[1]);
    writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{1, 6}, tail_ops});

    auto store = openStore(backend);
    EXPECT_FALSE(store->resolveRef(ns, "a").has_value());
    const auto b = store->resolveRef(ns, "b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->manifest_id.ref, mb);
    EXPECT_EQ(store->listRefs(ns).size(), 1u);
}

/// Restart-on-vanish (spec §Startup And Recovery): the selected snapshot vanishes between the LIST and
/// its GET (here: a concurrent GC cleanup+republish race is simulated by publishing a NEWER, covering
/// snapshot exactly when the vanish fires). Recovery must restart with a fresh LIST and converge on the
/// newer snapshot, not treat the vanish as corruption.
TEST(RefWriterRecovery, RestartOnVanishConvergesOnNewerSnapshot)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/vanish_race"};
    const ManifestRef ma = manifestRef(1, 1, 1);
    const ManifestRef mb = manifestRef(1, 2, 1);

    const RefTxnId snap_x{1, 10};
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), snap_x, {committedRow("a", ma)}));
    backend->vanish_once_keys.insert(layout.refSnapshotKey(ns, snap_x));
    backend->on_vanish_fire = [&]
    {
        const RefTxnId snap_y{1, 20};
        writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), snap_y, {committedRow("b", mb)}));
    };

    auto store = openStore(backend);
    const auto b = store->resolveRef(ns, "b");
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->manifest_id.ref, mb);
    EXPECT_FALSE(store->resolveRef(ns, "a").has_value()) << "must converge on snapshot Y, not a mix of X and Y";
    EXPECT_EQ(store->refRecoveryRestartsForTest(ns), 1u);
}

/// A DIFFERENT valid object at the exact snapshot key (not merely absent) is corruption, never a
/// restart signal -- pins the boundary between "vanished" (restart) and "corrupt" (fail closed).
TEST(RefWriterRecovery, DifferentBytesAtSelectedSnapshotIsCorruptionNotRestart)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/corrupt_snap"};
    const RefTxnId snap_x{1, 10};

    /// A structurally-valid snapshot BODY, but for a DIFFERENT namespace, placed under `ns`'s own key
    /// (a copy-under-the-wrong-prefix scenario) -- decodeRefTableSnapshot's key/body cross-check must
    /// reject it, never treat it as a restart signal.
    const RootNamespace other_ns{"srv1/other"};
    DB::Cas::RefTableSnapshot foreign;
    foreign.ns = other_ns.string();
    foreign.snapshot_id = snap_x;
    foreign.lifecycle = RefLifecycle::Live;
    backend->putIfAbsent(layout.refSnapshotKey(ns, snap_x), DB::Cas::encodeRefTableSnapshot(foreign));

    auto store = openStore(backend);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->resolveRef(ns, "anything"); });
}

/// ===================================================================================
/// Append lane: request cost + batching (spec §Common Mutation Path / §Local Batching Queue)
/// ===================================================================================

TEST(RefWriterAppendLane, WarmIsolatedMutationCostsOneCreateZeroReads)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/warm"};
    publishEmptyPart(store, ns, "part_1");   /// setup: births + populates the table (not measured)
    ASSERT_TRUE(store->resolveRef(ns, "part_1").has_value());

    const uint64_t get_before = backend->getTotal();
    const uint64_t list_before = backend->listTotal();
    const uint64_t put_before = backend->putTotal();

    store->dropRef(ns, "part_1");

    EXPECT_EQ(backend->getTotal(), get_before) << "a warm mutation performs no read request";
    EXPECT_EQ(backend->listTotal(), list_before) << "a warm mutation performs no LIST";
    EXPECT_EQ(backend->putTotal(), put_before + 1) << "exactly one body PUT with create-if-absent";
    EXPECT_FALSE(store->resolveRef(ns, "part_1").has_value());
}

/// `B` compatible queued mutations share one create (spec §Writer Budget).
TEST(RefWriterAppendLane, CompatibleMutationsShareOneCreate)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/cobatch"};
    publishEmptyPart(store, ns, "a");
    publishEmptyPart(store, ns, "b");
    ASSERT_TRUE(store->resolveRef(ns, "a").has_value());
    ASSERT_TRUE(store->resolveRef(ns, "b").has_value());

    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    store->setRefPreCarveHookForTest([&]
    {
        std::unique_lock lk(m);
        if (entered)
            return;   /// only the leader's own first carve blocks; a second flush (if any) proceeds
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return store->refQueuePendingForTest(ns) >= 2; });
    });

    const uint64_t put_before = backend->putTotal();
    std::thread t_a([&] { store->dropRef(ns, "a"); });
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return entered; });
    }
    std::thread t_b([&] { store->dropRef(ns, "b"); });
    while (store->refQueuePendingForTest(ns) < 2)
        std::this_thread::yield();
    cv.notify_all();   /// wakes the pre-carve hook's own wait once its predicate (>=2 pending) holds
    t_a.join();
    t_b.join();
    store->setRefPreCarveHookForTest(nullptr);

    EXPECT_EQ(backend->putTotal(), put_before + 1) << "both drops must land in ONE created log object";
    EXPECT_FALSE(store->resolveRef(ns, "a").has_value());
    EXPECT_FALSE(store->resolveRef(ns, "b").has_value());
}

/// An invalid queued request returns its own exception without entering the transaction; the
/// co-batched neighbor still lands, in the SAME one create.
TEST(RefWriterAppendLane, InvalidBatchEntryGetsOwnExceptionBatchSurvives)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/invalid_entry"};
    publishEmptyPart(store, ns, "good");

    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    store->setRefPreCarveHookForTest([&]
    {
        std::unique_lock lk(m);
        if (entered)
            return;
        entered = true;
        cv.notify_all();
        cv.wait(lk, [&] { return store->refQueuePendingForTest(ns) >= 2; });
    });

    const uint64_t put_before = backend->putTotal();
    std::exception_ptr bad_error;
    std::thread t_bad([&]
    {
        try { store->dropRef(ns, "does_not_exist"); }
        catch (...) { bad_error = std::current_exception(); }
    });
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return entered; });
    }
    std::thread t_good([&] { store->dropRef(ns, "good"); });
    while (store->refQueuePendingForTest(ns) < 2)
        std::this_thread::yield();
    cv.notify_all();
    t_bad.join();
    t_good.join();
    store->setRefPreCarveHookForTest(nullptr);

    ASSERT_TRUE(bad_error != nullptr) << "the invalid item's OWN caller must receive its exception";
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { std::rethrow_exception(bad_error); });
    EXPECT_EQ(backend->putTotal(), put_before + 1) << "the survivor's own transaction still costs one create";
    EXPECT_FALSE(store->resolveRef(ns, "good").has_value()) << "the innocent co-batched drop must land";
}

/// ===================================================================================
/// Append lane: wedge semantics (spec §Writer-Side Linearization)
/// ===================================================================================

TEST(RefWriterAppendLane, WedgedLaneBlocksSameTableWhileOtherTableProceeds)
{
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend, budget);
    const Layout & layout = store->layout();
    const RootNamespace ns_a{"srv1/wedge_a"};
    const RootNamespace ns_b{"srv1/wedge_b"};
    publishEmptyPart(store, ns_a, "x");
    publishEmptyPart(store, ns_b, "y");

    backend->fault_key_substr = layout.refsNamespacePrefix(ns_a) + "_log/";
    backend->fault_count = 1;

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropRef(ns_a, "x"); });
    EXPECT_TRUE(store->refLaneWedgedForTest(ns_a));

    /// A different table proceeds normally while ns_a stays wedged.
    EXPECT_NO_THROW(store->dropRef(ns_b, "y"));
    EXPECT_FALSE(store->resolveRef(ns_b, "y").has_value());

    /// Retrying ns_a does not allocate a later id: the wedge's own key was never actually written
    /// (the fault never wrote through), so resolution still finds it absent -- still uncertain.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropRef(ns_a, "x"); });
    EXPECT_TRUE(store->refLaneWedgedForTest(ns_a));
    EXPECT_TRUE(store->resolveRef(ns_a, "x").has_value()) << "the wedged (never-resolved) drop must not have applied";
}

TEST(RefWriterAppendLane, WedgedAppendObservedDurableAppliesBeforeNextId)
{
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 100;
    budget.lease_safety_margin_ms = 100;

    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend, budget);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/wedge_unwedge"};
    publishEmptyPart(store, ns, "x");
    publishEmptyPart(store, ns, "y");

    backend->fault_key_substr = layout.refsNamespacePrefix(ns) + "_log/";
    backend->fault_count = 1;

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropRef(ns, "x"); });
    ASSERT_TRUE(store->refLaneWedgedForTest(ns));
    ASSERT_TRUE(store->resolveRef(ns, "x").has_value()) << "not yet applied while wedged";

    /// The earlier request eventually lands server-side; the caller just never saw the ack.
    backend->materializePendingDelayedWrite();

    /// A later mutation on the SAME table first resolves the wedge (applying "drop x" to cache) BEFORE
    /// allocating its own next id (which drops "y").
    EXPECT_NO_THROW(store->dropRef(ns, "y"));

    EXPECT_FALSE(store->refLaneWedgedForTest(ns));
    EXPECT_FALSE(store->resolveRef(ns, "x").has_value()) << "the wedged drop was applied on resolution";
    EXPECT_FALSE(store->resolveRef(ns, "y").has_value()) << "the next mutation committed normally afterward";
}

/// ===================================================================================
/// Task 11: snapshot publication (spec §writer-snapshot-publication)
/// ===================================================================================

/// The count threshold fires a background publish covering the whole retained tail; its bytes must
/// equal an INDEPENDENT oracle's replay of the same logs through the published id (cache-replay
/// equivalence), and the retained tail must be fully pruned afterward (spec: "Publication is
/// background and never blocks an append").
TEST(RefWriterSnapshotPublish, ThresholdTriggerPublishesCacheReplayEquivalentBytes)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    PoolConfig config;
    config.snapshot_log_count_threshold = 3;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    config.snapshot_min_log_age_ms = 0;
    auto store = openStoreWithConfig(backend, config);
    const RootNamespace ns{"srv1/threshold_publish"};

    publishEmptyPart(store, ns, "a");   /// tail: 2 (birth+add, promote)
    publishEmptyPart(store, ns, "b");   /// tail: 3, then 4 (4 > 3 -> dispatches ONE background publish)

    store->waitForSnapshotPublishSettleForTest(ns);

    const auto snap_id = listGreatestSnapshotIdForTest(*backend, layout, ns);
    ASSERT_TRUE(snap_id.has_value()) << "the threshold trigger must have published a snapshot";
    EXPECT_EQ(store->newestPublishedSnapshotIdForTest(ns), snap_id);
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), 0u) << "a snapshot covering everything prunes the whole tail";

    const auto got = backend->get(layout.refSnapshotKey(ns, *snap_id));
    ASSERT_TRUE(got.has_value());

    /// The independent oracle: replay every `_log/` object directly, ignoring the snapshot entirely.
    const RefTableState oracle = independentFullReplayForTest(*backend, layout, ns, *snap_id);
    const String expected_bytes = encodeRefTableSnapshot(snapshotOf(oracle, ns.string()));
    EXPECT_EQ(got->bytes, expected_bytes) << "published snapshot bytes must equal replay(logs through X)";
}

/// A fresh mount that recovers a large PRE-EXISTING tail (left by a predecessor whose own thresholds
/// never fired) must trigger its own publish from a plain READ (resolveRef/listRefs), not only from a
/// write -- the mount-time trigger (spec: "or right after recovery replays a tail already above one").
TEST(RefWriterSnapshotPublish, MountTimeTriggerPublishesAfterRecoveryReplaysLargeTail)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/mount_time_publish"};

    {
        /// Predecessor: default (high) thresholds, so nothing publishes yet. 3 parts -> 6 tail entries.
        auto predecessor = openStore(backend);
        publishEmptyPart(predecessor, ns, "a");
        publishEmptyPart(predecessor, ns, "b");
        publishEmptyPart(predecessor, ns, "c");
    }   /// mount released; the tail is durable but nothing has published it

    PoolConfig config;
    config.snapshot_log_count_threshold = 3;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    config.snapshot_min_log_age_ms = 0;
    auto successor = openStoreWithConfig(backend, config);

    /// A mere READ triggers recovery; recovery alone must dispatch the mount-time publish (the table is
    /// never otherwise mutated by this mount).
    EXPECT_EQ(successor->listRefs(ns).size(), 3u);
    successor->waitForSnapshotPublishSettleForTest(ns);

    const auto snap_id = listGreatestSnapshotIdForTest(*backend, layout, ns);
    ASSERT_TRUE(snap_id.has_value()) << "the mount-time trigger must have published a snapshot";
    EXPECT_EQ(successor->tailSinceSnapshotCountForTest(ns), 0u);

    const auto got = backend->get(layout.refSnapshotKey(ns, *snap_id));
    ASSERT_TRUE(got.has_value());
    const RefTableState oracle = independentFullReplayForTest(*backend, layout, ns, *snap_id);
    EXPECT_EQ(got->bytes, encodeRefTableSnapshot(snapshotOf(oracle, ns.string())));
}

/// Grace age (spec §Late Predecessor PUT): a candidate id never covers a log younger than
/// `snapshot_min_log_age_ms`. Drives `trySnapshotPublishOnce` directly (bypassing thresholds/background
/// dispatch entirely) against an injected fake clock for a fully deterministic age check.
TEST(RefWriterSnapshotPublish, GraceAgeRespectedYoungLogNotCovered)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/grace_age"};
    uint64_t fake_now = 1000;

    PoolConfig config;
    config.snapshot_min_log_age_ms = 60000;
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    /// The local write fence's deadline is ALSO measured off this same fake clock (armed at open as
    /// `bootMsNow() + mount_lease_ttl_ms`); widen it well past the clock jump below so advancing the
    /// fake clock to exercise the grace age does not ALSO trip the (unrelated) mount-lease fence.
    config.mount_lease_ttl_ms = std::chrono::milliseconds(10'000'000);
    auto store = openStoreWithConfig(backend, config);

    publishEmptyPart(store, ns, "a");   /// both commits stamped observed_at_ms = 1000

    EXPECT_FALSE(store->trySnapshotPublishOnce(ns)) << "every tail entry is still within the grace window";
    EXPECT_FALSE(listGreatestSnapshotIdForTest(*backend, layout, ns).has_value());

    fake_now += 61000;   /// past the 60s grace window
    EXPECT_TRUE(store->trySnapshotPublishOnce(ns));
    const auto snap_id = listGreatestSnapshotIdForTest(*backend, layout, ns);
    ASSERT_TRUE(snap_id.has_value());
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), 0u);
}

/// Publication must never block a concurrent append on the SAME table (spec: "Publication is
/// background and never blocks an append"): while a dispatched background publish is stuck mid-PUT, an
/// ordinary mutation on the table must still complete promptly (a real deadlock would hang this test).
TEST(RefWriterSnapshotPublish, PublicationNeverBlocksConcurrentAppend)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/publish_no_block"};
    PoolConfig config;
    config.snapshot_log_count_threshold = 3;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    config.snapshot_min_log_age_ms = 0;
    auto store = openStoreWithConfig(backend, config);

    backend->armPutBlock("_snap/");

    publishEmptyPart(store, ns, "a");
    publishEmptyPart(store, ns, "b");   /// tail reaches 4 (> 3) -> dispatches a background publish

    backend->awaitBlockEntered();   /// the dispatched attempt is now stuck mid-PUT on the snapshot key

    /// An unrelated mutation on the SAME table must complete without waiting for the stuck publish.
    EXPECT_NO_THROW(store->dropRef(ns, "a"));
    EXPECT_FALSE(store->resolveRef(ns, "a").has_value());

    backend->releaseBlock();
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_TRUE(listGreatestSnapshotIdForTest(*backend, layout, ns).has_value());
}

/// Review caution (T10 review): a dispatched background publish must never outlive the Store object
/// it operates on -- `maybeScheduleSnapshotPublish` captures `shared_from_this()` BY VALUE into the
/// dispatch lambda specifically to guarantee this (the classic "background thread references a
/// dangling owner" shutdown segfault, avoided here since a shared_ptr copy keeps the object alive for
/// as long as the thread holds it, regardless of what every OTHER holder does). Proves it directly:
/// blocks a dispatched publish mid-PUT, drops the TEST's own (only) Store handle while still blocked,
/// and confirms via a `weak_ptr` that the Store demonstrably survives on the blocked thread's own
/// reference alone. Then unblocks it with no live Store handle anywhere in this test any more -- a
/// dangling-pointer crash here would abort the whole test binary, the strongest possible signal for
/// this specific hazard.
TEST(RefWriterSnapshotPublish, PublishThreadOutlivesDroppedStoreHandleWithoutCrashing)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const RootNamespace ns{"srv1/publish_outlives_store"};
    PoolConfig config;
    config.snapshot_log_count_threshold = 3;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    config.snapshot_min_log_age_ms = 0;
    auto store = openStoreWithConfig(backend, config);

    backend->armPutBlock("_snap/");
    publishEmptyPart(store, ns, "a");
    publishEmptyPart(store, ns, "b");   /// tail reaches 4 (> 3) -> dispatches a background publish
    backend->awaitBlockEntered();       /// stuck mid-PUT, holding its OWN shared_ptr<Store> copy

    std::weak_ptr<Store> weak_store = store;
    store.reset();   /// drop the ONLY Store handle this test holds
    EXPECT_FALSE(weak_store.expired())
        << "the blocked background thread's own shared_ptr copy must keep the Store alive";

    backend->releaseBlock();
    /// Deterministic, sleep-free: waits for the blocked call to actually RETURN (not merely unblock),
    /// entirely through the backend -- this test holds no Store handle to wait on any more.
    backend->awaitBlockedCallCompleted();
}

/// ===================================================================================
/// Task 11: successor stale-precommit cleanup (spec §Clean Up Old Precommits)
/// ===================================================================================

/// A predecessor's dangling (never-promoted) precommits are swept by the successor mount's first touch
/// of the table; a precommit the SUCCESSOR itself adds under its OWN (current) epoch must survive.
TEST(RefWriterStalePrecommitSweep, SweepsOnlyStaleEpochPrecommitsKeepsCurrentEpoch)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const RootNamespace ns{"srv1/precommit_sweep_basic"};

    {
        /// A predecessor writer leaves THREE precommits dangling (a crash before promote).
        auto predecessor = openStore(backend);
        for (const String & name : {"stale_a", "stale_b", "stale_c"})
        {
            auto build = startBuildFor(predecessor, ns, name);
            const ManifestId id = build->stageManifest({});
            build->precommitAdd(ns, name, id);
            /// no promote -- left dangling, as a crashed build would leave it
        }
    }   /// predecessor destroyed: its mount lease is released

    /// The successor allocates a strictly higher durable writer_epoch; its own FRESH precommit must
    /// survive the sweep its very first touch of the table triggers.
    auto successor = openStore(backend);
    auto build = startBuildFor(successor, ns, "fresh_x");
    const ManifestId fresh_id = build->stageManifest({});
    build->precommitAdd(ns, "fresh_x", fresh_id);   /// this call's own appendRefOps hoists the sweep first

    const RefTableState replayed = independentFullReplayForTest(*backend, successor->layout(), ns);
    EXPECT_EQ(replayed.lifecycle, RefLifecycle::Live);
    EXPECT_TRUE(replayed.committed.empty());
    ASSERT_EQ(replayed.precommits.size(), 1u);
    EXPECT_EQ(replayed.precommits.begin()->first, "fresh_x");
    EXPECT_EQ(replayed.precommits.begin()->second, fresh_id.ref);
}

/// The sweep chunks its removal to `ref_txn_max_ops` (1000) stale precommits per transaction (spec
/// §Clean Up Old Precommits), and an interruption (an uncertain PUT, wedging the lane) leaves the
/// remainder harmlessly for a LATER mount's own fresh recovery to finish -- "each chunk re-reads the
/// LIVE state, so a partial sweep just leaves fewer stale bindings for the next chunk (or the next
/// mount's recovery) to find."
TEST(RefWriterStalePrecommitSweep, BoundedBatchesAndInterruptionResumeAcrossMounts)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/precommit_sweep_bounded"};
    constexpr int kTotalStale = 1200;   /// > ref_txn_max_ops (1000): forces at least two removal chunks

    uint64_t e1;
    {
        auto predecessor = openStore(backend);
        e1 = predecessor->writerEpoch();
    }   /// predecessor released; only its epoch is needed -- the stale precommits are seeded raw below

    /// Seed kTotalStale precommits directly (bypassing any Store) under the predecessor's epoch,
    /// spread over two raw log objects (each within the per-transaction 1000-op ENCODE cap) so recovery
    /// costs only two GETs, not kTotalStale of them.
    {
        std::vector<RefOp> ops1;
        ops1.push_back(namespaceBirthOp());
        for (int i = 0; i < 700; ++i)
        {
            RefOp op;
            op.kind = RefOpKind::OwnerTransition;
            op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "stale_" + std::to_string(i), manifestRef(e1, static_cast<uint64_t>(i + 1), 1)};
            ops1.push_back(op);
        }
        writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{e1, 1}, ops1});

        std::vector<RefOp> ops2;
        for (int i = 700; i < kTotalStale; ++i)
        {
            RefOp op;
            op.kind = RefOpKind::OwnerTransition;
            op.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "stale_" + std::to_string(i), manifestRef(e1, static_cast<uint64_t>(i + 1), 1)};
            ops2.push_back(op);
        }
        writeRefLogTxnRaw(*backend, layout, RefLogTxn{ns.string(), RefTxnId{e1, 2}, ops2});
    }

    /// The successor: a tight retry budget so ONE simulated ambiguous response wedges rather than
    /// transparently retries away (mirrors the existing wedge-semantics tests in this file exactly).
    CasRequestBudget budget;
    budget.max_attempts = 1;
    budget.attempt_timeout_ms = 100;
    budget.operation_deadline_ms = 100;
    budget.lease_safety_margin_ms = 100;
    PoolConfig config;
    config.cas_request_budget = budget;
    auto successor = openStoreWithConfig(backend, config);

    backend->fault_key_substr = layout.refsNamespacePrefix(ns) + "_log/";
    backend->fault_count = 1;   /// hits exactly the sweep's FIRST removal chunk's PUT

    /// The sweep is piggybacked on this mount's very first touch; its (uncertain) failure is INSULATED
    /// from the read (resolveRef/listRefs call `sweepStalePrecommitsForRead`, not
    /// `maybeSweepStalePrecommits` directly): the read itself still succeeds, the failure is counted.
    const uint64_t deferred_before = ProfileEvents::global_counters[ProfileEvents::CasRefSweepDeferred].load();
    EXPECT_NO_THROW(successor->listRefs(ns));
    const uint64_t deferred_after = ProfileEvents::global_counters[ProfileEvents::CasRefSweepDeferred].load();
    EXPECT_EQ(deferred_after, deferred_before + 1)
        << "the read-only caller must observe (and count) the deferred sweep failure, not throw";
    EXPECT_TRUE(successor->refLaneWedgedForTest(ns));

    /// The first chunk's request actually landed server-side; the caller just never saw the ack.
    backend->materializePendingDelayedWrite();
    successor.reset();   /// abandoned mid-sweep WITHOUT ever resolving its own wedge in-memory

    /// A THIRD mount (successor-of-the-successor): fresh recovery replays the two raw seed logs PLUS the
    /// first chunk's now-durable removal, sees `needs_stale_precommit_sweep` armed again, and finishes
    /// the remaining stale precommits in exactly one further chunk (<= 1000 remain).
    auto resumer = openStore(backend);
    EXPECT_NO_THROW(resumer->listRefs(ns));

    const RefTableState final_state = independentFullReplayForTest(*backend, layout, ns);
    EXPECT_EQ(final_state.lifecycle, RefLifecycle::Live);
    EXPECT_TRUE(final_state.precommits.empty()) << "every stale precommit must eventually be swept";

    /// Bounded batches: exactly two NEW removal transactions (epoch > e1) were needed for kTotalStale
    /// items -- never one (would violate the 1000-op cap) and never kTotalStale individual ones.
    size_t new_log_objects = 0;
    {
        String cursor;
        for (;;)
        {
            const ListPage page = backend->list(layout.refsNamespacePrefix(ns), cursor, 1000);
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = layout.parseRefObjectKey(lk.key);
                if (parsed && parsed->ns == ns && parsed->kind == RefObjectKind::Log && parsed->txn_id.writer_epoch != e1)
                    ++new_log_objects;
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }
    EXPECT_EQ(new_log_objects, 2u);
}

/// ===================================================================================
/// Task 11: namespace removal (spec §Namespace Removal)
/// ===================================================================================

/// dropNamespace's ONE body transaction names an exact removal for every committed ref AND every
/// dangling precommit, with `remove_namespace` as the FINAL op -- never any other shape.
TEST(RefWriterNamespaceRemoval, TxnNamesEveryOwnerThenRemoveNamespace)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/remove_shape"};

    publishEmptyPart(store, ns, "committed_1");
    publishEmptyPart(store, ns, "committed_2");
    /// One precommit left dangling (never promoted) so the removal txn must ALSO name it.
    auto build = startBuildFor(store, ns, "dangling");
    const ManifestId dangling_id = build->stageManifest({});
    build->precommitAdd(ns, "dangling", dangling_id);

    store->dropNamespace(ns);

    /// The newest `_log/` object for `ns` is the removal transaction.
    std::optional<RefTxnId> newest_log;
    {
        String cursor;
        for (;;)
        {
            const ListPage page = backend->list(layout.refsNamespacePrefix(ns), cursor, 1000);
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = layout.parseRefObjectKey(lk.key);
                if (parsed && parsed->ns == ns && parsed->kind == RefObjectKind::Log
                    && (!newest_log || *newest_log < parsed->txn_id))
                    newest_log = parsed->txn_id;
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
    }
    ASSERT_TRUE(newest_log.has_value());
    const auto got = backend->get(layout.refLogKey(ns, *newest_log));
    ASSERT_TRUE(got.has_value());
    const RefLogTxn removal_txn = decodeRefLogTxn(got->bytes, ns.string(), *newest_log);

    ASSERT_FALSE(removal_txn.ops.empty());
    EXPECT_EQ(removal_txn.ops.back().kind, RefOpKind::RemoveNamespace);
    size_t owner_removals = 0;
    for (size_t i = 0; i + 1 < removal_txn.ops.size(); ++i)
    {
        const RefOp & op = removal_txn.ops[i];
        EXPECT_EQ(op.kind, RefOpKind::OwnerTransition);
        EXPECT_TRUE(op.old_binding.has_value());
        EXPECT_FALSE(op.new_binding.has_value());
        ++owner_removals;
    }
    EXPECT_EQ(owner_removals, 3u) << "2 committed + 1 dangling precommit";
}

/// After the removal transaction is durable, dropNamespace publishes the constant-size `Removed`
/// snapshot; the retained tail is fully pruned by it (constant-size going forward).
TEST(RefWriterNamespaceRemoval, RemovedSnapshotPublished)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"srv1/remove_snapshot"};

    publishEmptyPart(store, ns, "a");
    store->dropNamespace(ns);

    const auto remove_id = store->newestPublishedSnapshotIdForTest(ns);
    ASSERT_TRUE(remove_id.has_value());
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), 0u);

    const auto got = backend->get(layout.refSnapshotKey(ns, *remove_id));
    ASSERT_TRUE(got.has_value());
    const RefTableSnapshot snap = decodeRefTableSnapshot(got->bytes, ns.string(), *remove_id);
    EXPECT_EQ(snap.lifecycle, RefLifecycle::Removed);
    ASSERT_TRUE(snap.remove_txn_id.has_value());
    EXPECT_EQ(*snap.remove_txn_id, *remove_id);
    EXPECT_TRUE(snap.committed.empty());
    EXPECT_TRUE(snap.precommits.empty());
}

/// Review fix (prerequisite to this task's dropNamespace rewiring): `flushRefBatch`'s per-item
/// validation previously previewed each op as its OWN single-op trial transaction, so a
/// whole-transaction-shape rule ("remove_namespace must be the FINAL op") trivially passed on every
/// singleton slice regardless of an item's REAL combined shape -- a malformed item would only have
/// been caught by the post-persist apply, AFTER its transaction object was already durable (bricking
/// the table on every future recovery and permanently wedging this table's lane). Drives
/// `appendRefOps` directly with a deliberately malformed multi-op item (remove_namespace not last) to
/// prove the whole-item shape check now rejects it BEFORE any backend object is created.
TEST(RefWriterNamespaceRemoval, MalformedShapeWithRemoveNamespaceNotFinalRejectedBeforeAnyCreate)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/malformed_shape"};
    publishEmptyPart(store, ns, "a");   /// births the table so the malformed item isn't ALSO rejected
                                        /// for the unrelated reason "namespace_birth was needed first"

    const uint64_t put_before = backend->putTotal();
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        store->appendRefOps(ns, MutationScope::wholeShard(),
            [](const RefTableState &) -> std::vector<RefOp>
            {
                RefOp remove_ns_1;
                remove_ns_1.kind = RefOpKind::RemoveNamespace;
                RefOp remove_ns_2;
                remove_ns_2.kind = RefOpKind::RemoveNamespace;
                return {remove_ns_1, remove_ns_2};   /// remove_namespace NOT the final op -- malformed
            },
            RootMutationOrigin::Writer, RootMutationKind::DropNamespace);
    });

    EXPECT_EQ(backend->putTotal(), put_before) << "the malformed shape must be rejected before any object is created";
    ASSERT_TRUE(store->resolveRef(ns, "a").has_value()) << "the malformed attempt left no trace on the table";
}

/// ===================================================================================
/// Task 11: namespace birth / the recreation gate (spec §Namespace Birth)
/// ===================================================================================

/// Recreating a `Removed` namespace is rejected absent the exact `_cleanup/<remove_txn_id>` marker --
/// even though a FRESH mount's own recovery sees the table as functionally "empty" (the pre-removal
/// logs are all at/below the small Removed snapshot and are ignored by the recovery rule), an
/// empty-looking prefix is never sufficient on its own. Once the marker is observed, birth succeeds and
/// the table's id timeline continues strictly above the old remove_txn_id.
TEST(RefWriterNamespaceBirth, BirthFromRemovedRejectedWithoutMarkerAcceptedWithMarker)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/recreate"};

    RefTxnId remove_id;
    {
        auto store = openStore(backend);
        publishEmptyPart(store, ns, "a");
        store->dropNamespace(ns);
        const auto id = store->newestPublishedSnapshotIdForTest(ns);
        ASSERT_TRUE(id.has_value());
        remove_id = *id;
    }   /// mount released

    /// A fresh mount, no marker observed: rejected.
    {
        auto store2 = openStore(backend);
        auto build = startBuildFor(store2, ns, "reborn");
        const ManifestId id = build->stageManifest({});
        expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->precommitAdd(ns, "reborn", id); });
    }

    /// GC's namespace-cleanup item (Task 12) publishes the exact completion marker -- simulated here
    /// directly via the raw fixture convention, since Task 12 has not landed yet.
    backend->putIfAbsent(layout.refCleanupMarkerKey(ns, remove_id), "");

    /// A further fresh mount, marker now observed: birth succeeds.
    {
        auto store3 = openStore(backend);
        auto build = startBuildFor(store3, ns, "reborn");
        const ManifestId id = build->stageManifest({});
        EXPECT_NO_THROW(build->precommitAdd(ns, "reborn", id));
        build->promote(ns, "reborn", build->buildId(), id);

        const auto resolved = store3->resolveRef(ns, "reborn");
        ASSERT_TRUE(resolved.has_value());
        EXPECT_EQ(resolved->manifest_id.ref, id.ref);

        const RefTableState replayed = independentFullReplayForTest(*backend, layout, ns);
        EXPECT_EQ(replayed.lifecycle, RefLifecycle::Live);
        EXPECT_GT(replayed.greatest_applied, remove_id) << "the reborn timeline continues strictly above the old removal";
    }
}

/// A never-born namespace needs no marker at all (spec: "A never-born table (remove_txn_id absent)
/// needs no marker.").
TEST(RefWriterNamespaceBirth, BirthFromNeverBornNeedsNoMarker)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    auto store = openStore(backend);
    const RootNamespace ns{"srv1/virgin"};

    EXPECT_FALSE(store->observedNamespaceCleanupMarker(ns, RefTxnId{1, 1}));
    EXPECT_NO_THROW(publishEmptyPart(store, ns, "first"));
    EXPECT_TRUE(store->resolveRef(ns, "first").has_value());
}
