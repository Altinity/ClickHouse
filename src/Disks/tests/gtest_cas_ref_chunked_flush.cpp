#include <gtest/gtest.h>

#include "config.h"

#include <base/defines.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/tests/cas_killable_query.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadStatus.h>
#include <Common/tests/gtest_global_context.h>

#include <IO/S3Common.h>

#include <Poco/Exception.h>

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
extern const int LIMIT_EXCEEDED;
extern const int CORRUPTED_DATA;
extern const int NETWORK_ERROR;
extern const int LOGICAL_ERROR;
extern const int ABORTED;
}

namespace ProfileEvents
{
extern const Event CASRefBatchFlushes;
extern const Event CASRefBatchedMutations;
extern const Event CASRefSnapshotPublishDispatched;
extern const Event CASRefAppendWedged;
}

/// Task 8 (stage-1 §3 "Budget: counts only, chunked flush"): the counts-only admission caps --
/// `ref_txn_max_ops` (5000), the carve item cap `kMaxRefBatch` (1000), and the per-op size cap
/// `ref_op_max_bytes` (4096 bytes on normal-class ops) -- plus their failure-isolation contract: a
/// single item whose own op count, or whose one op's encoded size, exceeds its cap fails ALONE; a
/// neighbor co-batched into the same flush still commits. `ref_txn_max_ops` is checked exactly (the
/// `build_ops` result's size), and the per-op cap is checked by encoding exactly one op at a time --
/// no accumulation, matching the admission machinery. The per-item / per-op isolation tests and
/// the canonical round-trip leg cover
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
    ///
    /// The FROZEN clock is load-bearing, not hygiene. `CasMountRuntime::refAppendFenceOk` gates every
    /// controlled attempt against `boot_ms_fn`, and with the compiled defaults (mount_lease_ttl_ms
    /// 30000, safety margin 7000) a pool opened on the REAL clock fences itself ~23s later — no
    /// background renewal advances that deadline in a unit-test pool. Every test in this suite is
    /// about chunking and op-caps, none about wall-clock lease behaviour, so any of them that runs
    /// long enough simply dies of an unrelated fence trip: `DropNamespaceOverOpCapSucceeds` (5200
    /// refs) takes 43-65s under a sanitizer and failed deterministically on all three sanitizer CI
    /// builds with `mount fence tripped` — the pre-attempt fence reject, not a real retry
    /// exhaustion. Same artifact, same fix as
    /// `CASPartWriteTxn.ManifestCapEncodedBytesJustUnderStagesSuccessfully` (2026-07-18): decouple the
    /// fence from execution speed. The waits in this file are `steady_clock` timeouts on futures and
    /// condvars, which are unaffected by this injection.
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    return Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test", .boot_ms_fn = [] { return uint64_t{0}; }});
}

/// A legal blob-free part: stage an empty manifest, precommit, promote -- enough to leave one
/// committed ref (and a `Live` table) that a later co-batched item can join.
///
/// Stage B (Task 4-C): pin `ns` to the sentinel before the first real touch -- the ONE choke point
/// every test in this file uses to birth its namespace, before any `launchAppendOps`/`launchAppend`/
/// `launchDrop` call. Several tests separately compute an expected key via
/// `DB::Cas::tests::fixture::fixtureLife(ns)` for verification/fault injection; without this the real
/// production birth mints a random incarnation and those computed keys land nowhere real.
void publishEmptyPart(const PoolPtr & s, const RootNamespace & ns, const String & ref)
{
    DB::Cas::tests::casAdmitRecoverableEntry(*s->poolBackendPtr(), s->layout(), ns, s->liveWriterEpoch());
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

    /// A fatal `ASSERT_*` between launch and the explicit `t.join()` below returns from `TestBody` with
    /// `t` still joinable; `std::thread::~thread` on a joinable thread calls `std::terminate`, aborting
    /// the whole binary and discarding every later test. This destructor is the backstop: on every
    /// success path the explicit join already ran and left nothing for it to do.
    ~Caller() { if (t.joinable()) t.join(); }
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

/// `n` filler ops for a `build_ops` result whose only purpose is to overflow the per-item op-count
/// cap. They are NOT inert-when-applied: a default-constructed op is a `NamespaceBirth`, which throws
/// `CORRUPTED_DATA` ("namespace_birth while already Live") if it were ever applied to the pre-published
/// namespace. The load-bearing safety property is that the count check fires BEFORE any of these ops is
/// applied or otherwise inspected.
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

/// A single `SetPublishedAt` op whose `ref_name` is padded so its OWN encoded size (`encodedOpSize`)
/// is exactly `target_bytes`. Every added 'a' is one un-escaped byte in the JSON ref-name string, so
/// the size grows one-for-one; `checkCanonicalRefName` imposes no length limit, so this stays a
/// valid, merely over-long, canonical ref name.
RefOp paddedSetPublishedAtOp(size_t target_bytes)
{
    RefOp op;
    op.kind = RefOpKind::SetPublishedAt;
    op.ref_name = "r";
    op.expected_manifest_ref = ManifestRef{1, 1, 1};
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
TEST(CASRefWriterChunkedFlush, OversizedItemFailsAlone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/chunked_oversized_item"};
    publishEmptyPart(store, ns, "neighbor");
    ASSERT_TRUE(store->resolveRef(ns, "neighbor").has_value());

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);

    /// `fillerOps` returns default `RefOp{}` of kind `NamespaceBirth`, which names no ref, so the
    /// item's scope name is never compared against anything and the scope check (step 3) would pass
    /// this item even with the op-count cap removed -- the cap is what's under test here, not the scope.
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
TEST(CASRefWriterChunkedFlush, OversizedOpFailsItsItemAlone)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/chunked_oversized_op"};
    publishEmptyPart(store, ns, "neighbor");
    ASSERT_TRUE(store->resolveRef(ns, "neighbor").has_value());

    const RefOp oversized_op = paddedSetPublishedAtOp(ref_op_max_bytes + 1);
    ASSERT_GT(encodedOpSize(oversized_op), ref_op_max_bytes);

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);

    Caller oversized = launchAppend(store, ns, MutationScope::ref(oversized_op.ref_name),
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

/// Per-item isolation of the `MutationScope` validation (`CasRefLedger.cpp`'s `flushRefBatch` step 3):
/// a mis-scoped item co-batched with an innocent neighbor must fail ALONE, and the neighbor's mutation
/// must COMMIT -- not merely avoid throwing -- exactly the batch-isolation shape
/// `OversizedOpFailsItsItemAlone` proves for the step-1 admission caps. Release-arm only: in a debug or
/// sanitizer build the mis-scoped item's `LOGICAL_ERROR` aborts the whole process, taking the co-batched
/// neighbor down with it before either assertion can run.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASRefWriterChunkedFlush, MisScopedItemFailsAloneNeighborCommits)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const RootNamespace ns{"srv1/chunked_misscoped"};
    publishEmptyPart(store, ns, "neighbor");
    ASSERT_TRUE(store->resolveRef(ns, "neighbor").has_value());

    RefOp add;
    add.kind = RefOpKind::OwnerTransition;
    add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, "y", ManifestRef{900000004, 1, 1}};

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);

    Caller misscoped = launchAppend(store, ns, MutationScope::ref("x"),
        [add](const RefTableState &) -> std::vector<RefOp> { return {add}; });
    waitEntered(sync);
    Caller neighbor = launchDrop(store, ns, "neighbor");
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();

    ASSERT_EQ(misscoped.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "mis-scoped item must not hang";
    ASSERT_EQ(neighbor.fut.wait_for(std::chrono::seconds(10)), std::future_status::ready) << "neighbor must not hang";
    const std::exception_ptr misscoped_err = misscoped.fut.get();
    const std::exception_ptr neighbor_err = neighbor.fut.get();
    misscoped.t.join();
    neighbor.t.join();
    store->setRefPreCarveHookForTest(nullptr);

    expectFailedWithCode(misscoped_err, DB::ErrorCodes::LOGICAL_ERROR, "mis-scoped item");
    EXPECT_TRUE(neighbor_err == nullptr) << "the co-batched neighbor must commit despite the mis-scoped item";
    EXPECT_FALSE(store->resolveRef(ns, "neighbor").has_value()) << "neighbor's drop must have committed";
}
#endif

/// Test 12, canonical round-trip leg: the maximum legally-admissible normal-class transaction under
/// the new counts-only caps -- `ref_txn_max_ops` ops, each padded to exactly `ref_op_max_bytes` --
/// round-trips comfortably under the whole-transaction `ref_txn_max_bytes` decode cap (5000 * 4096 =
/// 20,480,000 bytes, with framing headroom to spare). Pure codec-level: proves the two counts-only
/// caps compose without ever approaching the byte cap the encode-side estimation machinery used to
/// police.
TEST(CASRefWriterChunkedFlush, CanonicalMaxTransactionRoundTrips)
{
    RefLogTxn txn;
    txn.ns = "ns";
    txn.txn_id = RefTxnId{1, 1};
    txn.ops.reserve(ref_txn_max_ops);
    for (size_t i = 0; i < ref_txn_max_ops; ++i)
    {
        RefOp op = paddedSetPublishedAtOp(ref_op_max_bytes);
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
TEST(CASRefWriterChunkedFlush, DropNamespaceOverOpCapSucceeds)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/dropns_over_cap"};
    constexpr size_t kTotalRefs = static_cast<size_t>(ref_txn_max_ops) + 200;

    /// Open the store FIRST (still untouched for `ns`) so the seeded snapshot can use THIS mount's own
    /// writer_epoch: namespace recovery is per-namespace and lazy (first touch), so writing the raw
    /// fixture directly to `backend` after open, but before `ns` is ever touched, is observed identically
    /// to writing it before open.
    auto store = openPool(backend);
    const uint64_t epoch = store->writerEpoch();
    /// Stage B (Task 4-C): pin `ns` to the sentinel now, before the raw snapshot below -- `listRefs`/
    /// `dropNamespace` further down are real production reads that trigger `resolveNamespaceLife`,
    /// which for an UNADMITTED namespace mints a fresh RANDOM incarnation rather than adopting the
    /// sentinel the raw fixture wrote at. Pinning first makes them adopt it instead.
    DB::Cas::tests::fixture::admitLive(*backend, layout, ns);

    /// Ids are PER-NAMESPACE and derived from the table's own `greatest_applied` (INV-1), so seeding
    /// `ns` at `{epoch, 1}` is all this fixture has to do: the `dropNamespace` below derives `{epoch, 2}`
    /// from the seeded snapshot, and no other namespace's traffic can move it.
    std::vector<RefCommittedRow> committed;
    committed.reserve(kTotalRefs);
    for (size_t i = 0; i < kTotalRefs; ++i)
        committed.push_back(committedRow(paddedRefName(i), ManifestRef{epoch, i + 1, 1}));
    ASSERT_GT(committed.size(), ref_txn_max_ops);

    /// Recovery's checkpoint anchor includes the same-id ordinary log. The synthetic snapshot stands
    /// for a long prior history, while this genesis record supplies the retained non-seal witness the
    /// real publisher would necessarily leave at the selected id.
    DB::Cas::tests::fixture::writeRefLogRaw(*backend, layout, RefLogTxn{
        .ns = ns.string(),
        .txn_id = RefTxnId{epoch, 1},
        .ops = {DB::Cas::tests::namespaceBirthOp()},
        .prev_epoch_seal = std::nullopt});
    writeRefSnapshotRaw(*backend, layout, minimalLiveSnapshot(ns.string(), RefTxnId{epoch, 1}, committed));
    DB::Cas::tests::writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = epoch,
        .committed_through = RefTxnId{epoch, 1},
        .checkpoint_snapshot_id = RefTxnId{epoch, 1},
        .last_epoch_seal = std::nullopt,
    });
    CasRequests catalog_requests(backend, Fence::open(), {}, {}, nullptr, DB::Cas::kDefaultRequestWindowMs);
    CasOperation catalog_op = catalog_requests.admit();
    const NamespaceLifeId life = CasRefCatalog::lifeIfCataloged(catalog_op, layout, ns).value();
    backend->resetCounts();
    ASSERT_EQ(store->listRefs(ns).size(), kTotalRefs);
    EXPECT_EQ(backend->getCount(layout.refLogKey(life, RefTxnId{epoch, 1})), 1u);
    EXPECT_EQ(backend->getCount(layout.refSnapshotKey(life, RefTxnId{epoch, 1})), 1u);

    DropNamespaceStats stats;
    EXPECT_NO_THROW(stats = store->dropNamespace(ns));
    EXPECT_EQ(stats.committed_refs, kTotalRefs);
    EXPECT_EQ(CasRefCatalog::read(catalog_op, layout).catalog.entries.front().state, NsState::Removing);
}

/// Test 11, second leg: `WholeShard` scope ALONE is not the removal-class discriminator -- the
/// stale-precommit reclaim sweep is also `WholeShard`-scoped but is not removal-class
/// (`CasRefLedger.cpp` ~:1979). Only a SYNTHETIC item can pin this: the production stale-precommit
/// sweep self-limits its own chunk size to the op cap, so running it proves nothing (spec's own
/// warning). This item drives `MutationScope::wholeShard()` directly with ops that contain NO
/// `RemoveNamespace` op -- if classification were keyed on scope instead of op inspection, this would
/// be wrongly treated as removal-class and admitted; op-inspection correctly rejects it under the
/// ordinary normal-class op-count cap, exactly like `OversizedItemFailsAlone` above.
TEST(CASRefWriterChunkedFlush, SyntheticWholeShardNonRemovalRejected)
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

/// ===================================================================================
/// Task 10 (spec §3 "Chunked flush, where each chunk is a complete commit boundary"): when admitting
/// the next item's ops would exceed `ref_txn_max_ops`, the leader commits the accumulated chunk as a
/// COMPLETE ref-log transaction (real id, PUT, apply, tail, metrics, survivor completion + waiter
/// wakeups, snapshot scheduling), reseeds `working`/the trial-id high-water mark from the now-live
/// state, and continues into a fresh chunk -- so one tenure can emit several transactions, each a valid
/// persisted prefix. The failure-isolation and tenure-containment contracts are pinned below.
/// ===================================================================================

namespace
{

/// The `_log/`-PUT fault seam these tests are built on now lives in `cas_test_helpers.h`, next to
/// `CountingBackend` it derives from: `gtest_cas_ref_install_safety.cpp` needs the SAME seam (spec §A1
/// sites 2 and 3 both turn on what happens when a `_log/` PUT's response is lost), and two copies of a
/// fault backend would drift apart.
using DB::Cas::tests::ChunkFaultBackend;

PoolPtr openPoolWith(const BackendPtr & backend, PoolConfig cfg)
{
    DB::Cas::tests::seedPoolMetaForRestart(*backend);
    cfg.pool_prefix = "p";
    cfg.server_root_id = "test";
    /// Same frozen clock as `openPool` above, and for the same reason — see its comment. Defaulted
    /// rather than forced, so a future test that IS about lease timing can still supply its own.
    if (!cfg.boot_ms_fn)
        cfg.boot_ms_fn = [] { return uint64_t{0}; };
    return Pool::open(backend, cfg);
}

/// `num_pairs` add-then-remove precommit op pairs (2 * `num_pairs` ops total) on ONE ref, each pair
/// naming a distinct valid manifest, so an item scoped `MutationScope::ref(ref)` names exactly the ref
/// its ops mutate (the flush validates that). Every pair adds a precommit binding and immediately
/// removes it, so the LIVE state (the `precommits` set, the committed COW map, the owned-manifest
/// index) stays ~empty throughout the whole transaction -- keeping the per-op `admits` preview and the
/// sanitizer-only body-counter assert O(1), so validating a maximal chunk of thousands of ops stays
/// O(ops), not O(ops^2). It is the OP COUNT (not the resident state) that drives the chunk split under
/// test; each op is tiny (well under `ref_op_max_bytes`), so the whole run is admissible on a `Live`
/// namespace. The durable transaction still carries every op verbatim, so a chunk's ops can be compared
/// against the exact expected vector.
std::vector<RefOp> addRemovePrecommitPairs(const String & ref, size_t num_pairs, uint64_t manifest_epoch)
{
    std::vector<RefOp> ops;
    ops.reserve(num_pairs * 2);
    for (size_t i = 0; i < num_pairs; ++i)
    {
        const ManifestRef manifest{manifest_epoch, i + 1, 1};
        RefOp add;
        add.kind = RefOpKind::OwnerTransition;
        add.new_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref, manifest};
        ops.push_back(std::move(add));
        RefOp remove;
        remove.kind = RefOpKind::OwnerTransition;
        remove.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, ref, manifest};
        ops.push_back(std::move(remove));
    }
    return ops;
}

/// Every durable `_log/` transaction for `ns`, decoded, sorted ascending by transaction id. Undecodable
/// objects (e.g. the foreign bytes a `ForeignConflict` fault lands) are skipped so a corrupt object never
/// breaks the inventory. Reads the backend directly (no Pool cache).
std::vector<RefLogTxn> listLogTxns(DB::Cas::Backend & backend, const DB::Cas::Layout & layout, const RootNamespace & ns)
{
    DB::Cas::tests::OperationForTest operation(backend);
    std::vector<RefTxnId> ids;
    String cursor;
    for (;;)
    {
        const ListPage page = (*operation).list(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)), cursor, 1000, Retry::standard());
        for (const ListedKey & lk : page.keys)
        {
            const auto parsed = layout.parseRefObjectKey(lk.key);
            if (parsed && parsed->life_id == DB::Cas::tests::fixture::fixtureLife(ns).incarnation
                && parsed->kind == RefObjectKind::Log)
                ids.push_back(parsed->txn_id);
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    std::sort(ids.begin(), ids.end(), [](const RefTxnId & a, const RefTxnId & b) { return a < b; });
    std::vector<RefLogTxn> txns;
    for (const RefTxnId & id : ids)
    {
        const auto got = (*operation).read(layout.refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), id), Retry::standard());
        if (!got)
            continue;
        try
        {
            txns.push_back(decodeRefLogTxn(openObject(FormatId::RefLog, got->bytes), ns.string(), id));
        }
        catch (...) // NOLINT(bugprone-empty-catch): best-effort helper -- an undecodable txn is simply skipped, not asserted on
        {
        }
    }
    return txns;
}

/// One queued append driven on its own thread, capturing BOTH the committed transaction id (on success)
/// and the exception (on failure); `build_calls` (when non-null) counts `build_ops` invocations to pin
/// the at-most-once contract across chunk boundaries. The ops are precomputed and returned verbatim, so a
/// second invocation (a bug) is caught by the counter, not masked by a state-dependent rebuild.
struct AppendResult
{
    std::exception_ptr err;
    RefTxnId id{};
};

struct AppendCaller
{
    std::thread t;
    std::future<AppendResult> fut;

    /// Same hazard as `Caller` above (see its destructor comment): a fatal `ASSERT_*` before the
    /// explicit join leaves `t` joinable, and a joinable thread's destructor calls `std::terminate`.
    ~AppendCaller() { if (t.joinable()) t.join(); }
};

AppendCaller launchAppendOps(const PoolPtr & store, const RootNamespace & ns, MutationScope scope,
                             std::vector<RefOp> ops, std::shared_ptr<std::atomic<int>> build_calls)
{
    auto prom = std::make_shared<std::promise<AppendResult>>();
    std::future<AppendResult> fut = prom->get_future();
    auto build_ops = [captured_ops = std::move(ops), build_calls](const RefTableState &) -> std::vector<RefOp>
    {
        if (build_calls)
            build_calls->fetch_add(1);
        return captured_ops;
    };
    std::thread t([store, ns, scope, build_ops, prom]
    {
        AppendResult r;
        try { r.id = store->appendRefOps(ns, scope, build_ops, RootMutationOrigin::Writer, RootMutationKind::Publish); }
        catch (...) { r.err = std::current_exception(); }
        prom->set_value(r);
    });
    return AppendCaller{std::move(t), std::move(fut)};
}

}

/// Test 9 (happy path): a carve whose total ops exceed `ref_txn_max_ops` emits >= 2 ref-log transactions
/// in ONE leader tenure. Three items (2000 ops each = 6000 > 5000) split into chunk 1 = {item_a,item_b}
/// (4000 ops, one id) and chunk 2 = {item_c} (2000 ops, the next id). Per-chunk assertions: committed
/// ids (co-chunk survivors share one real id; the next chunk allocates the next), tail counters (one per
/// chunk), per-chunk metrics (`CASRefBatchFlushes` once per chunk, `CASRefBatchedMutations` counting
/// survivors per chunk), follower wakeups (both followers return their correct real id -> completed +
/// woken at their chunk's commit), `build_ops` at-most-once (invocation counters == 1), and folded state
/// == the sequential result (the two durable transactions carry exactly item_a++item_b, then item_c).
TEST(CASRefWriterChunkedFlush, ChunkedFlushCommitsPerChunk)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// Default thresholds: this handful of transactions never crosses the snapshot-publish threshold, so
    /// no background publish interleaves and the tail/metric deltas below are exact.
    auto store = openPool(backend);
    const DB::Cas::Layout & layout = store->layout();
    const RootNamespace ns{"srv1/chunked_commits_per_chunk"};
    publishEmptyPart(store, ns, "seed");
    ASSERT_TRUE(store->resolveRef(ns, "seed").has_value());

    /// 2000 ops per item (1000 add/remove pairs) -> 6000 > ref_txn_max_ops (5000): chunk 1 =
    /// {item_a,item_b} (4000), chunk 2 = {item_c} (2000).
    const std::vector<RefOp> ops1 = addRemovePrecommitPairs("item_a", 1000, 900000001);
    const std::vector<RefOp> ops2 = addRemovePrecommitPairs("item_b", 1000, 900000002);
    const std::vector<RefOp> ops3 = addRemovePrecommitPairs("item_c", 1000, 900000003);
    auto c1 = std::make_shared<std::atomic<int>>(0);
    auto c2 = std::make_shared<std::atomic<int>>(0);
    auto c3 = std::make_shared<std::atomic<int>>(0);

    const size_t tail_before = store->tailSinceSnapshotCountForTest(ns);
    const uint64_t flushes_before = ProfileEvents::global_counters[ProfileEvents::CASRefBatchFlushes];
    const uint64_t mutations_before = ProfileEvents::global_counters[ProfileEvents::CASRefBatchedMutations];

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 3);
    /// Serialise the enqueue order so the batch is exactly [item_a(leader), item_b, item_c].
    AppendCaller a = launchAppendOps(store, ns, MutationScope::ref("item_a"), ops1, c1);
    waitEntered(sync);
    AppendCaller b = launchAppendOps(store, ns, MutationScope::ref("item_b"), ops2, c2);
    waitPendingAtLeast(store, ns, 2);
    AppendCaller c = launchAppendOps(store, ns, MutationScope::ref("item_c"), ops3, c3);
    waitPendingAtLeast(store, ns, 3);
    sync->cv.notify_all();

    ASSERT_EQ(a.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "item_a must not hang";
    ASSERT_EQ(b.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "item_b must not hang";
    ASSERT_EQ(c.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "item_c must not hang";
    const AppendResult ra = a.fut.get();
    const AppendResult rb = b.fut.get();
    const AppendResult rc = c.fut.get();
    a.t.join();
    b.t.join();
    c.t.join();
    store->setRefPreCarveHookForTest(nullptr);

    ASSERT_TRUE(ra.err == nullptr) << "item_a must commit";
    ASSERT_TRUE(rb.err == nullptr) << "item_b must commit";
    ASSERT_TRUE(rc.err == nullptr) << "item_c must commit";

    /// `build_ops` ran exactly once per item -- including item_c, the overflowing item validated once in
    /// the fresh chunk it lands in.
    EXPECT_EQ(c1->load(), 1);
    EXPECT_EQ(c2->load(), 1);
    EXPECT_EQ(c3->load(), 1);

    /// Committed ids per chunk: item_a and item_b share chunk 1's real id (co-chunk survivors, both
    /// woken with it); item_c gets chunk 2's id, exactly one sequence step above chunk 1.
    EXPECT_EQ(ra.id, rb.id) << "co-chunk survivors must complete with the SAME real transaction id";
    EXPECT_EQ(rc.id.writer_epoch, ra.id.writer_epoch);
    EXPECT_EQ(rc.id.ref_sequence, ra.id.ref_sequence + 1) << "chunk 2 must allocate the id after chunk 1";

    /// >= 2 durable transactions in the tenure, and the split is exactly the sequential result: chunk 1
    /// carries item_a's then item_b's ops (survivor order), chunk 2 carries item_c's.
    const std::vector<RefLogTxn> logs = listLogTxns(*backend, layout, ns);
    std::optional<RefLogTxn> chunk1_txn;
    std::optional<RefLogTxn> chunk2_txn;
    for (const RefLogTxn & txn : logs)
    {
        if (txn.txn_id == ra.id)
            chunk1_txn = txn;
        if (txn.txn_id == rc.id)
            chunk2_txn = txn;
    }
    ASSERT_TRUE(chunk1_txn.has_value()) << "chunk 1 must be durable";
    ASSERT_TRUE(chunk2_txn.has_value()) << "chunk 2 must be durable (a second transaction in one tenure)";
    std::vector<RefOp> expect_chunk1 = ops1;
    expect_chunk1.insert(expect_chunk1.end(), ops2.begin(), ops2.end());
    EXPECT_EQ(chunk1_txn->ops, expect_chunk1);
    EXPECT_EQ(chunk2_txn->ops, ops3);

    /// Tail counters advanced once per committed chunk.
    EXPECT_EQ(store->tailSinceSnapshotCountForTest(ns), tail_before + 2);

    /// Per-chunk metrics: one batch-flush per chunk (2), survivors counted per chunk (2 + 1 = 3). The
    /// snapshot-scheduling trigger is the final step of the SAME committed arm that increments
    /// `CASRefBatchFlushes`, so == 2 also proves the scheduler was invoked per chunk;
    /// `SnapshotPublisherLatchedAcrossChunks` proves that trigger actually re-fires across chunks.
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefBatchFlushes] - flushes_before, 2u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefBatchedMutations] - mutations_before, 3u);
}

namespace
{

/// Shared body for the three chunk-failure variants: two items (3000 ops each) -> chunk 1 = {item_a}
/// (the leader's own item), chunk 2 = {item_b}. `mode` faults ONLY chunk 2's `_log/` PUT (skip chunk 1).
/// In every variant chunk 1 commits and the leader's own call returns chunk 1's real id, while chunk 2's
/// caller fails. Returns the two callers' results plus chunk 1's id for the per-variant assertions.
/// The engine reissues an unresolved write until its OWN retry window closes, and that window is
/// measured on a clock the engine reads. Both seams here share one counter -- the sleep the engine
/// performs is what advances the clock -- so a fault that stays armed ends the call at its deadline
/// with no real time passing. Installed on the whole pool, because the ref-lane write, its settling
/// read and the recovery retry loop all pace through the same seam. The pool owns the closures and the
/// closures own the clock, so it outlives everything that can still read it.
class VirtualRetryClock
{
public:
    static std::shared_ptr<VirtualRetryClock> installOn(const PoolPtr & store)
    {
        auto clock = std::make_shared<VirtualRetryClock>();
        store->setCasRequestNowFnForTest([clock] { return clock->nowMs(); });
        store->setCasRetrySleepForTest([clock](uint64_t ms) { clock->advance(ms); });
        return clock;
    }

    uint64_t nowMs() const
    {
        std::lock_guard lock(mutex);
        return now_ms;
    }
    size_t pauseCount() const
    {
        std::lock_guard lock(mutex);
        return pauses;
    }
    uint64_t longestPause() const
    {
        std::lock_guard lock(mutex);
        return longest_pause;
    }

    void advance(uint64_t ms)
    {
        std::lock_guard lock(mutex);
        /// Plus one millisecond, because full jitter can draw a ZERO pause: a clock that does not move
        /// would leave the loop reissuing for ever against a fault that never clears.
        now_ms += ms + 1;
        ++pauses;
        longest_pause = std::max(longest_pause, ms);
    }

private:
    mutable std::mutex mutex;
    uint64_t now_ms = 0;
    size_t pauses = 0;
    uint64_t longest_pause = 0;
};

/// `ChunkFaultBackend` COUNTS its faults, and a count can no longer make one conclusive: the write
/// engine settles every ambiguity by an exact read and then REISSUES, so a fault that runs out
/// mid-call is answered by the next attempt instead of by the call's own deadline. This keeps it armed
/// until the latch is cleared, on both legs -- the write's, and the lost read `Mode::LandedThenLost`
/// arms, which the read engine would otherwise simply reissue past.
class LatchedChunkFaultBackend : public ChunkFaultBackend
{
public:
    bool latched = false;

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (latched && !fail_read_once_key.empty() && key == fail_read_once_key)
            throw Poco::TimeoutException("LatchedChunkFaultBackend: the lost read stays lost");
        return ChunkFaultBackend::read(key, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             DB::Cas::TransportAccess & access) override
    {
        if (latched && mode != Mode::None && fault_skip == 0 && !expected_value && !fault_substr.empty()
            && key.find(fault_substr) != String::npos)
            fault_count = 1;
        return ChunkFaultBackend::write(key, bytes, expected_value, access);
    }

    void disarm()
    {
        latched = false;
        mode = Mode::None;
        fault_count = 0;
        fault_skip = 0;
        fail_read_once_key.clear();
    }
};

struct ChunkFailureOutcome
{
    AppendResult leader;     /// item_a, chunk 1
    AppendResult follower;   /// item_b, chunk 2
    RefTxnId chunk1_id{};
    std::shared_ptr<LatchedChunkFaultBackend> backend;
    PoolPtr store;
    std::shared_ptr<VirtualRetryClock> clock;
};

ChunkFailureOutcome runChunkFailureCase(const String & ns_suffix, ChunkFaultBackend::Mode mode)
{
    auto backend = std::make_shared<LatchedChunkFaultBackend>();
    PoolConfig cfg;
    /// The budget bounds the mount lease's own admission arithmetic and nothing else -- a write's
    /// attempt count is the `Retry` policy's. What makes the injected fault conclusive is that it
    /// stays armed for the whole call while the injected clock carries the call to its own deadline.
    CasRequestBudget budget;
    budget.attempt_timeout_ms = 100;
    budget.lease_safety_margin_ms = 100;
    cfg.cas_request_budget = budget;
    /// What the request engine reserves per attempt is the BACKEND's attempt timeout, not the budget
    /// field alone; pair the two so the mount lease's admission arithmetic sees what the budget claims.
    backend->setAttemptTimeoutMs(budget.attempt_timeout_ms);
    auto store = openPoolWith(backend, cfg);
    auto clock = VirtualRetryClock::installOn(store);
    const DB::Cas::Layout & layout = store->layout();
    const RootNamespace ns{String("srv1/") + ns_suffix};
    publishEmptyPart(store, ns, "seed");

    /// Fault ONLY chunk 2's `_log/` PUT: skip chunk 1's (the first match), fault the second. Armed AFTER
    /// the seed so only the flush's two log PUTs are counted.
    backend->fault_substr = layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_log/";
    backend->mode = mode;
    backend->fault_skip = 1;
    backend->fault_count = 1;
    backend->latched = true;

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);
    /// 3000 ops per item (1500 add/remove pairs) -> 6000 > ref_txn_max_ops: chunk 1 = {item_a},
    /// chunk 2 = {item_b}.
    AppendCaller a = launchAppendOps(store, ns, MutationScope::ref("item_a"), addRemovePrecommitPairs("item_a", 1500, 900000001), nullptr);
    waitEntered(sync);
    AppendCaller b = launchAppendOps(store, ns, MutationScope::ref("item_b"), addRemovePrecommitPairs("item_b", 1500, 900000002), nullptr);
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();

    EXPECT_EQ(a.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "leader must not hang";
    EXPECT_EQ(b.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "follower must not hang";
    ChunkFailureOutcome out;
    out.leader = a.fut.get();
    out.follower = b.fut.get();
    a.t.join();
    b.t.join();
    /// Disarmed before anything else touches the store: a topped-up count would fault the pool's own
    /// teardown writes too.
    backend->disarm();
    store->setRefPreCarveHookForTest(nullptr);
    out.chunk1_id = out.leader.id;
    out.backend = backend;
    out.store = store;
    out.clock = clock;
    return out;
}

}

/// Test 9 (chunk-failure variant a -- definite failure): chunk 2's create is conclusively rejected by
/// the store. Chunk 1's caller (the leader's own item) observes SUCCESS with chunk 1's real id; chunk
/// 2's caller fails; the lane does NOT wedge (a proven refusal is a safe gap, not an uncertain
/// write).
TEST(CASRefWriterChunkedFlush, ChunkFailureDefinite)
{
#if !USE_AWS_S3
    GTEST_SKIP() << "DefiniteFailure classification requires S3 error types (USE_AWS_S3 off)";
#endif
    ChunkFailureOutcome out = runChunkFailureCase("chunk_fail_definite", ChunkFaultBackend::Mode::Definite);
    ASSERT_TRUE(out.leader.err == nullptr) << "chunk-1 caller must observe success even though chunk 2 failed";
    ASSERT_TRUE(out.follower.err != nullptr) << "chunk-2 caller must observe the definite failure";
    EXPECT_FALSE(out.store->refLaneWedgedForTest(RootNamespace{"srv1/chunk_fail_definite"}))
        << "a definite failure is proven non-durable and must NOT wedge the lane";

    const auto logs = listLogTxns(*out.backend, out.store->layout(), RootNamespace{"srv1/chunk_fail_definite"});
    bool saw_chunk1 = false;
    for (const RefLogTxn & txn : logs)
        if (txn.txn_id == out.chunk1_id)
            saw_chunk1 = true;
    EXPECT_TRUE(saw_chunk1) << "chunk 1 must be durably committed";
}

/// Test 9 (chunk-failure variant b -- unresolved wedge): chunk 2's PUT is ambiguous and exhausts the
/// budget, wedging the lane. Chunk 1's caller observes SUCCESS; chunk 2's caller fails; the wedge holds
/// ONLY chunk 2's key (chunk 1 + 1), and chunk 1's object is durable while chunk 2's was never written.
TEST(CASRefWriterChunkedFlush, ChunkFailureWedge)
{
    const RootNamespace ns{"srv1/chunk_fail_wedge"};
    ChunkFailureOutcome out = runChunkFailureCase("chunk_fail_wedge", ChunkFaultBackend::Mode::Unresolved);
    ASSERT_TRUE(out.leader.err == nullptr) << "chunk-1 caller must observe success even though chunk 2 wedged";
    ASSERT_TRUE(out.follower.err != nullptr) << "chunk-2 caller must observe the append failure";
    /// The give-up was chunk 2's OWN retry window: the fault outlasted several reissues, and every one
    /// of them paced through the injected sleep rather than a real one.
    EXPECT_GT(out.clock->pauseCount(), 1u);
    EXPECT_LE(out.clock->longestPause(), 5000u) << "each pause is the engine's own capped full jitter";
    EXPECT_GE(out.clock->nowMs(), 60000u);

    EXPECT_TRUE(out.store->refLaneWedgedForTest(ns)) << "chunk 2's unresolved PUT must wedge the lane";
    RefTxnId chunk2_id = out.chunk1_id;
    ++chunk2_id.ref_sequence;
    EXPECT_EQ(out.store->wedgedKeyForTest(ns), out.store->layout().refLogKey(DB::Cas::tests::fixture::fixtureLife(ns), chunk2_id))
        << "the wedge must contain ONLY chunk 2's key";

    const auto logs = listLogTxns(*out.backend, out.store->layout(), ns);
    bool saw_chunk1 = false;
    bool saw_chunk2 = false;
    for (const RefLogTxn & txn : logs)
    {
        if (txn.txn_id == out.chunk1_id)
            saw_chunk1 = true;
        if (txn.txn_id == chunk2_id)
            saw_chunk2 = true;
    }
    EXPECT_TRUE(saw_chunk1) << "chunk 1 must be durably committed";
    EXPECT_FALSE(saw_chunk2) << "chunk 2's wedged object was never durably written";
}

/// Test 9 (chunk-failure variant c -- a throw): chunk 2's PUT surfaces a proven conflict (CORRUPTED_DATA
/// thrown by the controller). Chunk 1's caller observes SUCCESS; chunk 2's caller fails with
/// CORRUPTED_DATA; the lane does NOT wedge (a conclusive rejection).
TEST(CASRefWriterChunkedFlush, ChunkFailureThrow)
{
    const RootNamespace ns{"srv1/chunk_fail_throw"};
    ChunkFailureOutcome out = runChunkFailureCase("chunk_fail_throw", ChunkFaultBackend::Mode::ForeignConflict);
    ASSERT_TRUE(out.leader.err == nullptr) << "chunk-1 caller must observe success even though chunk 2 threw";
    ASSERT_TRUE(out.follower.err != nullptr) << "chunk-2 caller must observe the thrown failure";
    expectFailedWithCode(out.follower.err, DB::ErrorCodes::CORRUPTED_DATA, "chunk-2 proven-conflict throw");
    EXPECT_FALSE(out.store->refLaneWedgedForTest(ns)) << "a proven conflict is conclusive and must NOT wedge";

    const auto logs = listLogTxns(*out.backend, out.store->layout(), ns);
    bool saw_chunk1 = false;
    for (const RefLogTxn & txn : logs)
        if (txn.txn_id == out.chunk1_id)
            saw_chunk1 = true;
    EXPECT_TRUE(saw_chunk1) << "chunk 1 must be durably committed";
}

/// Test 9 (containment variant 1): the leader's OWN item lands in chunk 1; a throw is injected at the
/// chunk boundary (simulating a reseed allocation failure) AFTER chunk 1 committed. Tenure containment
/// (spec §3): the leader's own `appendRefOps` returns chunk 1's real id -- NOT the later exception --
/// while the unattempted remainder (item_b) fails. This exercises the reworked outer catch, which no
/// longer rethrows unconditionally over a durable own item.
TEST(CASRefWriterChunkedFlush, LeaderOwnItemCommittedBeforeThrow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    const DB::Cas::Layout & layout = store->layout();
    const RootNamespace ns{"srv1/chunk_leader_own_committed"};
    publishEmptyPart(store, ns, "seed");

    auto c1 = std::make_shared<std::atomic<int>>(0);
    auto c2 = std::make_shared<std::atomic<int>>(0);

    /// Throw once at the first chunk boundary -- after chunk 1 (the leader's own item) is durable and
    /// before the reseed completes.
    auto boundary_hits = std::make_shared<std::atomic<int>>(0);
    store->setCarveHookForTest([boundary_hits](CasRefLedger::CarvePhaseForTest ph)
    {
        if (ph == CasRefLedger::CarvePhaseForTest::ChunkReseed && boundary_hits->fetch_add(1) == 0)
            throw std::bad_alloc{};
    });

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);
    /// 3000 ops per item (1500 add/remove pairs) -> chunk 1 = {item_a}, boundary throw before chunk 2.
    AppendCaller a = launchAppendOps(store, ns, MutationScope::ref("item_a"), addRemovePrecommitPairs("item_a", 1500, 900000001), c1);
    waitEntered(sync);
    AppendCaller b = launchAppendOps(store, ns, MutationScope::ref("item_b"), addRemovePrecommitPairs("item_b", 1500, 900000002), c2);
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();

    ASSERT_EQ(a.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "leader must not hang";
    ASSERT_EQ(b.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "follower must not hang";
    const AppendResult ra = a.fut.get();
    const AppendResult rb = b.fut.get();
    a.t.join();
    b.t.join();
    store->setCarveHookForTest(nullptr);
    store->setRefPreCarveHookForTest(nullptr);

    ASSERT_TRUE(ra.err == nullptr)
        << "the leader's own committed-chunk item must return success, not the later boundary throw";
    ASSERT_TRUE(rb.err != nullptr) << "the unattempted remainder must fail";

    const std::vector<RefLogTxn> logs = listLogTxns(*backend, layout, ns);
    std::optional<RefLogTxn> chunk1_txn;
    for (const RefLogTxn & txn : logs)
        if (txn.txn_id == ra.id)
            chunk1_txn = txn;
    ASSERT_TRUE(chunk1_txn.has_value()) << "chunk 1 must be durable";
    EXPECT_EQ(chunk1_txn->ops, addRemovePrecommitPairs("item_a", 1500, 900000001));
    /// item_a's build_ops ran once (chunk 1); item_b's ran once (before the boundary throw preempted its
    /// validation) and is NOT re-invoked -- the at-most-once contract holds through the failed tenure.
    EXPECT_EQ(c1->load(), 1);
    EXPECT_EQ(c2->load(), 1);
}

/// Test 9 (containment variant 2 -- snapshot coalescing): a snapshot publisher dispatched by chunk 1 is
/// latched at its PUT AFTER capturing chunk 1's prefix; chunk 2 then commits and its publish trigger is
/// discarded by the single-in-flight gate. When the latched publisher settles, settlement must re-fire
/// the dropped trigger so a FOLLOW-UP publication covers chunk 2 -- otherwise chunk 2 would stay
/// unsnapshotted until an unrelated later mutation. The chunk boundary is gated until the publisher has
/// parked, so its captured candidate is provably chunk 1's prefix only.
TEST(CASRefWriterChunkedFlush, SnapshotPublisherLatchedAcrossChunks)
{
    auto backend = std::make_shared<ChunkFaultBackend>();
    PoolConfig cfg;
    cfg.snapshot_log_count_threshold = 0;   /// every committed chunk crosses the tail-count threshold
    /// A setup publisher that meets a `Writing` lane gives up; a backoff armed on the frozen clock would
    /// then delay the parked `_snap/` PUT this test waits for.
    cfg.snapshot_publish_backoff_initial_ms = 0;
    auto store = openPoolWith(backend, cfg);
    const DB::Cas::Layout & layout = store->layout();
    const RootNamespace ns{"srv1/chunk_snapshot_coalesce"};
    /// NOT `publishEmptyPart`: that helper makes `precommitAdd` (which folds an implicit
    /// `namespace_birth` and the add into ONE transaction, since the namespace is not yet `Live`) and
    /// `promote` two separate, back-to-back `appendRefOps` calls. `precommitAdd`'s own post-commit
    /// trigger (`maybeScheduleSnapshotPublish` at the end of `commitRefChunk`) dispatches a background
    /// publisher for what it just committed; with `snapshot_log_count_threshold` at 0 (every single
    /// commit is eligible -- never reachable through a normal, 256-count threshold) that publisher can
    /// still be in flight when `promote`, moments later, becomes lane leader for its OWN write and
    /// moves the lane to `Writing` -- a real, reachable race between that capture and this transition.
    /// A lost race backs the publisher off, and this pool's frozen `boot_ms_fn` (see `openPool`'s
    /// comment) never advances past that deadline, so the backoff never clears on its own, poisoning
    /// every later dispatch on `ns` for the rest of the test, including chunk 1's. Draining
    /// (`waitForSnapshotPublishSettleForTest`) between the two commits removes the in-flight publisher
    /// `promote` would otherwise race, and driving one explicitly
    /// (`tryPublishSnapshotAndAdvanceCheckpointOnce`, the direct synchronous seam built for exactly this
    /// -- "public so tests can drive one attempt deterministically without depending on the background
    /// dispatch's timing") covers anything a dispatch was never even admitted for.
    DB::Cas::tests::casAdmitRecoverableEntry(*store->poolBackendPtr(), store->layout(), ns, store->liveWriterEpoch());
    PartWriteInfo seed_info;
    seed_info.intended_namespace = ns;
    seed_info.intended_ref = ns.string() + "/seed";
    auto seed_build = store->beginPartWrite(seed_info);
    const ManifestId seed_manifest_id = seed_build->stageManifest({});
    seed_build->precommitAdd(ns, "seed", seed_manifest_id);
    store->waitForSnapshotPublishSettleForTest(ns);
    store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns);
    seed_build->promote(ns, "seed", seed_build->buildId(), seed_manifest_id);
    store->waitForSnapshotPublishSettleForTest(ns);   /// drain the seed's publish chain -> tail == 0
    store->tryPublishSnapshotAndAdvanceCheckpointOnce(ns);
    ASSERT_EQ(store->tailSinceSnapshotCountForTest(ns), 0u) << "the setup must end with a compacted tail";

    /// Latch the FIRST `_snap/` PUT (chunk 1's publisher) at its conditional PUT -- i.e. AFTER it has
    /// captured chunk 1's prefix under state_mutex.
    backend->armBlock(layout.namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_snap/");
    /// Gate the leader at the chunk boundary on the publisher's CAPTURE (`snapshot_after_capture_hook_for_test`,
    /// fired the moment the publish attempt has read `rt->state` under `state_mutex` and passed the
    /// `lane_state == Ready` admission check), not on the publisher's later blocked PUT. Capture is
    /// causally prior to the PUT the block intercepts -- it is the OTHER side of the very check
    /// (`CasRefLedger.cpp`'s `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntimeImpl`) whose failure logs
    /// "refusing snapshot publication while the append lane is not Ready" -- so waiting for it is the
    /// exact fact the boundary needs, whereas waiting for the PUT also waits on however long it takes the
    /// dispatched publish to be scheduled onto a worker thread at all. Under contention that scheduling
    /// delay can outlast a bounded wait, and a leader released by the wait's own timeout (rather than by
    /// the capture it was meant to prove) can start chunk 2 -- moving the lane to `Writing` -- before the
    /// not-yet-scheduled publisher ever captures, so it captures Writing and is refused.
    auto captured = std::make_shared<CaseSync>();
    store->setSnapshotAfterCaptureHookForTest([captured]
    {
        std::lock_guard lk(captured->m);
        captured->entered = true;
        captured->cv.notify_all();
    });
    store->setCarveHookForTest([captured](CasRefLedger::CarvePhaseForTest ph)
    {
        if (ph != CasRefLedger::CarvePhaseForTest::ChunkReseed)
            return;
        std::unique_lock lk(captured->m);
        captured->cv.wait_for(lk, std::chrono::seconds(10), [&] { return captured->entered; });
        ASSERT_TRUE(captured->entered) << "chunk 1's snapshot publisher never captured its candidate within 10s";
    });

    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(store, ns, sync, 2);
    /// 3000 ops per item (1500 add/remove pairs) -> chunk 1 = {item_a}, chunk 2 = {item_b}.
    AppendCaller a = launchAppendOps(store, ns, MutationScope::ref("item_a"), addRemovePrecommitPairs("item_a", 1500, 900000001), nullptr);
    waitEntered(sync);
    AppendCaller b = launchAppendOps(store, ns, MutationScope::ref("item_b"), addRemovePrecommitPairs("item_b", 1500, 900000002), nullptr);
    waitPendingAtLeast(store, ns, 2);
    sync->cv.notify_all();

    ASSERT_EQ(a.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "leader must not hang";
    ASSERT_EQ(b.fut.wait_for(std::chrono::seconds(20)), std::future_status::ready) << "follower must not hang";
    const AppendResult ra = a.fut.get();
    const AppendResult rb = b.fut.get();
    a.t.join();
    b.t.join();
    ASSERT_TRUE(ra.err == nullptr) << "chunk 1 must commit";
    ASSERT_TRUE(rb.err == nullptr) << "chunk 2 must commit";
    RefTxnId chunk2_id = ra.id;
    ++chunk2_id.ref_sequence;
    EXPECT_EQ(rb.id, chunk2_id);

    /// Independently confirm the latched publisher actually reached its blocked PUT, on the main test
    /// thread rather than as the leader's own gate: without this, a wiring regression that never parks
    /// the publisher would let the settlement assertion below pass VACUOUSLY (a direct, non-coalesced
    /// dispatch can still cover chunk 2).
    backend->awaitBlockEntered();

    /// Release the latched chunk-1 publisher. Its settlement must re-fire the chunk-2 trigger the
    /// single-flight gate dropped -> a follow-up publication covers chunk 2.
    backend->releaseBlock();
    store->waitForSnapshotPublishSettleForTest(ns);
    store->setCarveHookForTest(nullptr);
    store->setSnapshotAfterCaptureHookForTest(nullptr);
    store->setRefPreCarveHookForTest(nullptr);

    const std::optional<RefTxnId> newest = store->newestPublishedSnapshotIdForTest(ns);
    ASSERT_TRUE(newest.has_value()) << "at least one snapshot must have been published";
    EXPECT_FALSE(*newest < chunk2_id)
        << "settlement must re-fire the dropped chunk-2 trigger so a snapshot covers chunk 2 (no lost trigger)";
}

/// ===================================================================================
/// A leader flushes in its caller's thread group. A killed query does not stop the shared flush, and a stopped
/// background owner can still withdraw the leader's own item until the carve.
/// ===================================================================================

namespace
{

using DB::Cas::tests::CasClientLikeBackend;
using DB::Cas::tests::CasKillableQuery;
using DB::Cas::tests::StoppedBy;
using DB::Cas::tests::backgroundOwnerStoppedBy;
using DB::Cas::tests::nameOf;
using DB::Cas::tests::stopperOf;

constexpr auto KILL_WATCHDOG = std::chrono::seconds(30);

/// Calls `before_answer` before the first ref-log create under `log_prefix` after `armed` is set, then stores it.
class LeaderKillLogPutBackend final : public DB::Cas::tests::CountingBackend
{
public:
    String log_prefix;
    std::function<void()> before_answer;
    std::atomic<bool> armed{false};
    std::atomic<bool> fired{false};

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value,
                                             DB::Cas::TransportAccess & access) override
    {
        if (!expected_value && key.starts_with(log_prefix) && armed.exchange(false))
        {
            fired = true;
            if (before_answer)
                before_answer();
        }
        return CountingBackend::write(key, bytes, expected_value, access);
    }
};

struct KilledLeaderFixture
{
    std::shared_ptr<LeaderKillLogPutBackend> faults = std::make_shared<LeaderKillLogPutBackend>();
    PoolPtr store;
    std::shared_ptr<VirtualRetryClock> clock;
    RootNamespace ns;

    explicit KilledLeaderFixture(const String & ns_suffix)
        : ns(String("srv1/") + ns_suffix)
    {
        store = openPoolWith(std::make_shared<CasClientLikeBackend>(faults), PoolConfig{});
        clock = VirtualRetryClock::installOn(store);
        publishEmptyPart(store, ns, "seed");
        faults->log_prefix = store->layout().namespaceStreamPrefix(DB::Cas::tests::fixture::fixtureLife(ns)) + "_log/";
    }
};

int exceptionCode(const std::exception_ptr & e)
{
    if (!e)
        return 0;
    try
    {
        std::rethrow_exception(e);
    }
    catch (const DB::Exception & ex)
    {
        return ex.code();
    }
    catch (...)
    {
        return -1;
    }
}

/// The leader's side of a killed-leader case, run on a raw thread with its own `ThreadStatus` and a
/// query registered in the process list. `body` gets the query and returns the append's result.
struct LeaderRun
{
    AppendResult result;
    bool group_restored = false;
};

template <typename Body>
std::future<LeaderRun> launchQueryLeader(std::thread & thread, Body leader_body)
{
    auto prom = std::make_shared<std::promise<LeaderRun>>();
    std::future<LeaderRun> fut = prom->get_future();
    thread = std::thread([prom, body = std::move(leader_body)]() mutable
    {
        std::optional<DB::ThreadStatus> thread_status;
        thread_status.emplace();
        LeaderRun run;
        {
            CasKillableQuery query(getContext().context);
            run.result = body(query);
            run.group_restored = DB::CurrentThread::getGroup() == query.group;
        }
        prom->set_value(run);
    });
    return fut;
}

/// The leader's item and a neighbor's share one chunk; the leader's query is killed between the carve
/// and the chunk's first answer.
struct KilledLeaderOutcome
{
    LeaderRun leader;
    AppendResult neighbor;
    int neighbor_builds = 0;
};

KilledLeaderOutcome runKilledLeaderChunk(KilledLeaderFixture & fx)
{
    auto sync = std::make_shared<CaseSync>();
    armPreCarveBlock(fx.store, fx.ns, sync, 2);
    auto neighbor_builds = std::make_shared<std::atomic<int>>(0);

    std::thread leader_thread;
    std::future<LeaderRun> leader = launchQueryLeader(leader_thread, [&fx](CasKillableQuery & query)
    {
        fx.faults->before_answer = [&query] { query.kill(); };
        fx.faults->armed = true;
        AppendResult r;
        try
        {
            r.id = fx.store->appendRefOps(fx.ns, MutationScope::ref("item_leader"),
                [](const RefTableState &) { return addRemovePrecommitPairs("item_leader", 1, 910000001); },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
        }
        catch (...)
        {
            r.err = std::current_exception();
        }
        return r;
    });
    waitEntered(sync);
    AppendCaller neighbor = launchAppendOps(fx.store, fx.ns, MutationScope::ref("item_neighbor"),
        addRemovePrecommitPairs("item_neighbor", 1, 910000002), neighbor_builds);
    waitPendingAtLeast(fx.store, fx.ns, 2);
    sync->cv.notify_all();

    KilledLeaderOutcome out;
    EXPECT_EQ(leader.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the killed leader must not hang";
    EXPECT_EQ(neighbor.fut.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the neighbor must not hang";
    out.leader = leader.get();
    out.neighbor = neighbor.fut.get();
    leader_thread.join();
    neighbor.t.join();
    fx.faults->before_answer = {};
    fx.store->setRefPreCarveHookForTest(nullptr);
    out.neighbor_builds = neighbor_builds->load();
    return out;
}

}

/// A leader's query killed after the carve, on a store that answers every request successfully: the kill does not
/// stop the shared flush, so the chunk commits once for the leader's item and the neighbor's.
TEST(CASRefWriterChunkedFlush, KilledLeaderOnHealthyStoreCommitsWithNeighbor)
{
    KilledLeaderFixture fx("killed_leader_healthy_store");
    const uint64_t wedged_before = ProfileEvents::global_counters[ProfileEvents::CASRefAppendWedged];

    const KilledLeaderOutcome out = runKilledLeaderChunk(fx);

    ASSERT_TRUE(fx.faults->fired) << "the kill never ran before the chunk's create";
    EXPECT_EQ(exceptionCode(out.leader.result.err), 0) << "a killed query's carved item commits";
    EXPECT_EQ(exceptionCode(out.neighbor.err), 0) << "a live neighbor must commit";
    EXPECT_EQ(out.leader.result.id, out.neighbor.id) << "both items were in one chunk";
    EXPECT_EQ(out.neighbor_builds, 1);
    EXPECT_TRUE(out.leader.group_restored);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRefAppendWedged] - wedged_before, 0u);
    EXPECT_FALSE(fx.store->refLaneWedgedForTest(fx.ns));
    const std::vector<RefLogTxn> logs = listLogTxns(*fx.faults, fx.store->layout(), fx.ns);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [&](const RefLogTxn & txn) { return txn.txn_id == out.neighbor.id; }), 1)
        << "the chunk is durable exactly once";
}

/// Leaving a query's thread group finalizes the thread's counters and writes a `system.query_thread_log` row, so a
/// leader must flush in its caller's group: the same group before the append, inside the flush and after it.
TEST(CASRefWriterChunkedFlush, LeaderFlushesInItsCallersThreadGroup)
{
    KilledLeaderFixture fx("leader_keeps_thread_group");
    DB::ThreadGroupPtr group_before;
    DB::ThreadGroupPtr group_inside_flush;
    DB::ThreadGroupPtr group_after;
    bool hook_ran = false;

    std::thread leader_thread;
    std::future<LeaderRun> leader = launchQueryLeader(leader_thread, [&](CasKillableQuery &)
    {
        group_before = DB::CurrentThread::getGroup();
        fx.store->setRefPreCarveHookForTest([&]
        {
            hook_ran = true;
            group_inside_flush = DB::CurrentThread::getGroup();
        });
        AppendResult r;
        try
        {
            r.id = fx.store->appendRefOps(fx.ns, MutationScope::ref("item_leader"),
                [](const RefTableState &) { return addRemovePrecommitPairs("item_leader", 1, 945000001); },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
        }
        catch (...)
        {
            r.err = std::current_exception();
        }
        group_after = DB::CurrentThread::getGroup();
        return r;
    });
    ASSERT_EQ(leader.wait_for(KILL_WATCHDOG), std::future_status::ready);
    const LeaderRun run = leader.get();
    leader_thread.join();
    fx.store->setRefPreCarveHookForTest(nullptr);

    EXPECT_EQ(exceptionCode(run.result.err), 0);
    ASSERT_TRUE(hook_ran) << "the hook did not run inside the flush";
    ASSERT_NE(group_before, nullptr);
    EXPECT_EQ(group_inside_flush, group_before) << "the flush left the caller's thread group";
    EXPECT_EQ(group_after, group_before);
    EXPECT_TRUE(run.group_restored);
}

namespace
{

/// The leader's owner stops during the flush prelude while a live waiter's item is queued behind it.
void runLeaderStoppedDuringFlushPrelude(StoppedBy owner)
{
    KilledLeaderFixture fx("killed_leader_withdraws");
    auto leader_builds = std::make_shared<std::atomic<int>>(0);
    auto neighbor_builds = std::make_shared<std::atomic<int>>(0);
    auto sync = std::make_shared<CaseSync>();
    std::function<void()> stop_leader;

    fx.store->setRefPreCarveHookForTest([&, sync]
    {
        std::unique_lock lk(sync->m);
        if (sync->entered)
            return;
        sync->entered = true;
        sync->cv.notify_all();
        sync->cv.wait_for(lk, KILL_WATCHDOG, [&] { return fx.store->refQueuePendingForTest(fx.ns) >= 2; });
        stop_leader();
    });

    std::thread leader_thread;
    std::future<LeaderRun> leader = launchQueryLeader(leader_thread, [&](CasKillableQuery & query)
    {
        auto background_stopped = std::make_shared<std::atomic<bool>>(false);
        const auto background = backgroundOwnerStoppedBy(background_stopped);
        stop_leader = stopperOf(owner, query, background_stopped);
        AppendResult r;
        try
        {
            r.id = fx.store->appendRefOps(fx.ns, MutationScope::ref("item_leader"),
                [leader_builds](const RefTableState &)
                {
                    leader_builds->fetch_add(1);
                    return addRemovePrecommitPairs("item_leader", 1, 920000001);
                },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
        }
        catch (...)
        {
            r.err = std::current_exception();
        }
        return r;
    });
    waitEntered(sync);
    AppendCaller neighbor = launchAppendOps(fx.store, fx.ns, MutationScope::ref("item_neighbor"),
        addRemovePrecommitPairs("item_neighbor", 1, 920000002), neighbor_builds);
    waitPendingAtLeast(fx.store, fx.ns, 2);
    sync->cv.notify_all();

    ASSERT_EQ(leader.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the stopped leader must not hang";
    ASSERT_EQ(neighbor.fut.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the neighbor must not hang";
    const LeaderRun run = leader.get();
    const AppendResult rn = neighbor.fut.get();
    leader_thread.join();
    neighbor.t.join();
    fx.store->setRefPreCarveHookForTest(nullptr);

    EXPECT_EQ(exceptionCode(rn.err), 0) << "the live waiter commits";
    EXPECT_EQ(neighbor_builds->load(), 1);
    EXPECT_TRUE(run.group_restored);
    EXPECT_EQ(fx.store->refQueuePendingForTest(fx.ns), 0u);
    const std::vector<RefLogTxn> logs = listLogTxns(*fx.faults, fx.store->layout(), fx.ns);
    const auto neighbor_txn = std::find_if(logs.begin(), logs.end(), [&](const RefLogTxn & txn) { return txn.txn_id == rn.id; });
    ASSERT_NE(neighbor_txn, logs.end());
    if (owner == StoppedBy::Background)
    {
        EXPECT_EQ(exceptionCode(run.result.err), DB::ErrorCodes::ABORTED);
        EXPECT_EQ(leader_builds->load(), 0) << "a withdrawn item is never built";
        EXPECT_EQ(neighbor_txn->ops, addRemovePrecommitPairs("item_neighbor", 1, 920000002)) << "the neighbor commits alone";
    }
    else
    {
        EXPECT_EQ(exceptionCode(run.result.err), 0) << "a killed leader's item commits";
        EXPECT_EQ(leader_builds->load(), 1);
        EXPECT_EQ(run.result.id, rn.id) << "both items were carved into one chunk";
    }
}

}

/// A background stop during the flush prelude withdraws the leader's own item at the carve: its `build_ops` never
/// runs, it gets the owner's verdict, and the live waiter's item commits alone.
TEST(CASRefWriterChunkedFlush, BackgroundStoppedLeaderWithdrawsOwnItemAtCarve)
{
    runLeaderStoppedDuringFlushPrelude(StoppedBy::Background);
}

/// A leader killed during the flush prelude keeps its item: it is carved with the waiter's and the chunk commits for both.
TEST(CASRefWriterChunkedFlush, KilledLeadersItemIsCarvedAndCommitted)
{
    runLeaderStoppedDuringFlushPrelude(StoppedBy::Query);
}

namespace
{

/// A leader whose owner stopped before its flush, on a lane with an outstanding wedge.
void runLeaderStoppedBeforeItsFlush(StoppedBy owner)
{
    KilledLeaderFixture fx("killed_leader_before_flush");
    const std::vector<RefLogTxn> logs = listLogTxns(*fx.faults, fx.store->layout(), fx.ns);
    ASSERT_FALSE(logs.empty());
    RefLogTxn wedged;
    wedged.ns = fx.ns.string();
    wedged.txn_id = logs.back().txn_id;
    ++wedged.txn_id.ref_sequence;
    wedged.ops = addRemovePrecommitPairs("item_wedged", 1, 940000002);
    fx.store->forceWedgeForTest(fx.ns, wedged.txn_id.writer_epoch, wedged.txn_id.ref_sequence,
        fx.store->layout().refLogKey(DB::Cas::tests::fixture::fixtureLife(fx.ns), wedged.txn_id),
        sealObject(FormatId::RefLog, encodeRefLogTxn(wedged)));
    auto builds = std::make_shared<std::atomic<int>>(0);
    size_t requests = 0;

    std::thread leader_thread;
    std::future<LeaderRun> leader = launchQueryLeader(leader_thread, [&](CasKillableQuery & query)
    {
        auto background_stopped = std::make_shared<std::atomic<bool>>(false);
        const auto background = backgroundOwnerStoppedBy(background_stopped);
        stopperOf(owner, query, background_stopped)();
        const size_t before = fx.faults->journal().size();
        AppendResult r;
        try
        {
            r.id = fx.store->appendRefOps(fx.ns, MutationScope::ref("item_leader"),
                [builds](const RefTableState &)
                {
                    builds->fetch_add(1);
                    return addRemovePrecommitPairs("item_leader", 1, 940000001);
                },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
        }
        catch (...)
        {
            r.err = std::current_exception();
        }
        requests = fx.faults->journal().size() - before;
        return r;
    });
    ASSERT_EQ(leader.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the stopped leader must not hang";
    const LeaderRun run = leader.get();
    leader_thread.join();

    EXPECT_EQ(fx.store->refQueuePendingForTest(fx.ns), 0u);
    EXPECT_TRUE(run.group_restored);
    if (owner == StoppedBy::Background)
    {
        EXPECT_EQ(exceptionCode(run.result.err), DB::ErrorCodes::ABORTED);
        EXPECT_EQ(requests, 0u) << "a stopped leader starts no flush";
        EXPECT_EQ(builds->load(), 0);
        EXPECT_TRUE(fx.store->refLaneWedgedForTest(fx.ns)) << "the wedge is left for a live leader";
    }
    else
    {
        EXPECT_EQ(exceptionCode(run.result.err), 0) << "a killed query's leader flushes on a healthy store";
        EXPECT_GT(requests, 0u);
        EXPECT_EQ(builds->load(), 1);
        EXPECT_FALSE(fx.store->refLaneWedgedForTest(fx.ns)) << "the leader resolved the wedge";
    }
}

}

/// A leader whose background owner stopped before its flush sends nothing: not even the read that resolves the lane's
/// outstanding wedge. The carve check alone would let that read go out first.
TEST(CASRefWriterChunkedFlush, BackgroundStoppedLeaderSendsNothingBeforeItsFlush)
{
    runLeaderStoppedBeforeItsFlush(StoppedBy::Background);
}

/// A leader whose query was killed before its flush flushes on a healthy store: it resolves the wedge and commits its
/// item.
TEST(CASRefWriterChunkedFlush, KilledLeaderResolvesTheWedgeAndCommits)
{
    runLeaderStoppedBeforeItsFlush(StoppedBy::Query);
}

namespace
{

template <typename Body>
void waitUntil(const char * what, Body ready)
{
    const auto deadline = std::chrono::steady_clock::now() + KILL_WATCHDOG;
    while (!ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    ASSERT_TRUE(ready()) << what;
}

/// A follower on its own query thread. Its `build_ops` runs `on_build` and writes into a local of the follower's
/// own frame, so a follower that returned while its item was carved would leave the leader writing to a dead
/// frame (ASan reports it). `returned` is set when the follower's call has returned.
std::future<LeaderRun> launchQueryFollower(
    std::thread & thread, KilledLeaderFixture & fx, const String & ref, std::atomic<int> & builds, int64_t seed,
    std::function<void()> & kill, std::function<void()> on_build, std::atomic<bool> & returned, StoppedBy owner)
{
    return launchQueryLeader(
        thread, [&fx, ref, &builds, seed, &kill, build_hook = std::move(on_build), &returned, owner](CasKillableQuery & query)
    {
        auto background_stopped = std::make_shared<std::atomic<bool>>(false);
        const auto background = backgroundOwnerStoppedBy(background_stopped);
        kill = stopperOf(owner, query, background_stopped);
        int frame_slot = 0;
        AppendResult r;
        try
        {
            r.id = fx.store->appendRefOps(fx.ns, MutationScope::ref(ref),
                [&builds, &frame_slot, build_hook, ref, seed](const RefTableState &)
                {
                    builds.fetch_add(1);
                    if (build_hook)
                        build_hook();
                    frame_slot = 1;
                    return addRemovePrecommitPairs(ref, 1, seed);
                },
                RootMutationOrigin::Writer, RootMutationKind::Publish);
        }
        catch (...)
        {
            r.err = std::current_exception();
        }
        returned = true;
        return r;
    });
}

}

namespace
{

/// A follower's owner stops while its item is queued behind a parked leader.
void runFollowerStoppedWhileQueued(StoppedBy owner)
{
    KilledLeaderFixture fx("killed_pending_follower");
    auto sync = std::make_shared<CaseSync>();
    bool released = false;
    fx.store->setRefPreCarveHookForTest([sync, &released]
    {
        std::unique_lock lk(sync->m);
        if (sync->entered)
            return;
        sync->entered = true;
        sync->cv.notify_all();
        sync->cv.wait_for(lk, KILL_WATCHDOG, [&] { return released; });
    });
    auto leader_builds = std::make_shared<std::atomic<int>>(0);
    std::atomic<int> follower_builds{0};
    std::atomic<bool> follower_returned{false};
    std::function<void()> stop_follower;
    std::atomic<int> follower_wait_checks{0};
    fx.store->setRefFollowerWaitHookForTest([&](FollowerWaitPhase) { follower_wait_checks.fetch_add(1); });

    AppendCaller leader = launchAppendOps(fx.store, fx.ns, MutationScope::ref("item_leader"),
        addRemovePrecommitPairs("item_leader", 1, 950000001), leader_builds);
    waitEntered(sync);
    std::thread follower_thread;
    std::future<LeaderRun> follower = launchQueryFollower(
        follower_thread, fx, "item_follower", follower_builds, 950000002, stop_follower, {}, follower_returned, owner);
    waitPendingAtLeast(fx.store, fx.ns, 2);
    ASSERT_GE(fx.store->refQueuePendingForTest(fx.ns), 2u);
    const int checks_before_stop = follower_wait_checks.load();
    stop_follower();
    /// The leader stays parked until a stopped background owner's follower has left the queue. A killed query's
    /// follower is given two evaluations of its wait, so its owner was asked after the kill, and must still be queued.
    if (owner == StoppedBy::Background)
    {
        waitUntil("the stopped follower never withdrew its item", [&] { return fx.store->refQueuePendingForTest(fx.ns) <= 1; });
    }
    else
    {
        waitUntil("the follower's wait was not re-evaluated", [&] { return follower_wait_checks.load() >= checks_before_stop + 2; });
        EXPECT_EQ(fx.store->refQueuePendingForTest(fx.ns), 2u) << "a killed query's follower withdrew its item";
    }
    {
        std::lock_guard lk(sync->m);
        released = true;
    }
    sync->cv.notify_all();

    ASSERT_EQ(follower.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the stopped follower must not hang";
    ASSERT_EQ(leader.fut.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the leader must not hang";
    const LeaderRun run = follower.get();
    const AppendResult rl = leader.fut.get();
    follower_thread.join();
    leader.t.join();
    fx.store->setRefPreCarveHookForTest(nullptr);
    fx.store->setRefFollowerWaitHookForTest(nullptr);

    EXPECT_EQ(exceptionCode(rl.err), 0);
    EXPECT_EQ(leader_builds->load(), 1);
    const std::vector<RefLogTxn> logs = listLogTxns(*fx.faults, fx.store->layout(), fx.ns);
    const auto leader_txn = std::find_if(logs.begin(), logs.end(), [&](const RefLogTxn & txn) { return txn.txn_id == rl.id; });
    ASSERT_NE(leader_txn, logs.end());
    if (owner == StoppedBy::Background)
    {
        EXPECT_EQ(exceptionCode(run.result.err), DB::ErrorCodes::ABORTED);
        EXPECT_EQ(follower_builds.load(), 0) << "a withdrawn item is never built";
        EXPECT_EQ(leader_txn->ops, addRemovePrecommitPairs("item_leader", 1, 950000001)) << "the withdrawn item is absent";
    }
    else
    {
        EXPECT_EQ(exceptionCode(run.result.err), 0) << "a killed query's queued item commits";
        EXPECT_EQ(follower_builds.load(), 1);
        const bool follower_committed
            = std::any_of(logs.begin(), logs.end(), [&](const RefLogTxn & txn) { return txn.txn_id == run.result.id; });
        EXPECT_TRUE(follower_committed);
    }
}

}

/// A follower whose background owner stopped while its item is queued withdraws it: it gets the owner's verdict, its
/// `build_ops` never runs, nothing of it reaches the log, and the leader then commits alone.
TEST(CASRefWriterChunkedFlush, BackgroundStoppedPendingFollowerWithdrawsWhileLeaderBlocked)
{
    runFollowerStoppedWhileQueued(StoppedBy::Background);
}

/// A follower whose query was killed while its item is queued stays queued and commits with the leader.
TEST(CASRefWriterChunkedFlush, KilledPendingFollowerStaysQueuedAndCommits)
{
    runFollowerStoppedWhileQueued(StoppedBy::Query);
}

/// A follower whose item was carved belongs to the batch: its background owner stops after the carve and before
/// its `build_ops` returns, and the follower sees the stop while the item is carved and not done. It still waits
/// for the chunk and returns the committed id. A killed query's follower never sees a stop and gets the same id.
TEST(CASRefWriterChunkedFlush, KilledCarvedFollowerWaitsForActualResult)
{
    for (const StoppedBy owner : {StoppedBy::Background, StoppedBy::Query})
    {
        SCOPED_TRACE(nameOf(owner));
        KilledLeaderFixture fx("killed_carved_follower");
        auto sync = std::make_shared<CaseSync>();
        armPreCarveBlock(fx.store, fx.ns, sync, 2);
        auto leader_builds = std::make_shared<std::atomic<int>>(0);
        std::atomic<int> follower_builds{0};
        std::atomic<bool> follower_returned{false};
        std::atomic<int> carved_waits{0};
        std::function<void()> stop_follower;
        fx.store->setRefFollowerWaitHookForTest([&](FollowerWaitPhase phase)
        {
            if (phase == FollowerWaitPhase::Carved)
                carved_waits.fetch_add(1);
        });
        /// Runs on the leader's thread, after the carve: stop the follower, then hold the batch until a stopped
        /// background owner's follower has seen the stop and entered its carved wait, or has wrongly returned.
        const auto stop_follower_while_carved = [&]
        {
            stop_follower();
            if (owner == StoppedBy::Query)
                return;
            const auto deadline = std::chrono::steady_clock::now() + KILL_WATCHDOG;
            while (carved_waits.load() == 0 && !follower_returned && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
        };

        AppendCaller leader = launchAppendOps(fx.store, fx.ns, MutationScope::ref("item_leader"),
            addRemovePrecommitPairs("item_leader", 1, 960000001), leader_builds);
        waitEntered(sync);
        std::thread follower_thread;
        std::future<LeaderRun> follower = launchQueryFollower(
            follower_thread, fx, "item_follower", follower_builds, 960000002, stop_follower, stop_follower_while_carved,
            follower_returned, owner);
        waitPendingAtLeast(fx.store, fx.ns, 2);
        sync->cv.notify_all();

        ASSERT_EQ(follower.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the carved follower must not hang";
        ASSERT_EQ(leader.fut.wait_for(KILL_WATCHDOG), std::future_status::ready) << "the leader must not hang";
        const LeaderRun run = follower.get();
        const AppendResult rl = leader.fut.get();
        follower_thread.join();
        leader.t.join();
        fx.store->setRefFollowerWaitHookForTest(nullptr);
        fx.store->setRefPreCarveHookForTest(nullptr);

        if (owner == StoppedBy::Background)
            EXPECT_GE(carved_waits.load(), 1) << "the follower never saw its stop while its item was carved";
        else
            EXPECT_EQ(carved_waits.load(), 0) << "a killed query is not a stop for the follower's wait";
        EXPECT_EQ(exceptionCode(run.result.err), 0) << "a carved item reports its chunk's result, not its owner's verdict";
        EXPECT_EQ(exceptionCode(rl.err), 0);
        EXPECT_EQ(run.result.id, rl.id) << "both items were in one chunk";
        EXPECT_EQ(follower_builds.load(), 1);
        const std::vector<RefLogTxn> logs = listLogTxns(*fx.faults, fx.store->layout(), fx.ns);
        EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [&](const RefLogTxn & txn) { return txn.txn_id == rl.id; }), 1);
    }
}
