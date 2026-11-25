#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"
#include <base/scope_guard.h>

#include <Common/ProfileEvents.h>

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int MEMORY_LIMIT_EXCEEDED;
}

namespace ProfileEvents
{
extern const Event CASRefLogBodyGets;
}

/// THE BOUNDED FOLD WALK.
///
/// Ref intake reads the chain by exact key in successor order and ends at the stop planning read absent,
/// or at a terminal record; nothing above that key is folded in this round, whatever lands meanwhile. A
/// time slice bounds how long that takes: the slice never cuts pass 1, which gives every namespace its start and one
/// read window, pass 2 continues the rest from `round mod n`, and a namespace the slice cuts keeps its cursor
/// and stays unproven.
///
/// So the properties these tests pin are:
///   * a round stops at its first absent key, and a quiet namespace costs its `_ckpt` and one probe;
///   * a round where no tail moved is skipped outright by the existing defer machinery;
///   * the slice leaves an unfinished namespace unproven and starves none of them;
///   * a cold cursor resumes without reading a key cleanup may have reclaimed;
///   * a manifest edge fold costs one GET and never a HEAD;
///   * a namespace that folded nothing still keeps its sealed coverage row.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// Composed over `CountingBackend` because these tests assert REQUEST COUNTS: "folds nothing and reads
/// once" is the claim, and only a counting backend can check it.
using CountingHintHoleBackend = DB::Cas::tests::HintHoleBackendOn<DB::Cas::tests::CountingBackend>;

/// The `RefCoverage` the newest fold seal recorded for `ns`'s opaque catalog life. Scans downward from
/// the adopted generation for the most recent fold seal, mirroring `foldCursorOf`'s reasoning (a
/// completed round's `gc/state` points at the recheck generation, which writes a completion seal rather
/// than a fold seal).
std::optional<RefCoverage> coverageOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    DB::Cas::tests::OperationForTest op(backend);
    const uint64_t gen = currentGenerationOf(backend, layout);
    const uint64_t attempt = currentAttemptOf(backend, layout);
    const UInt128 life_id = catalogLifeIdForTest(backend, layout, ns);
    for (uint64_t g = gen; ; --g)
    {
        if (const auto got = (*op).read(layout.foldSealKey(g, attempt), Retry::standard()))
        {
            const CasFoldSeal seal = decodeFoldSeal(got->bytes);
            const auto it = seal.ref_lives.find(life_id);
            if (it == seal.ref_lives.end())
                return std::nullopt;
            return it->second.coverage;
        }
        if (g == 0)
            return std::nullopt;
    }
}

RefTxnId cursorOf(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const auto cov = coverageOf(backend, layout, ns);
    return cov ? cov->last_folded_ref_id : RefTxnId{};
}

/// A phase metric, or 0 when the row does not carry it. Reading it this way rather than through
/// `std::map::at` is deliberate: against the unbounded walk these columns do not exist yet, and a
/// missing column should fail the assertion that names it, not abort the test with an exception.
UInt64 metric(const std::map<String, UInt64> & row, const String & name)
{
    const auto it = row.find(name);
    return it == row.end() ? 0 : it->second;
}

/// Every key the backend was asked to delete, so a failing zero-delete assertion names the site that
/// leaked instead of only reporting a count.
String deletedKeysMessage(const CountingBackend & backend)
{
    String out;
    for (const String & key : backend.deletedKeys())
        out += "\n    " + key;
    return out.empty() ? String{" (none)"} : out;
}

/// Every `_log/` GET this round issued against `ns` over ids `first..last` -- the "read once, fold
/// nothing" claim, made against the store rather than against a counter the fold keeps about itself.
uint64_t refLogGetsFor(const CountingBackend & backend, const Layout & layout, const RootNamespace & ns,
                       uint64_t first, uint64_t last, uint64_t epoch = 1)
{
    uint64_t total = 0;
    for (uint64_t i = first; i <= last; ++i)
        total += backend.getCount(layout.refLogKey(fixture::fixtureLife(ns), RefTxnId{epoch, i}));
    return total;
}

/// One round driven directly on `Gc`, capturing the `fold_ref_intake` phase row. Driving `Gc` rather
/// than `CasGcScheduler` is what lets a caller choose the universe policy, which the suppression test
/// needs. An EMPTY row means the round deferred and folded nothing at all.
std::map<String, UInt64> runRoundCapturingIntake(Gc & gc, UniversePolicy policy = UniversePolicy::kDefault)
{
    std::map<String, UInt64> intake;
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    const RoundReport report = gc.runRegularRound({}, /*allow_steal*/true, policy);
    gc.setPhaseSink({});
    EXPECT_TRUE(report.acquired_lease) << "the round must have run at all";
    return intake;
}

/// Publish ids `first .. last` of `ns` in writer epoch 1, each pinning its own blob.
void publishRange(Backend & backend, const Layout & layout, const RootNamespace & ns, uint64_t first, uint64_t last)
{
    for (uint64_t i = first; i <= last; ++i)
        publishAt(backend, layout, ns, RefTxnId{1, i}, "ref_" + std::to_string(i), i, DB::UInt128(i),
                  /*birth=*/i == 1);
}

String logKey(const Layout & layout, const RootNamespace & ns, const RefTxnId & id)
{
    return layout.refLogKey(fixture::fixtureLife(ns), id);
}

/// The stored bytes of a log at `id` that publishes `ref_name` -> a fresh manifest pinning `blob`. The blob
/// and the manifest are written now; the log itself is not.
String publishLogBytes(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id,
                       const String & ref_name, uint64_t build_sequence, const DB::UInt128 & blob)
{
    const ManifestRef mref{.writer_epoch = id.writer_epoch, .build_sequence = build_sequence, .manifest_ordinal = 1};
    writeBlobBody(backend, layout, blob);
    writeManifestRaw(backend, layout, ns, mref, {blobEntryFor("data.bin", blob)});
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    txn.ops = publishCommittedOps(ref_name, mref);
    return sealObject(FormatId::RefLog, encodeRefLogTxn(txn));
}

/// A sequence-1 head at `id` whose backlink is `prev`, publishing `ref_name` -> a fresh manifest pinning
/// `blob`. Header generation 3: below it a backlink must name the immediately preceding epoch.
void publishHeadAt(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id,
                   const String & ref_name, uint64_t build_sequence, const DB::UInt128 & blob, const RefTxnId & prev)
{
    const ManifestRef mref{.writer_epoch = id.writer_epoch, .build_sequence = build_sequence, .manifest_ordinal = 1};
    writeBlobBody(backend, layout, blob);
    writeManifestRaw(backend, layout, ns, mref, {blobEntryFor("data.bin", blob)});
    RefLogTxn txn;
    txn.ns = ns.string();
    txn.txn_id = id;
    txn.ops = publishCommittedOps(ref_name, mref);
    txn.prev_epoch_seal = prev;
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(
        logKey(layout, ns, id), sealObject(FormatId::RefLog, encodeRefLogTxnAtGeneration(txn, 3)), Retry::standard())));
}

/// Create or replace `ns`'s `_ckpt` with `ckpt` at header `generation`.
void putCkptAtGeneration(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefCkpt & ckpt,
                         uint32_t generation)
{
    OperationForTest op(backend);
    const String key = layout.refCkptKey(fixture::fixtureLife(ns));
    const String bytes = encodeRefCkptAtGeneration(ckpt, generation);
    if (const auto head = (*op).head(key, Retry::standard()))
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, bytes, head->etag, Retry::standard())));
    else
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::standard())));
}

void removeObject(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    const auto head = (*op).head(key, Retry::standard());
    ASSERT_TRUE(head.has_value()) << key;
    (*op).remove(key, head->etag, Retry::standard());
    ASSERT_FALSE((*op).head(key, Retry::standard()).has_value()) << key;
}

/// A log at `id` that drops the committed ref `ref_name` -> `mref`: folded, it takes one edge off the blob.
void writeDropAt(Backend & backend, const Layout & layout, const RootNamespace & ns, const RefTxnId & id,
                 const String & ref_name, const ManifestRef & mref)
{
    writeTxnAt(backend, layout, ns, id,
        {ownerTransitionOp(RefOwnerBinding{RefOwnerKind::Committed, ref_name, mref}, std::nullopt)});
}

/// A GET that finds `key` absent stores the armed bytes there, once per key: a record lands just after the
/// walk read its slot as absent.
class LateAppendBackend : public CountingBackend
{
public:
    void armLate(const String & key, const String & bytes)
    {
        std::lock_guard lock(late_mutex);
        armed[key] = bytes;
    }

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        auto result = CountingBackend::read(key, access);
        if (result)
            return result;
        String bytes;
        {
            std::lock_guard lock(late_mutex);
            const auto it = armed.find(key);
            if (it == armed.end())
                return result;
            bytes = std::move(it->second);
            armed.erase(it);
        }
        InMemoryBackend::write(key, bytes, std::nullopt, access);
        return result;
    }

private:
    std::mutex late_mutex;
    std::map<String, String> armed;
};

/// A `mono_ms_fn` that jumps by one intake slice the first time the backend serves a GET of any key
/// registered with `stepAfterGet`; every registration is spent by that one step.
class SteppingClock : public CountingBackend
{
public:
    std::function<uint64_t()> monoMsFn()
    {
        return [this] { return now.load(); };
    }

    void stepAfterGet(const String & key)
    {
        std::lock_guard lock(clock_mutex);
        armed.insert(key);
    }

    /// Jumps by one intake slice now.
    void step() { now.fetch_add(Gc::kRefIntakeBudgetMs); }

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        auto result = CountingBackend::read(key, access);
        std::lock_guard lock(clock_mutex);
        if (armed.contains(key))
        {
            armed.clear();
            now.fetch_add(Gc::kRefIntakeBudgetMs);
        }
        return result;
    }

private:
    std::mutex clock_mutex;
    std::set<String> armed;
    std::atomic<uint64_t> now{1'000'000};
};

/// A small read-ahead window keeps the slice fixtures short: `GcReadAhead::window` is `4 * gc_io_concurrency`.
constexpr uint64_t kIoConcurrency = 2;
constexpr uint64_t kWindow = 4 * kIoConcurrency;

template <typename BackendT>
PoolPtr openPoolWithSmallWindow(std::shared_ptr<BackendT> backend)
{
    return Pool::open(std::move(backend), PoolConfig{
        .pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = kIoConcurrency});
}

RoundReport runRoundCapturingIntakeAndReport(Gc & gc, std::map<String, UInt64> & intake)
{
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            intake = rec.metrics;
    });
    RoundReport report = gc.runRegularRound({}, /*allow_steal*/true, UniversePolicy::kDefault);
    gc.setPhaseSink({});
    EXPECT_TRUE(report.acquired_lease) << "the round must have run at all";
    return report;
}

void setRemoving(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    OperationForTest op(backend);
    CasRefCatalog::casUpdate(*op, layout, [&](const RefCatalog & current)
    {
        RefCatalog next = current;
        const auto it = std::find_if(next.entries.begin(), next.entries.end(),
            [&](const CatalogEntry & entry) { return entry.ns == ns; });
        if (it == next.entries.end())
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "fixture namespace '{}' has no catalog entry", ns.string());
        it->state = NsState::Removing;
        it->removal_started_round = 1;
        return next;
    });
}

}

/// ===================== (c) A ROUND WHERE NO TAIL MOVED IS SKIPPED OUTRIGHT =====================
///
/// The round-level skip is the EXISTING defer machinery, whose signal is already exactly this
/// comparison: `RefScanSummary::changed_shards` counts the namespaces whose greatest listed log sits
/// above their sealed cursor. So a round in which no tail moved folds nothing at all -- no intake phase,
/// no per-namespace walk, not even the probes -- and an append un-defers it.
TEST(CASGCBoundedWalk, ARoundWhereNoTailMovedIsDeferredAndAnAppendUnDefersIt)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);   /// the DEFAULT defer window: this test is about the skip
    const Layout & layout = store->layout();
    const RootNamespace a{"00/aa@cas@"};
    const RootNamespace b{"00/bb@cas@"};

    publishRange(*backend, layout, a, 1, 2);
    publishRange(*backend, layout, b, 1, 2);
    writeRecoverableCkptForRawFixture(*backend, layout, a, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    writeRecoverableCkptForRawFixture(*backend, layout, b, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    ASSERT_FALSE(runRoundCapturingIntake(gc).empty()) << "the seeding round must actually fold";
    ASSERT_EQ(cursorOf(*backend, layout, a), (RefTxnId{1, 2}));
    ASSERT_EQ(cursorOf(*backend, layout, b), (RefTxnId{1, 2}));

    backend->resetCounts();
    EXPECT_TRUE(runRoundCapturingIntake(gc).empty())
        << "no tail moved, so the round has no fold to run";
    EXPECT_EQ(refLogGetsFor(*backend, layout, a, 1, 8), 0u) << "a deferred round reads no ref log at all";
    EXPECT_EQ(refLogGetsFor(*backend, layout, b, 1, 8), 0u);

    /// One append, in `a` only.
    publishAt(*backend, layout, a, RefTxnId{1, 3}, "ref_3", 3, DB::UInt128(0xaa3));
    advanceRecoverableCkptForRawFixture(*backend, layout, a, RefTxnId{1, 3});

    backend->resetCounts();
    const std::map<String, UInt64> woken = runRoundCapturingIntake(gc);
    ASSERT_FALSE(woken.empty()) << "an append must un-defer the round";
    EXPECT_EQ(metric(woken, "tails_advanced"), 1u) << "the appended-to namespace is walked again";
    EXPECT_EQ(metric(woken, "tails_unchanged"), 1u) << "and only that one";
    EXPECT_EQ(cursorOf(*backend, layout, a), (RefTxnId{1, 3}));
    EXPECT_EQ(refLogGetsFor(*backend, layout, b, 1, 8), 1u)
        << "the still-quiet namespace costs one probe of its remembered successor";
}

/// A `_ckpt` this build cannot read (header 3 under read ceiling 2) holds the namespace: the round folds
/// nothing for it and destroys nothing.
TEST(CASGCBoundedWalk, HeaderThreeCkptUnderCeilingTwoHoldsTheNamespace)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/hot@cas@"};
    const DB::UInt128 blob(0xd00d);

    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "kept", 1, DB::UInt128(0x1), /*birth=*/true);
    const ManifestRef doomed{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1};
    writeBlobBody(*backend, layout, blob);
    writeManifestRaw(*backend, layout, ns, doomed, {blobEntryFor("data.bin", blob)});
    writeTxnAt(*backend, layout, ns, RefTxnId{1, 2}, publishCommittedOps("doomed", doomed));
    dropRefTransition(*backend, layout, ns, "doomed", doomed);

    const String ckpt_key = layout.refCkptKey(fixture::fixtureLife(ns));
    {
        OperationForTest op(*backend);
        const auto head = (*op).head(ckpt_key, Retry::standard());
        ASSERT_TRUE(head.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(
            ckpt_key, encodeRefCkptAtGeneration(refCkptOf(1), 3), head->etag, Retry::standard())));
    }

    ScopedReadCeiling c(2);
    backend->resetCounts();
    Gc gc(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc, UniversePolicy::Authoritative);

    EXPECT_EQ(metric(intake, "frontier_proven"), 0u);
    EXPECT_EQ(metric(intake, "frontier_namespaces"), 1u);
    EXPECT_EQ(backend->deleteTotal(), 0u);
    OperationForTest head_op(*backend);
    EXPECT_TRUE((*head_op).head(layout.blobKey(legacyMetaTestRef(blob)), Retry::standard()).has_value());
}

/// A store that hides a namespace's records from every LIST does not lose them: the namespace goes
/// QUIET, and a quiet namespace is exactly the shape the exact-key probe at `cursor + 1` exists for. It
/// has no listed tail at all, and no bound is taken from a listing anyway -- bounding a namespace by a
/// tail the liar refuses to admit to would hand it the omission it was hoping for.
TEST(CASGCBoundedWalk, AListHiddenTailIsCaughtAndFoldedByTheQuietProbePath)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/liar@cas@"};

    publishRange(*backend, layout, ns, 1, 2);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 2},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

    /// A third record lands and the store stops listing the namespace at the same moment: its listed
    /// tail is now nothing at all, while `{1, 3}` is durable and readable by exact key.
    publishAt(*backend, layout, ns, RefTxnId{1, 3}, "ref_3", 3, DB::UInt128(0x1a3));
    advanceRecoverableCkptForRawFixture(*backend, layout, ns, RefTxnId{1, 3});
    backend->hidePrefix(layout.namespaceStreamPrefix(fixture::fixtureLife(ns)));

    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}))
        << "the exact-key probe sees what LIST omits, so the hidden record is folded, not lost";
    EXPECT_EQ(metric(intake, "unhinted_quiet_walked"), 1u) << "it is the quiet path that caught it";
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(0x1a3)), 1)
        << "the hidden record's owner edge must be folded, or its blob looks unreferenced";
}

/// ===================== (e) ONE ROUND TRIP PER MANIFEST EDGE =====================
///
/// The edge fold used to pay HEAD-then-GET: two serial round trips per manifest edge, on every folded
/// log, where the GET alone already answers "is it there".
TEST(CASGCBoundedWalk, ManifestEdgeFoldsPayAGetAndNeverAHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    const uint64_t records = 3;
    publishRange(*backend, layout, ns, 1, records);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, records},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    backend->resetCounts();
    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, records})) << "the round must have folded them";

    uint64_t manifest_heads = 0;
    uint64_t manifest_gets = 0;
    for (uint64_t i = 1; i <= records; ++i)
    {
        const ManifestId id{ns, ManifestRef{.writer_epoch = 1, .build_sequence = i, .manifest_ordinal = 1}};
        manifest_heads += backend->headCount(layout.manifestKey(id));
        manifest_gets += backend->getCount(layout.manifestKey(id));
    }
    EXPECT_EQ(manifest_heads, 0u) << "the fold must not HEAD a manifest body it is about to GET";
    EXPECT_GT(manifest_gets, 0u) << "the bodies were read, so the counters really are watching these keys";
}

/// An absent manifest body still takes the record-and-continue path -- the GET's own absence is the
/// signal the HEAD used to carry -- and still raises the fold barrier without ever HEADing the key.
TEST(CASGCBoundedWalk, AnAbsentManifestBodyStillHoldsWithoutAHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};

    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 3},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    const ManifestId gone{ns, ManifestRef{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 1}};
    deleteManifestBody(*backend, layout, gone);

    backend->resetCounts();
    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);

    const auto cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(cov.has_value());
    EXPECT_EQ(cov->last_folded_ref_id, (RefTxnId{1, 1}))
        << "the cursor stays BELOW the log whose manifest body is missing";
    ASSERT_TRUE(cov->hold.has_value()) << "an absent committed manifest body raises the fold barrier";
    EXPECT_EQ(cov->hold->reason, HoldReason::ManifestBodyMissing);
    EXPECT_EQ(cov->hold->offending_position, (RefTxnId{1, 2}));
    EXPECT_EQ(cov->classification, CoverageClass::Clamped);
    EXPECT_EQ(backend->headCount(layout.manifestKey(gone)), 0u)
        << "absence is decided by the GET, so the missing body costs no HEAD either";
}

/// ===================== (f) A NAMESPACE THAT FOLDED NOTHING KEEPS ITS COVERAGE ROW ===================
///
/// The fold seal writes a row only for the namespaces the intake loop visits, so any future shortcut
/// that stops visiting a namespace with nothing to fold would DROP its cursor -- and a dropped cursor is
/// not a lost optimisation, it is a re-fold from `{0, 0}`: every owner edge counted a second time, every
/// blob's in-degree inflated, and the eventual correction mass-condemning live data. This pins the row
/// against exactly that.
TEST(CASGCBoundedWalk, ANamespaceThatFoldedNothingKeepsItsSealedCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const RootNamespace moved{"00/moved@cas@"};

    publishRange(*backend, layout, quiet, 1, 3);
    publishRange(*backend, layout, moved, 1, 1);
    writeRecoverableCkptForRawFixture(*backend, layout, quiet, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 3},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });
    writeRecoverableCkptForRawFixture(*backend, layout, moved, RefCkpt{
        .life_epoch = 1,
        .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt,
        .last_epoch_seal = std::nullopt,
    });

    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    const auto before = coverageOf(*backend, layout, quiet);
    ASSERT_TRUE(before.has_value());
    ASSERT_EQ(before->last_folded_ref_id, (RefTxnId{1, 3}));
    ASSERT_FALSE(before->hold.has_value());

    /// A second round in which `quiet` folds nothing and `moved` does, so the round really does write a
    /// new seal that could have dropped the row.
    publishAt(*backend, layout, moved, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(0x22));
    advanceRecoverableCkptForRawFixture(*backend, layout, moved, RefTxnId{1, 2});
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    ASSERT_EQ(metric(intake, "tails_unchanged"), 1u) << "the fixture must actually exercise the quiet case";

    const auto after = coverageOf(*backend, layout, quiet);
    ASSERT_TRUE(after.has_value())
        << "the coverage row was DROPPED -- the next round would re-fold this namespace from {0,0}";
    /// The CURSOR and the HOLD are what the next round trusts, and both ride unchanged.
    /// `classification` legitimately moves from `Folded` ("this round folded records") to `Unchanged`,
    /// because that is what the round did — it is the one field that may differ, so it is the one field
    /// asserted loosely.
    EXPECT_EQ(after->last_folded_ref_id, before->last_folded_ref_id)
        << "a namespace that folded nothing must keep the cursor it had";
    EXPECT_EQ(after->hold, before->hold);
    EXPECT_NE(after->classification, CoverageClass::Clamped) << "folding nothing is not a clamp";
    EXPECT_EQ(metric(intake, "frontier_namespaces"), 2u)
        << "it stays in the round's universe, so its proof is still owed";
}

/// ===================== THE CUT AT THE FIRST ABSENT KEY =====================

/// A warm GC remembers the successor of the cursor it sealed, so a quiet namespace costs its `_ckpt`
/// and one probe of that successor, and never re-reads the cursor.
TEST(CASGCBoundedWalk, QuietNamespaceCostsCkptAndOneProbeOnceTheSuccessorIsRemembered)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/quiet@cas@"};
    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}));

    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    backend->resetCounts();
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    EXPECT_EQ(backend->getCount(layout.refCkptKey(fixture::fixtureLife(ns))), 1u);
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 4})), 1u) << "the remembered successor is probed";
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 3})), 0u) << "the cursor is not re-read";
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);
}

/// A GC without the memo learns the cursor's kind by reading it: one more GET than the warm path.
TEST(CASGCBoundedWalk, ColdCursorCostsOneMoreRead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/cold@cas@"};
    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}));

    {
        Gc warm(store, kGc);
        runRoundCapturingIntake(warm);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    backend->resetCounts();
    Gc cold(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(cold);
    EXPECT_EQ(backend->getCount(layout.refCkptKey(fixture::fixtureLife(ns))), 1u);
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 3})), 1u) << "the cursor is read for its kind";
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 4})), 1u);
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);
}

/// The round ends at the first key it reads absent, even when that key is present a moment later, and it
/// never reads past it. The record that landed is the next round's.
TEST(CASGCBoundedWalk, ARoundStopsAtItsFirstAbsentKey)
{
    auto backend = std::make_shared<LateAppendBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/late@cas@"};
    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}));
    backend->armLate(logKey(layout, ns, RefTxnId{1, 4}),
                     publishLogBytes(*backend, layout, ns, RefTxnId{1, 4}, "ref_4", 4, DB::UInt128(0x44)));
    backend->armLate(logKey(layout, ns, RefTxnId{1, 5}),
                     publishLogBytes(*backend, layout, ns, RefTxnId{1, 5}, "ref_5", 5, DB::UInt128(0x55)));

    Gc gc(store, kGc);
    const std::map<String, UInt64> first = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
    EXPECT_EQ(metric(first, "frontier_proven"), 1u);
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 5})), 0u) << "nothing past the absent key is read";

    runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 4})) << "the next round folds {1,4} and stops at {1,5}";
}

/// A generation 3 `_ckpt` has no `committed_through`, and its absence proves nothing: the walk reads the
/// chain from `{life_epoch,1}`.
TEST(CASGCBoundedWalk, GenerationThreeCkptWithoutAFrontierIsWalkedNotProven)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/g3@cas@"};
    publishRange(*backend, layout, ns, 1, 2);
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);

    Gc gc(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 3})), 1u) << "the frontier is proved by a read";
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);
}

/// A terminal record ends the stream: it proves the frontier and no key after it is read.
TEST(CASGCBoundedWalk, TerminalRecordProvesTheFrontierWithoutAProbe)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/terminal@cas@"};
    publishRange(*backend, layout, ns, 1, 2);
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    writeTxnAt(*backend, layout, ns, RefTxnId{1, 3}, {remove_op});
    setRemoving(*backend, layout, ns);
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);

    Gc gc(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3})) << "the terminal record is folded";
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 4})), 0u);
}

/// ===================== THE TIME SLICE =====================

/// The slice ends after pass 1: the namespace keeps the cursor it reached, is unproven, and the round
/// destroys nothing. A round with time left finishes the walk.
TEST(CASGCBoundedWalk, SliceCutLeavesTheNamespaceUnprovenAndKeepsItsCursor)
{
    auto backend = std::make_shared<SteppingClock>();
    auto store = openPoolWithSmallWindow(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/hot@cas@"};
    const uint64_t tail = 3 * kWindow;
    publishRange(*backend, layout, ns, 1, tail);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
    backend->stepAfterGet(logKey(layout, ns, RefTxnId{1, kWindow}));

    backend->resetCounts();
    Gc gc(store, kGc, {}, backend->monoMsFn());
    const std::map<String, UInt64> first = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, kWindow}));
    EXPECT_EQ(metric(first, "frontier_proven"), 0u);
    EXPECT_EQ(metric(first, "frontier_namespaces"), 1u);
    EXPECT_EQ(backend->deleteTotal(), 0u) << "Deleted:" << deletedKeysMessage(*backend);

    const std::map<String, UInt64> second = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, tail}));
    EXPECT_EQ(metric(second, "frontier_proven"), 1u);
}

/// The slice deadline is taken before planning ends (before `AfterDiscovery`), so planning time is charged to it:
/// planning that takes the whole slice leaves pass 2 no read, and planning one millisecond shorter leaves a walk
/// that takes no time enough to finish.
TEST(CASGCBoundedWalk, PlanningTimeIsChargedToTheSlice)
{
    for (const uint64_t planning_ms : {Gc::kRefIntakeBudgetMs - 1, Gc::kRefIntakeBudgetMs})
    {
        SCOPED_TRACE(planning_ms);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolWithSmallWindow(backend);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/hot@cas@"};
        const uint64_t tail = 3 * kWindow;
        publishRange(*backend, layout, ns, 1, tail);
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));

        std::atomic<uint64_t> mono{1'000'000};
        Gc gc(store, kGc, {}, [&mono] { return mono.load(); });
        ScopedRoundCallback planning(gc, RoundInterleavePoint::AfterDiscovery, {}, [&] { mono.fetch_add(planning_ms); });
        const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
        ASSERT_EQ(planning.fired(), 1u);
        const bool cut = planning_ms >= Gc::kRefIntakeBudgetMs;
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, cut ? kWindow : tail}));
        EXPECT_EQ(metric(intake, "frontier_proven"), cut ? 0u : 1u);
    }
}

namespace
{
/// The text of the first line of `text` that contains `needle`, or empty.
String lineContaining(const String & text, std::string_view needle)
{
    size_t from = 0;
    while (from < text.size())
    {
        size_t end = text.find('\n', from);
        if (end == String::npos)
            end = text.size();
        const String line = text.substr(from, end - from);
        if (line.find(needle) != String::npos)
            return line;
        from = end + 1;
    }
    return {};
}

/// The number after `key=` in `line`, or nullopt when the line has no such field.
std::optional<uint64_t> fieldOf(const String & line, const String & key)
{
    const size_t at = line.find(" " + key + "=");
    if (at == String::npos)
        return std::nullopt;
    return std::stoull(line.substr(at + key.size() + 2));
}

constexpr std::string_view kPlanningLine = "ref intake planned";
constexpr std::string_view kProgressLine = "ref intake progress";
}

/// One folding round plans once, whatever its lives look like: one quiet life, whose first key is absent, and one
/// hot life with a backlog.
TEST(CASGCLog, APlannedRoundLogsItsStopsOnce)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolWithSmallWindow(backend);
    const Layout & layout = store->layout();
    const RootNamespace quiet{"00/quiet@cas@"};
    const RootNamespace hot{"01/hot@cas@"};
    publishRange(*backend, layout, quiet, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, quiet, refCkptOf(1, RefTxnId{1, 3}));
    publishRange(*backend, layout, hot, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, hot, refCkptOf(1, RefTxnId{1, 3}));

    Gc gc(store, kGc);
    {
        ScopedLogCapture capture("CasGc", "information");
        runRoundCapturingIntake(gc);
        EXPECT_EQ(capture.count(kPlanningLine), 1u) << "positive control: the first round folds and plans";
    }

    publishRange(*backend, layout, hot, 4, 12);
    advanceRecoverableCkptForRawFixture(*backend, layout, hot, RefTxnId{1, 12});
    ScopedLogCapture capture("CasGc", "information");
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    ASSERT_EQ(metric(intake, "frontier_namespaces"), 2u);
    EXPECT_EQ(capture.count(kPlanningLine), 1u) << capture.captured();
    const String line = lineContaining(capture.captured(), kPlanningLine);
    SCOPED_TRACE(line);
    EXPECT_NE(line.find("round="), String::npos);
    EXPECT_EQ(fieldOf(line, "lives"), 2u);
    EXPECT_EQ(fieldOf(line, "hot"), 1u);
    EXPECT_EQ(fieldOf(line, "quiet"), 1u);
    EXPECT_EQ(fieldOf(line, "empty_hint"), 1u) << "the quiet life lists nothing above its first key";
    EXPECT_EQ(fieldOf(line, "crossings"), 0u);
    EXPECT_EQ(fieldOf(line, "crossing_limit"), 0u);
    EXPECT_EQ(fieldOf(line, "planner_misses"), 2u) << "one absent key ends each life";
    EXPECT_GT(fieldOf(line, "retained_bytes").value_or(0), 0u);
}

/// Planning keeps the bodies it read only up to a byte cap. A body larger than what is left under a nonzero cap is
/// not kept, and the walk reads it again. Planning reads the first record and, through the listing's last id, the
/// tail one; the walk then reads every record up to the stop.
TEST(CASGCLog, ABodyBeyondTheRetentionCapIsReadAgainByTheWalk)
{
    const auto run = [](std::optional<size_t> cap_bytes)
    {
        struct Out
        {
            uint64_t retained_bytes = 0;
            std::vector<uint64_t> gets;   /// per sequence 1..tail
            size_t first_body_bytes = 0;
            size_t last_body_bytes = 0;
            uint64_t body_get_events = 0;
        } out;
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolWithSmallWindow(backend);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/capped@cas@"};
        const uint64_t tail = 12;
        publishRange(*backend, layout, ns, 1, tail);
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
        {
            OperationForTest op(*backend);
            const auto first = (*op).read(logKey(layout, ns, RefTxnId{1, 1}), Retry::standard());
            const auto last = (*op).read(logKey(layout, ns, RefTxnId{1, tail}), Retry::standard());
            EXPECT_TRUE(first.has_value() && last.has_value());
            out.first_body_bytes = first ? first->bytes.size() : 0;
            out.last_body_bytes = last ? last->bytes.size() : 0;
        }
        backend->resetCounts();

        Gc gc(store, kGc);
        if (cap_bytes)
            gc.setRefPlanRetainedBytesForTest(*cap_bytes);
        ScopedLogCapture capture("CasGc", "information");
        const uint64_t events_before = ProfileEvents::global_counters[ProfileEvents::CASRefLogBodyGets];
        runRoundCapturingIntake(gc);
        out.body_get_events = ProfileEvents::global_counters[ProfileEvents::CASRefLogBodyGets] - events_before;
        const String line = lineContaining(capture.captured(), kPlanningLine);
        EXPECT_FALSE(line.empty());
        out.retained_bytes = fieldOf(line, "retained_bytes").value_or(0);
        for (uint64_t seq = 1; seq <= tail; ++seq)
            out.gets.push_back(backend->getCount(logKey(layout, ns, RefTxnId{1, seq})));
        return out;
    };

    const auto uncapped = run(std::nullopt);
    ASSERT_GT(uncapped.first_body_bytes, 0u);
    EXPECT_EQ(uncapped.retained_bytes, uncapped.first_body_bytes + uncapped.last_body_bytes)
        << "positive control: planning read the first and the tail record";
    for (const uint64_t gets : uncapped.gets)
        EXPECT_EQ(gets, 1u) << "under the default cap every record is read once";
    EXPECT_EQ(uncapped.body_get_events, std::accumulate(uncapped.gets.begin(), uncapped.gets.end(), uint64_t{0}))
        << "`CASRefLogBodyGets` does not count the ref-log bodies read";

    /// Room for the first body and one byte short of the tail one.
    const auto capped = run(uncapped.first_body_bytes + uncapped.last_body_bytes - 1);
    EXPECT_EQ(capped.retained_bytes, capped.first_body_bytes) << "only the first body fits under the cap";
    EXPECT_EQ(capped.gets.front(), 1u) << "the kept body is not read again";
    EXPECT_EQ(capped.gets.back(), 2u) << "the body that did not fit is read again by the walk";
    for (size_t i = 0; i + 1 < capped.gets.size(); ++i)
        EXPECT_EQ(capped.gets[i], 1u) << "sequence " << i + 1;
    EXPECT_EQ(capped.body_get_events, std::accumulate(capped.gets.begin(), capped.gets.end(), uint64_t{0}))
        << "`CASRefLogBodyGets` does not count the body the walk read again";
}

/// Advances the monotonic clock by `step_ms` on every served `GET` of a ref log.
class TickingLogBackend : public CountingBackend
{
public:
    std::function<uint64_t()> monoMsFn()
    {
        return [this] { return now.load(); };
    }

    void setStep(uint64_t step_ms) { step.store(step_ms); }

    std::optional<Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        auto result = CountingBackend::read(key, access);
        if (key.find("/_log/") != String::npos)
            now.fetch_add(step.load());
        return result;
    }

private:
    std::atomic<uint64_t> step{0};
    std::atomic<uint64_t> now{1'000'000};
};

/// While pass 2 walks, the round logs its progress at most once per minute of the monotonic clock; a walk shorter
/// than a minute logs none. The lines do not change what the round does.
TEST(CASGCLog, ThePassTwoProgressLineIsPacedByTheMonotonicClock)
{
    const auto run = [](uint64_t step_ms)
    {
        struct Out
        {
            size_t lines = 0;
            uint64_t pass_two_ms = 0;
            RefTxnId cursor;
        } out;
        auto backend = std::make_shared<TickingLogBackend>();
        auto store = openPoolWithSmallWindow(backend);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/long@cas@"};
        const uint64_t tail = 3 * kWindow;
        publishRange(*backend, layout, ns, 1, tail);
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
        backend->setStep(step_ms);

        Gc gc(store, kGc, {}, backend->monoMsFn());
        const auto now = backend->monoMsFn();
        uint64_t pass_two_began_ms = 0;
        gc.setRoundPointHookForTest([&](const Gc::RoundPointSiteForTest & site)
        {
            if (site.point == RoundInterleavePoint::AfterNamespaceWalk && site.pass == 1)
                pass_two_began_ms = now();
            else if (site.point == RoundInterleavePoint::AfterIntake)
                out.pass_two_ms = now() - pass_two_began_ms;
        });
        ScopedLogCapture capture("CasGc", "information");
        runRoundCapturingIntake(gc);
        gc.setRoundPointHookForTest({});
        out.lines = capture.count(kProgressLine);
        out.cursor = cursorOf(*backend, layout, ns);
        return out;
    };

    const auto fast = run(/*step_ms=*/100);
    EXPECT_LT(fast.pass_two_ms, 60'000u) << "positive control: the walk took under a minute";
    EXPECT_EQ(fast.lines, 0u);

    const auto slow = run(/*step_ms=*/11'000);
    EXPECT_GE(slow.pass_two_ms, 120'000u) << "positive control: pass 2 spanned minutes";
    EXPECT_LE(slow.lines, slow.pass_two_ms / 60'000);
    EXPECT_GE(slow.lines + 1, slow.pass_two_ms / 60'000);
    EXPECT_GE(slow.lines, 2u);
    EXPECT_EQ(slow.cursor, fast.cursor) << "logging changed what the round folded";
}

/// Deleting objects outside the protocol is unsupported. This records what the walk then does: a tail
/// above the cursor that vanished between rounds reads as the end of the log, so the namespace is proven,
/// not held.
TEST(CASGCBoundedWalk, ATailThatVanishedAboveTheCursorReadsAsTheEndOfTheLog)
{
    auto backend = std::make_shared<SteppingClock>();
    auto store = openPoolWithSmallWindow(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/vanished@cas@"};
    const uint64_t tail = 3 * kWindow;
    publishRange(*backend, layout, ns, 1, tail);
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);
    backend->stepAfterGet(logKey(layout, ns, RefTxnId{1, kWindow}));

    Gc gc(store, kGc, {}, backend->monoMsFn());
    runRoundCapturingIntake(gc);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, kWindow}));

    for (uint64_t seq = kWindow + 1; seq <= tail; ++seq)
        removeObject(*backend, logKey(layout, ns, RefTxnId{1, seq}));

    const std::map<String, UInt64> second = runRoundCapturingIntake(gc);
    const auto cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(cov.has_value());
    EXPECT_FALSE(cov->hold.has_value());
    EXPECT_EQ(cov->last_folded_ref_id, (RefTxnId{1, kWindow}));
    EXPECT_EQ(metric(second, "frontier_proven"), 1u);
}

/// Pass 1 gives every namespace one read window even after the slice ended. Pass 2 starts at index
/// `round mod n` of the unfinished namespaces, so no namespace is starved by its place in the order.
TEST(CASGCBoundedWalk, PassOneIsNeverCutAndPassTwoRotatesByRound)
{
    auto backend = std::make_shared<SteppingClock>();
    auto store = openPoolWithSmallWindow(backend);
    const Layout & layout = store->layout();
    const std::vector<RootNamespace> names{RootNamespace{"00/a@cas@"}, RootNamespace{"00/b@cas@"}, RootNamespace{"00/c@cas@"}};
    const uint64_t tail = 4 * kWindow;
    for (const RootNamespace & ns : names)
    {
        publishRange(*backend, layout, ns, 1, tail);
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
        backend->stepAfterGet(logKey(layout, ns, RefTxnId{1, 1}));
    }

    Gc gc(store, kGc, {}, backend->monoMsFn());
    std::map<String, UInt64> intake;
    runRoundCapturingIntakeAndReport(gc, intake);
    for (const RootNamespace & ns : names)
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, kWindow})) << ns.string();
    EXPECT_EQ(metric(intake, "frontier_proven"), 0u);

    /// The slice ends at the decode of the first pass-2 walk's last record `{1, tail}`, the `decodes`-th walk decode of
    /// the round. That walk still applies the record and ends at the stop planning fixed, which it checks before the
    /// slice; the next walk ends at its first slice check.
    const auto runEndingTheSliceAtDecode = [&](size_t decodes)
    {
        const auto seen = std::make_shared<size_t>(0);
        setWalkRecordDecodeProbeForTest([clock = backend, seen, decodes]
        {
            if (++*seen == decodes)
                clock->step();
        });
        SCOPE_EXIT({ setWalkRecordDecodeProbeForTest({}); });
        const RoundReport report = runRoundCapturingIntakeAndReport(gc, intake);
        EXPECT_GE(*seen, decodes) << "the slice never ended";
        return report;
    };
    /// Planning reads every namespace's end once per round.
    const auto endProbes = [&](const RootNamespace & ns)
    {
        return backend->getCount(logKey(layout, ns, RefTxnId{1, tail + 1}));
    };

    backend->resetCounts();
    /// Pass 1 takes `kWindow` records of each life, then the first pass-2 walk takes `{1, 2 * kWindow + 1} .. {1, tail}`.
    const RoundReport second = runEndingTheSliceAtDecode(names.size() * kWindow + 2 * kWindow);
    const size_t completed = second.round % names.size();
    for (size_t i = 0; i < names.size(); ++i)
    {
        EXPECT_EQ(cursorOf(*backend, layout, names[i]), (RefTxnId{1, i == completed ? tail : 2 * kWindow}))
            << names[i].string() << " in round " << second.round;
        EXPECT_EQ(endProbes(names[i]), 1u) << names[i].string();
    }
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);

    std::vector<RootNamespace> unfinished;
    for (size_t i = 0; i < names.size(); ++i)
        if (i != completed)
            unfinished.push_back(names[i]);
    backend->resetCounts();
    /// The life completed last round decodes nothing, the others take `kWindow` records each in pass 1, and the first
    /// pass-2 walk takes `{1, 3 * kWindow + 1} .. {1, tail}`.
    const RoundReport third = runEndingTheSliceAtDecode(unfinished.size() * kWindow + kWindow);
    const size_t completed_third = third.round % unfinished.size();
    for (size_t i = 0; i < unfinished.size(); ++i)
    {
        EXPECT_EQ(cursorOf(*backend, layout, unfinished[i]), (RefTxnId{1, i == completed_third ? tail : 3 * kWindow}))
            << unfinished[i].string() << " in round " << third.round;
        EXPECT_EQ(endProbes(unfinished[i]), 1u) << unfinished[i].string();
    }
    EXPECT_EQ(metric(intake, "frontier_proven"), 2u);
}

/// ===================== WHERE A COLD CURSOR RESUMES =====================

/// The cursor sits on a tombstone whose successor was absent. Its head lands and cleanup deletes the
/// tombstone: a new GC resumes at the linked epoch, never at `{1,4}`.
TEST(CASGCBoundedWalk, CursorOnATombstoneResumesAtTheLinkedEpoch)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/tomb@cas@"};
    publishRange(*backend, layout, ns, 1, 2);
    writeRefLogTxnRaw(*backend, layout, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 5));
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);
    {
        Gc first(store, kGc);
        runRoundCapturingIntake(first);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    publishHeadAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), RefTxnId{1, 3});
    removeObject(*backend, logKey(layout, ns, RefTxnId{1, 3}));
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1, std::nullopt, RefTxnId{5, 1}), 3);

    backend->resetCounts();
    Gc second(store, kGc);
    runRoundCapturingIntake(second);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 1}));
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 4})), 0u);
}

/// A seal without `next_epoch` links to the next numeric epoch.
TEST(CASGCBoundedWalk, CursorOnAnOldSealResumesAtTheNextEpoch)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/oldseal@cas@"};
    publishRange(*backend, layout, ns, 1, 2);
    writeSealAt(*backend, layout, ns, RefTxnId{1, 3});
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}, std::nullopt, RefTxnId{1, 3}));
    {
        Gc first(store, kGc);
        runRoundCapturingIntake(first);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    publishAt(*backend, layout, ns, RefTxnId{2, 1}, "ref_21", 1, DB::UInt128(0x21), false, RefTxnId{1, 3});
    replaceRecoverableCkptForRawFixture(*backend, layout, ns,
        refCkptOf(1, RefTxnId{2, 1}, std::nullopt, RefTxnId{1, 3}));

    Gc second(store, kGc);
    runRoundCapturingIntake(second);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{2, 1}));
}

/// A cursor below the snapshot id may name a key that cleanup emptied and a late create refilled, and so
/// may the key after it. Neither is read, nor hinted: the walk back-chains from the snapshot's epoch.
TEST(CASGCBoundedWalk, CursorBelowTheSnapshotIsNeverRead)
{
    {
        SCOPED_TRACE("generation 3 _ckpt");
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/below@cas@"};
        publishRange(*backend, layout, ns, 1, 1);
        writeRefLogTxnRaw(*backend, layout, tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5));
        putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);
        {
            Gc first(store, kGc);
            runRoundCapturingIntake(first);
        }
        ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

        publishHeadAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), RefTxnId{1, 2});
        publishAt(*backend, layout, ns, RefTxnId{5, 2}, "ref_52", 2, DB::UInt128(0x52));
        putCkptAtGeneration(*backend, layout, ns, refCkptOf(1, std::nullopt, RefTxnId{5, 2}), 3);

        backend->resetCounts();
        Gc second(store, kGc);
        runRoundCapturingIntake(second);
        EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 2})), 0u);
        EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 3})), 0u);
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 2}));
    }
    {
        SCOPED_TRACE("generation 1 _ckpt");
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/below1@cas@"};
        publishRange(*backend, layout, ns, 1, 1);
        writeSealAt(*backend, layout, ns, RefTxnId{1, 2});
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 2}, std::nullopt, RefTxnId{1, 2}));
        {
            Gc first(store, kGc);
            runRoundCapturingIntake(first);
        }
        ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

        /// Today's writer seals every numeric epoch it skips.
        writeSealAt(*backend, layout, ns, RefTxnId{2, 1}, RefTxnId{1, 2});
        writeSealAt(*backend, layout, ns, RefTxnId{3, 1}, RefTxnId{2, 1});
        writeSealAt(*backend, layout, ns, RefTxnId{4, 1}, RefTxnId{3, 1});
        publishAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), false, RefTxnId{4, 1});
        publishAt(*backend, layout, ns, RefTxnId{5, 2}, "ref_52", 2, DB::UInt128(0x52));
        replaceRecoverableCkptForRawFixture(*backend, layout, ns,
            refCkptOf(1, RefTxnId{5, 2}, RefTxnId{5, 2}, RefTxnId{4, 1}));

        backend->resetCounts();
        Gc second(store, kGc);
        runRoundCapturingIntake(second);
        EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 2})), 0u);
        EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 3})), 0u);
        EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 2}));
    }
}

/// A stray at the reclaimed cursor key, and one after it, each drop the ref of a live blob. Neither is
/// folded, so the blob survives every round.
TEST(CASGCBoundedWalk, StrayAtAReclaimedCursorKeyIsNotFolded)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/stray@cas@"};
    const DB::UInt128 live_blob(0x5a1);
    const ManifestRef kept{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "kept", 1, live_blob, /*birth=*/true);
    writeRefLogTxnRaw(*backend, layout, tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5));
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);
    {
        Gc first(store, kGc);
        runRoundCapturingIntake(first);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));

    publishHeadAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), RefTxnId{1, 2});
    publishAt(*backend, layout, ns, RefTxnId{5, 2}, "ref_52", 2, DB::UInt128(0x52));
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1, std::nullopt, RefTxnId{5, 2}), 3);
    removeObject(*backend, logKey(layout, ns, RefTxnId{1, 2}));
    writeDropAt(*backend, layout, ns, RefTxnId{1, 2}, "kept", kept);
    writeDropAt(*backend, layout, ns, RefTxnId{1, 3}, "kept", kept);

    Gc second(store, kGc);
    EXPECT_FALSE(runRoundsUntilAbsent(store, second, *backend, layout, live_blob, /*max_rounds*/ 6))
        << "a stray was folded: its drop condemned a blob the chain still references";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 2}));
}

/// Cleanup deleted a folded tombstone; strays at its key and the key after it drop a live blob's ref.
/// The warm GC goes straight to the linked epoch and the blob stays.
TEST(CASGCBoundedWalk, StrayAfterAReclaimedTombstoneIsNotFolded)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/straytomb@cas@"};
    const DB::UInt128 live_blob(0x5a2);
    const ManifestRef kept{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    publishAt(*backend, layout, ns, RefTxnId{1, 1}, "kept", 1, live_blob, /*birth=*/true);
    publishAt(*backend, layout, ns, RefTxnId{1, 2}, "ref_2", 2, DB::UInt128(0x62));
    writeRefLogTxnRaw(*backend, layout, tombstoneTxn(ns.string(), RefTxnId{1, 3}, 5));
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);

    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    publishHeadAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), RefTxnId{1, 3});
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1, std::nullopt, RefTxnId{5, 1}), 3);
    removeObject(*backend, logKey(layout, ns, RefTxnId{1, 3}));
    writeDropAt(*backend, layout, ns, RefTxnId{1, 3}, "kept", kept);
    writeDropAt(*backend, layout, ns, RefTxnId{1, 4}, "kept", kept);

    EXPECT_FALSE(runRoundsUntilAbsent(store, gc, *backend, layout, live_blob, /*max_rounds*/ 6))
        << "a stray was folded: its drop condemned a blob the chain still references";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{5, 1}));
}

/// A head whose backlink does not name the tombstone before it breaks the chain: the namespace holds at
/// the head, its cursor stays on the tombstone and the round deletes nothing.
TEST(CASGCBoundedWalk, HeadThatDoesNotNameItsTombstoneHoldsTheNamespace)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/badlink@cas@"};
    publishRange(*backend, layout, ns, 1, 1);
    writeRefLogTxnRaw(*backend, layout, tombstoneTxn(ns.string(), RefTxnId{1, 2}, 5));
    publishHeadAt(*backend, layout, ns, RefTxnId{5, 1}, "ref_51", 1, DB::UInt128(0x51), RefTxnId{1, 1});
    putCkptAtGeneration(*backend, layout, ns, refCkptOf(1), 3);

    backend->resetCounts();
    Gc gc(store, kGc);
    runRoundCapturingIntake(gc);
    const auto cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(cov.has_value());
    ASSERT_TRUE(cov->hold.has_value());
    EXPECT_EQ(cov->hold->reason, HoldReason::BodyUndecodable);
    EXPECT_EQ(cov->hold->offending_position, (RefTxnId{5, 1}));
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 2}));
    EXPECT_EQ(backend->deleteTotal(), 0u) << "Deleted:" << deletedKeysMessage(*backend);
}

/// The memo is used only while the durable cursor is the one it remembers. Another leader that moved the
/// cursor makes it stale: following it would fold `{1,4}` a second time and count its edge twice.
TEST(CASGCBoundedWalk, MemoIsNotUsedAfterAnotherLeaderMovedTheCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/memo@cas@"};
    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}));

    Gc first(store, kGc);
    runRoundCapturingIntake(first);
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));

    publishRange(*backend, layout, ns, 4, 5);
    advanceRecoverableCkptForRawFixture(*backend, layout, ns, RefTxnId{1, 5});
    {
        Gc other(store, kGc);
        runRoundCapturingIntake(other);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 5}));
    ASSERT_EQ(inDegreeOf(*backend, layout, DB::UInt128(4)), 1);

    backend->resetCounts();
    runRoundCapturingIntake(first);
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 5})), 1u) << "the moved cursor is read cold";
    EXPECT_EQ(backend->getCount(logKey(layout, ns, RefTxnId{1, 4})), 0u) << "the stale memo key is not read";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 5}));
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(4)), 1);
    EXPECT_EQ(inDegreeOf(*backend, layout, DB::UInt128(5)), 1);
}

/// A cursor at or above the snapshot id whose log is gone cannot be resumed: cleanup never deletes there,
/// so the namespace holds at `cursor + 1` and the round deletes nothing.
TEST(CASGCBoundedWalk, ColdCursorWhoseLogIsGoneHoldsTheNamespace)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/gone@cas@"};
    publishRange(*backend, layout, ns, 1, 3);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, 3}));
    {
        Gc first(store, kGc);
        runRoundCapturingIntake(first);
    }
    ASSERT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3}));
    removeObject(*backend, logKey(layout, ns, RefTxnId{1, 3}));

    backend->resetCounts();
    Gc cold(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(cold);
    const auto cov = coverageOf(*backend, layout, ns);
    ASSERT_TRUE(cov.has_value());
    ASSERT_TRUE(cov->hold.has_value());
    EXPECT_EQ(cov->hold->reason, HoldReason::UnconsumedSealCrossing);
    EXPECT_EQ(cov->hold->offending_position, (RefTxnId{1, 4}));
    EXPECT_EQ(cov->last_folded_ref_id, (RefTxnId{1, 3}));
    EXPECT_EQ(metric(intake, "frontier_proven"), 0u);
    EXPECT_EQ(backend->deleteTotal(), 0u) << "Deleted:" << deletedKeysMessage(*backend);
}

/// `SYSTEM CAS GC STOP` does not wait for a long intake: the background round is parked inside a walk read with a
/// backlog far beyond it, and once `stop` was called the round reads no further ref log, so `stop` returns.
TEST(CASGCBoundedWalk, ASchedulerStopReturnsWhileARoundIsInsideALongIntake)
{
    auto backend = std::make_shared<GetHookBackend>();
    auto store = Pool::open(backend, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1});
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/long@cas@"};
    const uint64_t tail = 40;
    publishRange(*backend, layout, ns, 1, tail);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));

    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    /// Planning reads `{1,1}`, the listed tail and the key after it; at concurrency 1 the walk reads `{1,3}` inline.
    /// At concurrency 1 the read-ahead window is 0 and pass 1 takes one record, so this read is in pass 2: the
    /// pass-2 coverage of the stop request.
    backend->armBeforeGet([key = logKey(layout, ns, RefTxnId{1, 3})](const String & k) { return k == key; }, [&]
    {
        entered.set_value();
        (void)released.wait_for(std::chrono::seconds(60));
    });
    CasGcScheduler sched(store, std::chrono::seconds(3600), "test::gc", "ca");
    sched.start();
    sched.requestRoundSoon();
    ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(60)), std::future_status::ready)
        << "the round never reached the walk";

    auto stopping = std::async(std::launch::async, [&sched] { sched.stop(); });
    while (!sched.stopRequested())
        std::this_thread::yield();
    const uint64_t gets_at_stop = refLogGetsFor(*backend, layout, ns, 1, tail + 1);
    release.set_value();
    ASSERT_EQ(stopping.wait_for(std::chrono::seconds(60)), std::future_status::ready) << "stop waited for the intake";
    EXPECT_TRUE(sched.isQuiescent());
    EXPECT_EQ(refLogGetsFor(*backend, layout, ns, 1, tail + 1), gets_at_stop + 1)
        << "after the stop request the walk read more than the read it was parked in";
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, 3})) << "the round did not keep the records it applied";
}

/// Planning reads a record it cannot decode as the end of what it plans, and the walk decides what that record
/// means when it reaches it. Any other error from the decode, here running out of memory, fails the round.
TEST(CASGCBoundedWalk, AnErrorOtherThanAMalformedRecordFailsThePlanningRound)
{
    for (const int code : {DB::ErrorCodes::CORRUPTED_DATA, DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED})
    {
        SCOPED_TRACE(code);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openPoolWithSmallWindow(backend);
        const Layout & layout = store->layout();
        const RootNamespace ns{"00/decoded@cas@"};
        const uint64_t tail = 12;
        publishRange(*backend, layout, ns, 1, tail);
        writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
        backend->resetCounts();

        Gc gc(store, kGc);
        setPlannedRecordDecodeProbeForTest([code] { throw DB::Exception(code, "injected while planning decodes a record"); });
        SCOPE_EXIT({ setPlannedRecordDecodeProbeForTest({}); });
        if (code == DB::ErrorCodes::MEMORY_LIMIT_EXCEEDED)
        {
            expectThrowsCode(code, [&] { (void)gc.runRegularRound(); });
        }
        else
        {
            const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
            EXPECT_GT(metric(intake, "frontier_namespaces"), 0u) << "positive control: the round folded";
            EXPECT_EQ(metric(intake, "frontier_proven"), 0u) << "planning observed nothing past the first record";
        }
        EXPECT_EQ(backend->deleteTotal(), 0u) << "Deleted:" << deletedKeysMessage(*backend);
    }
}

/// Pass 1 has a take budget, and its read-ahead stays within the records that budget can take, also when the listing
/// omits the records right after the expected one and names later ones. A body fetched beyond the budget would be
/// dropped at the end of pass 1 and fetched again in pass 2.
TEST(CASGCBoundedWalk, PassOneReadsNothingAheadBeyondItsTakeBudget)
{
    auto backend = std::make_shared<CountingHintHoleBackend>();
    auto store = openPoolWithSmallWindow(backend);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/holes@cas@"};
    const uint64_t tail = 3 * kWindow;
    publishRange(*backend, layout, ns, 1, tail);
    writeRecoverableCkptForRawFixture(*backend, layout, ns, refCkptOf(1, RefTxnId{1, tail}));
    std::vector<String> omitted;
    for (uint64_t seq = 2; seq <= kWindow; ++seq)
        omitted.push_back(logKey(layout, ns, RefTxnId{1, seq}));
    backend->setListOmissions(omitted);
    backend->resetCounts();

    Gc gc(store, kGc);
    const std::map<String, UInt64> intake = runRoundCapturingIntake(gc);
    EXPECT_EQ(cursorOf(*backend, layout, ns), (RefTxnId{1, tail}));
    EXPECT_EQ(metric(intake, "frontier_proven"), 1u);
    EXPECT_GT(metric(intake, "reflog_takes"), 0u);
    EXPECT_EQ(metric(intake, "reflog_reads_issued"), metric(intake, "reflog_takes")) << "a ref log was fetched and dropped";
    EXPECT_EQ(refLogGetsFor(*backend, layout, ns, 1, tail), tail) << "a ref log was read more than once";
}
