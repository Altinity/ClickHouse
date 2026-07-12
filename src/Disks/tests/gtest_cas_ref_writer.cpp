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
        return CountingBackend::putIfAbsent(key, bytes, meta);
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
