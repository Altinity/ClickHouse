#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
extern const int LIMIT_EXCEEDED;
}

/// Task 8 (stage-1 §3 "Budget: counts only, chunked flush"): the counts-only admission caps --
/// `ref_txn_max_ops` (5000), the carve item cap `kMaxRefBatch` (1000), and the per-op size cap
/// `ref_op_max_bytes` (4096 bytes on normal-class ops) -- plus their failure-isolation contract: a
/// single item whose own op count, or whose one op's encoded size, exceeds its cap fails ALONE; a
/// neighbor co-batched into the same flush still commits. `ref_txn_max_ops` is checked exactly (the
/// `build_ops` result's size), and the per-op cap is checked by encoding exactly one op at a time --
/// no accumulation, matching the admission machinery this replaces. T9 (removal-class detection by
/// op inspection) and T10 (chunked flush across a whole-batch op-count overflow) extend this file;
/// this task adds only the per-item / per-op isolation tests and the canonical round-trip leg of
/// test 12 (the maximum legally-admissible normal-class transaction).
///
/// The suite name is prefixed `RefWriter` so it is covered by the `RefWriter*` unit-test gate filter.

using namespace DB::Cas;
using DB::Cas::tests::committedRow;
using DB::Cas::tests::minimalLiveSnapshot;
using DB::Cas::tests::writeRefSnapshotRaw;

namespace
{

PoolPtr openPool(const BackendPtr & backend)
{
    /// A fresh pool with no residue, mirroring the T7 carve suite's `openPool`.
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
}

/// A legal blob-free part: stage an empty manifest, precommit, promote -- enough to leave one
/// committed ref (and a `Live` table) that a later co-batched item can join.
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

/// One queued append (or drop) driven on its own thread; the future becomes ready only when the call
/// RETURNS (normally or by throwing). Mirrors `gtest_cas_ref_carve.cpp`'s `Caller`/`launchDrop`.
struct Caller
{
    std::thread t;
    std::future<std::exception_ptr> fut;
};

Caller launchAppend(const PoolPtr & store, const RootNamespace & ns, MutationScope scope,
                     std::function<std::vector<RefOp>(const RefTableState &)> build_ops)
{
    auto prom = std::make_shared<std::promise<std::exception_ptr>>();
    std::future<std::exception_ptr> fut = prom->get_future();
    std::thread t([store, ns, scope, build_ops, prom]
    {
        std::exception_ptr err;
        try { store->appendRefOps(ns, scope, build_ops, RootMutationOrigin::Writer, RootMutationKind::Publish); }
        catch (...) { err = std::current_exception(); }
        prom->set_value(err);
    });
    return Caller{std::move(t), std::move(fut)};
}

Caller launchDrop(const PoolPtr & store, const RootNamespace & ns, const String & ref)
{
    auto prom = std::make_shared<std::promise<std::exception_ptr>>();
    std::future<std::exception_ptr> fut = prom->get_future();
    std::thread t([store, ns, ref, prom]
    {
        std::exception_ptr err;
        try { store->dropRef(ns, ref); }
        catch (...) { err = std::current_exception(); }
        prom->set_value(err);
    });
    return Caller{std::move(t), std::move(fut)};
}

/// `n` ops that contribute nothing when applied (default-constructed = `NamespaceBirth`) -- safe
/// filler for a `build_ops` result whose only purpose is to overflow the per-item op-count cap; the
/// count check fires before any of these ops is ever applied or otherwise inspected.
std::vector<RefOp> fillerOps(size_t n)
{
    return std::vector<RefOp>(n, RefOp{});
}

/// A zero-padded ref name for index `i`, so `kTotalRefs` names sort in the same order as their index
/// (the snapshot fixture's committed rows must already be sorted by `ref_name`).
String paddedRefName(size_t i)
{
    String s = std::to_string(i);
    return "ref_" + String(6 - s.size(), '0') + s;
}

/// A single `SetPayload` op whose `ref_name` is padded so its OWN encoded size (`encodedOpSize`) is
/// exactly `target_bytes`. Every added 'a' is one un-escaped byte in the JSON ref-name string, so the
/// size grows one-for-one; `checkCanonicalRefName` imposes no length limit, so this stays a valid,
/// merely over-long, canonical ref name.
RefOp paddedSetPayloadOp(size_t target_bytes)
{
    RefOp op;
    op.kind = RefOpKind::SetPayload;
    op.ref_name = "r";
    op.expected_manifest_ref = ManifestRef{1, 1, 1};
    op.payload = "";
    op.published_at_ms = 0;
    const size_t base = encodedOpSize(op);
    op.ref_name = "r" + String(target_bytes - base, 'a');
    return op;
}

/// Blocks the FIRST flush's leader in the pre-carve window until `expected_pending` items are queued,
/// forcing a deterministic multi-item batch (mirrors `gtest_cas_ref_carve.cpp`'s `CaseSync`/pre-carve
/// hook pattern). Only the first carve blocks; retries proceed straight through.
struct CaseSync
{
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
};

void armPreCarveBlock(const PoolPtr & store, const RootNamespace & ns, const std::shared_ptr<CaseSync> & sync, size_t expected_pending)
{
    store->setRefPreCarveHookForTest([sync, store, ns, expected_pending]
    {
        std::unique_lock lk(sync->m);
        if (sync->entered)
            return;
        sync->entered = true;
        sync->cv.notify_all();
        /// Bounded (10s) so a staging bug bounds the wait instead of blocking the whole suite.
        sync->cv.wait_for(lk, std::chrono::seconds(10), [&] { return store->refQueuePendingForTest(ns) >= expected_pending; });
    });
}

void waitEntered(const std::shared_ptr<CaseSync> & sync)
{
    std::unique_lock lk(sync->m);
    sync->cv.wait_for(lk, std::chrono::seconds(10), [&] { return sync->entered; });
}

void waitPendingAtLeast(const PoolPtr & store, const RootNamespace & ns, size_t n)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (store->refQueuePendingForTest(ns) < n && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
}

/// Asserts `err` is non-null and carries EXACTLY `expected_code` -- distinguishes the new counts-only
/// admission checks (`LIMIT_EXCEEDED`) from any other per-item validation failure.
void expectFailedWithCode(const std::exception_ptr & err, int expected_code, const char * what)
{
    ASSERT_TRUE(err != nullptr) << what << ": the caller must observe the admission-cap error";
    try
    {
        std::rethrow_exception(err);
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code) << what;
    }
}

}

/// Test 10 (spec §3 "Oversized item / oversized op fail alone"): an item whose OWN op count exceeds
/// `ref_txn_max_ops` fails alone -- its ops never enter the batch's transaction -- and a co-batched
/// neighbor still commits.
TEST(RefWriterChunkedFlush, OversizedItemFailsAlone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/chunked_oversized_item"};
    publishEmptyPart(store, ns, "neighbor");
    ASSERT_TRUE(store->resolveRef(ns, "neighbor").has_value());

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);

    Caller oversized = launchAppend(store, ns, MutationScope::ref("oversized"),
        [](const RefTableState &) -> std::vector<RefOp> { return fillerOps(ref_txn_max_ops + 1); });
    waitEntered(sync);
    Caller neighbor = launchDrop(store, ns, "neighbor");
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();   /// release the pre-carve hook now its (>=2 pending) predicate holds

    ASSERT_EQ(oversized.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "oversized item must not hang";
    ASSERT_EQ(neighbor.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "neighbor must not hang";
    const std::exception_ptr oversized_err = oversized.fut.get();
    const std::exception_ptr neighbor_err = neighbor.fut.get();
    oversized.t.join();
    neighbor.t.join();
    store->setRefPreCarveHookForTest(nullptr);

    expectFailedWithCode(oversized_err, DB::ErrorCodes::LIMIT_EXCEEDED, "oversized item (op count)");
    EXPECT_TRUE(neighbor_err == nullptr) << "the co-batched neighbor must commit despite the oversized item";
    EXPECT_FALSE(store->resolveRef(ns, "neighbor").has_value()) << "neighbor's drop must have committed";
}

/// Test 10, second leg: one op whose OWN encoded size exceeds `ref_op_max_bytes` (a maximum-length
/// ref name -- `checkCanonicalRefName` imposes no length limit) fails only its item; a co-batched
/// neighbor still commits.
TEST(RefWriterChunkedFlush, OversizedOpFailsItsItemAlone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/chunked_oversized_op"};
    publishEmptyPart(store, ns, "neighbor");
    ASSERT_TRUE(store->resolveRef(ns, "neighbor").has_value());

    const RefOp oversized_op = paddedSetPayloadOp(ref_op_max_bytes + 1);
    ASSERT_GT(encodedOpSize(oversized_op), ref_op_max_bytes);

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);

    Caller oversized = launchAppend(store, ns, MutationScope::ref("oversized_op"),
        [oversized_op](const RefTableState &) -> std::vector<RefOp> { return {oversized_op}; });
    waitEntered(sync);
    Caller neighbor = launchDrop(store, ns, "neighbor");
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();

    ASSERT_EQ(oversized.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "oversized op item must not hang";
    ASSERT_EQ(neighbor.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "neighbor must not hang";
    const std::exception_ptr oversized_err = oversized.fut.get();
    const std::exception_ptr neighbor_err = neighbor.fut.get();
    oversized.t.join();
    neighbor.t.join();
    store->setRefPreCarveHookForTest(nullptr);

    expectFailedWithCode(oversized_err, DB::ErrorCodes::LIMIT_EXCEEDED, "oversized op");
    EXPECT_TRUE(neighbor_err == nullptr) << "the co-batched neighbor must commit despite the oversized op";
    EXPECT_FALSE(store->resolveRef(ns, "neighbor").has_value()) << "neighbor's drop must have committed";
}

/// Test 12, canonical round-trip leg: the maximum legally-admissible normal-class transaction under
/// the new counts-only caps -- `ref_txn_max_ops` ops, each padded to exactly `ref_op_max_bytes` --
/// round-trips comfortably under the whole-transaction `ref_txn_max_bytes` decode cap (5000 * 4096 =
/// 20,480,000 bytes, with framing headroom to spare). Pure codec-level: proves the two counts-only
/// caps compose without ever approaching the byte cap the encode-side estimation machinery used to
/// police.
TEST(RefWriterChunkedFlush, CanonicalMaxTransactionRoundTrips)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    txn.ops.reserve(ref_txn_max_ops);
    for (size_t i = 0; i < ref_txn_max_ops; ++i)
    {
        RefOp op = paddedSetPayloadOp(ref_op_max_bytes);
        ASSERT_EQ(encodedOpSize(op), ref_op_max_bytes);
        txn.ops.push_back(std::move(op));
    }

    const String bytes = encodeRefLogTxn(txn);
    /// Every op contributes exactly `ref_op_max_bytes`; header/meta/trailer framing adds strictly
    /// more on top, and the whole thing still stays well under the 20 MiB decode cap.
    EXPECT_GT(bytes.size(), ref_txn_max_ops * ref_op_max_bytes);
    EXPECT_LT(bytes.size(), ref_txn_max_bytes);

    const RefLogTxn decoded = decodeRefLogTxn(bytes, txn.ns, txn.txn_id);
    EXPECT_EQ(decoded.ops.size(), ref_txn_max_ops);
    EXPECT_EQ(decoded, txn);
}

/// Test 11 (spec §3 "Removal-class detection, falsifiably"): `dropNamespace` over a table with
/// > `ref_txn_max_ops` committed refs builds ONE transaction whose ops (one `owner_transition`
/// removal per ref, plus a terminal `remove_namespace`) exceed the normal-class op-count cap --
/// and must still succeed, because removal-class is byte-budgeted (`ref_removal_max_bytes`, 64 MiB)
/// and has no op-count cap. Seeded via a raw snapshot (not `kTotalRefs` individual writer round-trips
/// through `publishEmptyPart`) so the fixture stays fast; the writer never touches these rows until
/// `dropNamespace` itself builds the one removal transaction.
TEST(RefWriterChunkedFlush, DropNamespaceOverOpCapSucceeds)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/dropns_over_cap"};
    constexpr size_t kTotalRefs = static_cast<size_t>(ref_txn_max_ops) + 200;

    /// Open the store FIRST (still untouched for `ns`) so the seeded snapshot can use THIS mount's own
    /// writer_epoch: namespace recovery is per-namespace and lazy (first touch), so writing the raw
    /// fixture directly to `backend` after open, but before `ns` is ever touched, is observed identically
    /// to writing it before open.
    auto store = openPool(backend);
    const uint64_t epoch = store->writerEpoch();

    /// `allocateRefTxnId`'s sequence counter is POOL-WIDE (one counter per writer incarnation, shared
    /// across every namespace it touches, `CasRefLedger.h:399`), not per-namespace, and starts fresh on
    /// a virgin mount -- so the very first id it allocates, on ANY namespace, is `{epoch, 1}`. A snapshot
    /// seeded directly at `{epoch, 1}` for `ns` would collide with that very first allocation once
    /// `dropNamespace` (below) asks for a real id, since the same id can't be both the seeded
    /// `greatest_applied` and the newly allocated one. Consume the first two allocations on an unrelated
    /// throwaway namespace first, so `ns`'s seeded `greatest_applied` sits safely below whatever
    /// `dropNamespace` allocates later.
    publishEmptyPart(store, RootNamespace{"srv1/_seq_bump_for_dropns_over_cap"}, "bump");

    std::vector<RefCommittedRow> committed;
    committed.reserve(kTotalRefs);
    for (size_t i = 0; i < kTotalRefs; ++i)
        committed.push_back(committedRow(paddedRefName(i), ManifestRef{epoch, i + 1, 1}));
    ASSERT_GT(committed.size(), ref_txn_max_ops);

    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{epoch, 1}, committed));
    ASSERT_EQ(store->listRefs(ns).size(), kTotalRefs);

    DropNamespaceStats stats;
    EXPECT_NO_THROW(stats = store->dropNamespace(ns));
    EXPECT_EQ(stats.committed_refs, kTotalRefs);
    EXPECT_TRUE(store->namespaceIsRemoved(ns));
}

/// Test 11, second leg: `WholeShard` scope ALONE is not the removal-class discriminator -- the
/// stale-precommit reclaim sweep is also `WholeShard`-scoped but is not removal-class
/// (`CasRefLedger.cpp` ~:1979). Only a SYNTHETIC item can pin this: the production stale-precommit
/// sweep self-limits its own chunk size to the op cap, so running it proves nothing (spec's own
/// warning). This item drives `MutationScope::wholeShard()` directly with ops that contain NO
/// `RemoveNamespace` op -- if classification were keyed on scope instead of op inspection, this would
/// be wrongly treated as removal-class and admitted; op-inspection correctly rejects it under the
/// ordinary normal-class op-count cap, exactly like `OversizedItemFailsAlone` above.
TEST(RefWriterChunkedFlush, SyntheticWholeShardNonRemovalRejected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/synthetic_wholeshard_nonremoval"};

    Caller synthetic = launchAppend(store, ns, MutationScope::wholeShard(),
        [](const RefTableState &) -> std::vector<RefOp> { return fillerOps(ref_txn_max_ops + 1); });
    ASSERT_EQ(synthetic.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "synthetic WholeShard item must not hang";
    const std::exception_ptr err = synthetic.fut.get();
    synthetic.t.join();

    expectFailedWithCode(err, DB::ErrorCodes::LIMIT_EXCEEDED,
        "synthetic WholeShard-scoped item with non-removal ops over the op cap");
}
