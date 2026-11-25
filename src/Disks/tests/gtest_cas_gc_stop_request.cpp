#include <gtest/gtest.h>

#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

/// What a stop request ends: the planning of the life in progress, the owner cleanup of a scheduler tick, and the
/// destructive work of a round that sees it before its intake ends.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const UInt128 kLeader = hexToU128("000000000000000000000000000000a1");

/// The retired row of `ref` in the adopted seal; the pool has one shard.
std::optional<RetiredEntry> retiredEntryFor(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    for (const RetiredEntry & entry : currentRetiredSet(backend, layout, blobShard(ref, 1)))
        if (entry.ref == ref)
            return entry;
    return std::nullopt;
}

RoundReport runGateRound(const PoolPtr & store, Gc & gc)
{
    const RoundReport report = gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
    store->renewWatermarkOnce();
    return report;
}

/// Writes one part holding `payload` through the writer.
BlobMaterializationAction writeOnePart(const PoolPtr & store, const String & ns, const String & ref_name,
                                       const String & payload)
{
    const RootNamespace root{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref_name;
    auto build = store->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(root, ref_name, id);
    const BlobUploadResult uploaded = build->uploadBlobDetached(BlobUploadRequest{
        .ref = entry.ref, .source = BlobSource::fromString(payload), .declared_size = payload.size()});
    build->mergeBlobUploadResults(std::span<const BlobUploadResult>(&uploaded, 1));
    build->promote(root, ref_name, build->buildId(), id);
    return uploaded.diagnostics.action;
}

UInt64 metricOr(const std::map<String, UInt64> & metrics, const String & name)
{
    const auto it = metrics.find(name);
    return it == metrics.end() ? 0 : it->second;
}

size_t indexOfMark(const std::vector<JournalEntry> & journal, const String & label)
{
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Mark && journal[i].detail == label)
            return i;
    return journal.size();
}

/// The stream prefix (`.../cas/ns/stream/<life>/`) of the first ref log written since the journal was cleared.
String firstWrittenStreamPrefix(const CountingBackend & backend)
{
    for (const JournalEntry & entry : backend.journal())
        if (entry.verb == JournalVerb::Write)
            for (const String & key : entry.keys)
                if (const size_t log = key.find("/_log/"); log != String::npos && key.find("/cas/ns/stream/") != String::npos)
                    return key.substr(0, log + 1);
    return {};
}

/// The stream `_log` keys written since the journal was cleared, in write order.
std::vector<String> writtenRefLogKeys(const CountingBackend & backend)
{
    std::vector<String> keys;
    for (const JournalEntry & entry : backend.journal())
        if (entry.verb == JournalVerb::Write)
            for (const String & key : entry.keys)
                if (key.find("/_log/") != String::npos && key.find("/cas/ns/stream/") != String::npos)
                    keys.push_back(key);
    return keys;
}

RefTxnId refIdOfKey(const Layout & layout, const String & key)
{
    const auto parsed = layout.parseRefObjectKey(key);
    return parsed ? parsed->txn_id : RefTxnId{};
}

/// The coverage the newest fold seal records for the catalog life of `ns`.
std::optional<RefCoverage> lifeCoverage(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const uint64_t attempt = currentAttemptOf(backend, layout);
    const UInt128 life_id = catalogLifeIdForTest(backend, layout, ns);
    OperationForTest op(backend);
    for (uint64_t g = currentGenerationOf(backend, layout); ; --g)
    {
        if (const auto got = (*op).read(layout.foldSealKey(g, attempt), Retry::once()))
        {
            const CasFoldSeal seal = decodeFoldSeal(got->bytes);
            const auto it = seal.ref_lives.find(life_id);
            return it == seal.ref_lives.end() ? std::nullopt : std::optional<RefCoverage>(it->second.coverage);
        }
        if (g == 0)
            return std::nullopt;
    }
}

/// While armed, every served `GET` of a ref log under a registered stream runs the armed callback with that key,
/// after the store answered. Each such `GET` is recorded with whether it found the key.
class HotLogReadBackend : public GetHookBackend
{
public:
    struct HotRead
    {
        String key;
        bool present = false;
    };

    void addStream(const String & stream)
    {
        std::lock_guard lock(hot_mutex);
        streams.push_back(stream);
    }

    void arm(std::function<void(const String &)> callback)
    {
        std::lock_guard lock(hot_mutex);
        on_hot_read = std::move(callback);
    }

    void disarm()
    {
        std::lock_guard lock(hot_mutex);
        on_hot_read = {};
    }

    std::vector<HotRead> hotReads() const
    {
        std::lock_guard lock(hot_mutex);
        return hot_reads;
    }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        auto result = GetHookBackend::read(key, access);
        std::function<void(const String &)> callback;
        {
            std::lock_guard lock(hot_mutex);
            if (!on_hot_read || key.find("/_log/") == String::npos
                || std::none_of(streams.begin(), streams.end(), [&](const String & s) { return key.starts_with(s); }))
                return result;
            hot_reads.push_back({key, result.has_value()});
            callback = on_hot_read;
        }
        callback(key);
        return result;
    }

private:
    mutable std::mutex hot_mutex;
    std::vector<String> streams;
    std::function<void(const String &)> on_hot_read;
    std::vector<HotRead> hot_reads;
};

/// A confirmed `pending(t)`, a live sentinel part and two hot lives with a backlog above one read window. During
/// `run` with `step_ms`, every served `GET` of a hot life's log appends the next record to that life and advances
/// the round's clock by `step_ms`.
class HotLivesPool
{
public:
    static constexpr uint64_t kIoConcurrency = 2;
    /// `GcReadAhead::window`: pass 1 takes this many records of each life.
    static constexpr uint64_t kWindow = 4 * kIoConcurrency;

    struct Life
    {
        String stream;
        std::optional<NamespaceLifeId> life;
        RefTxnId tail;
        RefTxnId cursor_before;
        uint64_t appended = 0;
    };

    struct Round
    {
        RoundReport report;
        std::map<String, UInt64> intake;
        String log;
        uint64_t elapsed_ms = 0;
        /// The round's requests and the indices of the marks at `AfterDiscovery` and `AfterIntake` in them.
        std::vector<JournalEntry> journal;
        size_t planned = 0;
        size_t intake_done = 0;
        std::map<String, UInt64> reduce;
    };

    HotLivesPool()
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0,
                          .gc_io_concurrency = kIoConcurrency};
        /// No background snapshot publisher writes into the hot lives while the test appends to them.
        config.snapshot_log_count_threshold = std::numeric_limits<uint64_t>::max();
        store = Pool::open(backend, std::move(config));
        gc = std::make_unique<Gc>(store, kLeader, std::function<uint64_t()>{}, [this] { return mono.load(); });
        EXPECT_EQ(writeOnePart(store, ns_sentinel, "s", sentinel), BlobMaterializationAction::Published);
        for (const String & ns : hot)
        {
            backend->clearJournal();
            EXPECT_EQ(writeOnePart(store, ns, "seed", "the seed of " + ns), BlobMaterializationAction::Published);
            lives[ns].stream = firstWrittenStreamPrefix(*backend);
            EXPECT_FALSE(lives[ns].stream.empty()) << ns;
            backend->addStream(lives[ns].stream);
            OperationForTest op(*backend);
            lives[ns].life = CasRefCatalog::lifeIfCataloged(*op, store->layout(), RootNamespace{ns});
            EXPECT_TRUE(lives[ns].life.has_value()) << ns;
        }
    }

    /// Ends with `pending(t)` committed and a backlog of `parts` parts in each hot life.
    void prepare(size_t parts)
    {
        ASSERT_EQ(writeOnePart(store, ns_old, "p0", payload), BlobMaterializationAction::Published);
        runGateRound(store, *gc);
        store->dropRef(RootNamespace{ns_old}, "p0");
        store->renewWatermarkOnce();
        runGateRound(store, *gc);
        ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "the condemning round did not commit pending(t)";

        for (const String & ns : hot)
        {
            Life & life = lives[ns];
            life.cursor_before = cursorOf(ns);
            backend->clearJournal();
            for (size_t i = 0; i < parts; ++i)
                ASSERT_EQ(writeOnePart(store, ns, fmt::format("b{}", i), fmt::format("backlog {} of {}", i, ns)),
                          BlobMaterializationAction::Published);
            for (const String & key : writtenRefLogKeys(*backend))
                life.tail = std::max(life.tail, refIdOfKey(store->layout(), key));
            ASSERT_GT(life.tail.ref_sequence, life.cursor_before.ref_sequence + kWindow)
                << ns << ": the backlog does not exceed one read window";
        }
    }

    /// One round; without `step_ms` the writers are idle. `also` runs after this pool's own round point handling.
    Round run(std::optional<uint64_t> step_ms, std::function<void(const Gc::RoundPointSiteForTest &)> also = {})
    {
        Round out;
        uint64_t began_ms = 0;
        gc->setRoundPointHookForTest([&](const Gc::RoundPointSiteForTest & site)
        {
            if (site.point == RoundInterleavePoint::AfterCatalogRead)
            {
                began_ms = mono.load();
                if (step_ms)
                    backend->arm([this, step = *step_ms](const String & key) { appendAfter(key, step); });
            }
            else if (site.point == RoundInterleavePoint::AfterDiscovery)
                backend->mark("planned");
            else if (site.point == RoundInterleavePoint::AfterIntake)
            {
                backend->mark("intake_done");
                backend->disarm();
                out.elapsed_ms = mono.load() - began_ms;
            }
            if (also)
                also(site);
        });
        gc->setPhaseSink([&out](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_ref_intake")
                out.intake = record.metrics;
            else if (record.phase == "fold_reduce")
                out.reduce = record.metrics;
        });
        const size_t from = backend->journal().size();
        {
            ScopedLogCapture capture("CasGc", "information");
            out.report = runGateRound(store, *gc);
            out.log = capture.captured();
        }
        backend->disarm();
        gc->setRoundPointHookForTest({});
        gc->setPhaseSink({});

        const auto journal = backend->journal();
        out.journal.assign(journal.begin() + from, journal.end());
        out.planned = indexOfMark(out.journal, "planned");
        out.intake_done = indexOfMark(out.journal, "intake_done");
        return out;
    }

    RefTxnId cursorOf(const String & ns) const
    {
        const auto coverage = lifeCoverage(*backend, store->layout(), RootNamespace{ns});
        return coverage ? coverage->last_folded_ref_id : RefTxnId{};
    }

    String logKey(const String & ns, const RefTxnId & id) const { return store->layout().refLogKey(lives.at(ns).life.value(), id); }

    std::optional<RetiredEntry> row() const { return retiredEntryFor(*backend, store->layout(), idOf(payload)); }

    std::shared_ptr<HotLogReadBackend> backend = std::make_shared<HotLogReadBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    std::atomic<uint64_t> mono{1'000'000};
    const String ns_old = "test/a_old";
    const String ns_sentinel = "test/s_sentinel";
    const std::vector<String> hot{"test/h_one", "test/h_two"};
    const String payload = "stop-request-pending";
    const String sentinel = "stop-request-sentinel";
    std::map<String, Life> lives;

private:
    /// Appends under one lock, so a life never has a hole for the walk to read below a record planning saw.
    void appendAfter(const String & key, uint64_t step_ms)
    {
        std::lock_guard lock(append_mutex);
        for (const String & ns : hot)
        {
            Life & life = lives.at(ns);
            if (!key.starts_with(life.stream))
                continue;
            life.tail = RefTxnId{life.tail.writer_epoch, life.tail.ref_sequence + 1};
            const ManifestRef mref{.writer_epoch = life.tail.writer_epoch, .build_sequence = 1'000'000 + next_blob,
                                   .manifest_ordinal = 1};
            const DB::UInt128 blob(0xA00000 + next_blob);
            ++next_blob;
            writeBlobBody(*backend, store->layout(), blob);
            writeManifestRaw(*backend, store->layout(), RootNamespace{ns}, mref, {blobEntryFor("data.bin", blob)});
            RefLogTxn txn;
            txn.ns = ns;
            txn.txn_id = life.tail;
            txn.ops = publishCommittedOps(fmt::format("late_{}", life.tail.ref_sequence), mref);
            OperationForTest op(*backend);
            EXPECT_TRUE(std::holds_alternative<Committed>((*op).create(
                logKey(ns, life.tail), sealObject(FormatId::RefLog, encodeRefLogTxn(txn)), Retry::standard())));
            ++life.appended;
            mono.fetch_add(step_ms);
            return;
        }
    }

    std::mutex append_mutex;
    uint64_t next_blob = 0;
};

/// After a round that left the hot lives unfinished, the next one starts each at its cursor and finishes it.
void expectTheNextRoundContinuesFrom(HotLivesPool & pool, const std::map<String, RefTxnId> & reached)
{
    const HotLivesPool::Round next = pool.run(/*step_ms=*/std::nullopt);
    EXPECT_GT(metricOr(next.intake, "frontier_namespaces"), 0u);
    EXPECT_EQ(metricOr(next.intake, "frontier_proven"), metricOr(next.intake, "frontier_namespaces")) << next.log;
    for (const String & ns : pool.hot)
    {
        SCOPED_TRACE(ns);
        const HotLivesPool::Life & life = pool.lives.at(ns);
        std::vector<RefTxnId> read_ids;
        ASSERT_LT(next.intake_done, next.journal.size());
        for (size_t i = 0; i < next.intake_done; ++i)
            if (const JournalEntry & entry = next.journal[i]; entry.verb == JournalVerb::Read && entry.keys.front().starts_with(life.stream)
                && entry.keys.front().find("/_log/") != String::npos)
                read_ids.push_back(refIdOfKey(pool.store->layout(), entry.keys.front()));
        ASSERT_FALSE(read_ids.empty());
        EXPECT_EQ(*std::min_element(read_ids.begin(), read_ids.end()), reached.at(ns)) << "the next round did not start at the cursor reached";
        EXPECT_EQ(pool.cursorOf(ns), life.tail);
    }
}

}

/// A stop request raised while one life is being planned ends that planning before its next probe: the life is not
/// walked, keeps its cursor and stays unproven by the slice-cut reason, the round deletes nothing, and the next
/// round plans and proves it.
TEST(CASGcStopRequest, AStopDuringPlanningEndsTheLifeBeforeItsNextProbe)
{
    HotLivesPool pool;
    ASSERT_NO_FATAL_FAILURE(pool.prepare(/*parts=*/5));
    const String body_key = pool.store->layout().blobKey(idOf(pool.payload));
    const String & first = pool.hot.front();
    const String first_stream = pool.lives.at(first).stream;
    /// Planning of the first hot life reads the record after its cursor, then the listed tail: its second probe.
    const String raise_at = pool.logKey(first, pool.lives.at(first).tail);

    std::atomic<bool> stop{false};
    std::atomic<size_t> hot_reads_at_stop{0};
    pool.gc->setStopSignal([&stop] { return stop.load(); });
    pool.backend->armBeforeGet([raise_at](const String & key) { return key == raise_at; }, [&]
    {
        hot_reads_at_stop = pool.backend->hotReads().size();
        stop = true;
    });
    const uint64_t deleted_before = pool.backend->deleteTotal();
    /// Every read of a hot log appends the next record, so planning finds each probe present and keeps doubling;
    /// the clock stands still, so the slice cannot end anything.
    const HotLivesPool::Round cut = pool.run(/*step_ms=*/0);
    pool.gc->setStopSignal({});
    ASSERT_TRUE(stop.load()) << "positive control: planning read the listed tail";

    const auto reads = pool.backend->hotReads();
    size_t first_reads_after_stop = 0;
    for (size_t i = hot_reads_at_stop.load(); i < reads.size(); ++i)
        first_reads_after_stop += reads[i].key.starts_with(first_stream);
    EXPECT_LE(first_reads_after_stop, 1u) << "planning of the life read on after the stop request, beyond the read in flight";

    EXPECT_NE(cut.log.find("asked to stop"), String::npos) << cut.log;
    EXPECT_NE(cut.log.find("intake_slice_cut="), String::npos) << cut.log;
    EXPECT_EQ(cut.log.find("discovery_incomplete="), String::npos) << "an unplanned life was walked past its plan: " << cut.log;
    EXPECT_GT(metricOr(cut.intake, "frontier_namespaces"), 0u);
    EXPECT_EQ(metricOr(cut.intake, "frontier_proven"), 0u) << cut.log;
    EXPECT_EQ(cut.report.bodies_deleted, 0u);
    EXPECT_EQ(pool.backend->deleteTotal(), deleted_before) << "the stopped round deleted an object";
    EXPECT_TRUE(objectPresent(*pool.backend, body_key));
    EXPECT_TRUE(pool.row() && pool.row()->delete_pending) << "the stopped round dropped pending(t)";

    std::map<String, RefTxnId> reached;
    for (const String & ns : pool.hot)
    {
        SCOPED_TRACE(ns);
        reached[ns] = pool.cursorOf(ns);
        EXPECT_EQ(reached[ns], pool.lives.at(ns).cursor_before) << "a cursor moved without a walk";
    }
    expectTheNextRoundContinuesFrom(pool, reached);
}

namespace
{

/// A pool that never admitted a namespace, so its catalog is proved empty, holding one condemned blob whose body is
/// present.
struct EmptyPoolWithCondemnedBlob
{
    EmptyPoolWithCondemnedBlob()
    {
        writeBlobBody(*backend, store->layout(), hash);
        OperationForTest op(*backend);
        const std::optional<Meta> observed = (*op).head(store->layout().blobKey(ref), Retry::once());
        EXPECT_TRUE(observed.has_value()) << "the seeded body must be present before it is condemned";
        injectRetire(*backend, store->layout(), /*round*/ 1, /*shard*/ 0,
            {RetiredEntry{.kind = ObjectKind::Blob, .ref = ref, .token = PersistedEtag::capture(observed.value().etag), .size = 0}});
        store->renewWatermarkOnce();
        EXPECT_TRUE(CasRefCatalog::read(*op, store->layout()).catalog.entries.empty());
        gc = std::make_unique<Gc>(store, kLeader);
    }

    bool bodyPresent() const { return objectPresent(*backend, store->layout().blobKey(ref)); }
    bool rowPending() const
    {
        const std::optional<RetiredEntry> entry = retiredEntryFor(*backend, store->layout(), ref);
        return entry && entry->delete_pending;
    }

    /// One round; the reduce phase's metrics. With `stop_at_catalog_read`, a stop is requested right after the
    /// round's catalog read.
    std::map<String, UInt64> round(bool stop_at_catalog_read)
    {
        std::atomic<bool> stop{false};
        std::map<String, UInt64> reduce;
        gc->setStopSignal([&stop] { return stop.load(); });
        gc->setRoundPointHookForTest([&](const Gc::RoundPointSiteForTest & site)
        {
            if (stop_at_catalog_read && site.point == Gc::RoundPointForTest::AfterCatalogRead)
                stop = true;
        });
        gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_reduce")
                reduce = record.metrics;
        });
        runGateRound(store, *gc);
        gc->setPhaseSink({});
        gc->setRoundPointHookForTest({});
        gc->setStopSignal({});
        EXPECT_EQ(stop.load(), stop_at_catalog_read) << "the round did not reach its catalog read";
        return reduce;
    }

    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    PoolPtr store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/0);
    std::unique_ptr<Gc> gc;
    const UInt128 hash = UInt128(0xbead);
    const BlobRef ref = legacyMetaTestRef(hash);
};

}

/// A proved-empty catalog leaves the intake no namespace to poll. A stop requested after the catalog read still
/// keeps the round that would delete the condemned body from deleting anything; the next round deletes it.
TEST(CASGcStopRequest, AStopAfterTheCatalogReadOfAnEmptyPoolSuppressesTheDeletingRound)
{
    /// Positive control: unstopped rounds, to find the round that deletes the body.
    constexpr size_t kMaxRounds = 5;
    size_t deleting_round = 0;
    {
        EmptyPoolWithCondemnedBlob control;
        for (size_t i = 1; i <= kMaxRounds && control.bodyPresent(); ++i)
        {
            control.round(/*stop_at_catalog_read=*/false);
            if (!control.bodyPresent())
                deleting_round = i;
        }
        ASSERT_GT(deleting_round, 1u) << "positive control: an unstopped round deletes the body after it is pending";
    }

    EmptyPoolWithCondemnedBlob pool;
    for (size_t i = 1; i < deleting_round; ++i)
        pool.round(/*stop_at_catalog_read=*/false);
    ASSERT_TRUE(pool.bodyPresent());
    ASSERT_TRUE(pool.rowPending()) << "the round before the deleting one must leave the row pending";

    pool.backend->resetCounts();
    const std::map<String, UInt64> stopped = pool.round(/*stop_at_catalog_read=*/true);
    EXPECT_EQ(metricOr(stopped, "frontier_complete"), 1u) << "the proved-empty catalog completes the frontier";
    EXPECT_EQ(metricOr(stopped, "suppress_destructive"), 1u) << "the stop must suppress the round's destructive work";
    EXPECT_EQ(pool.backend->deleteCountForKeysContaining("/blobs/"), 0u) << "the stopped round deleted the body";
    EXPECT_EQ(pool.backend->deleteTotal(), 0u) << "the stopped round deleted an object";
    EXPECT_TRUE(pool.bodyPresent());
    EXPECT_TRUE(pool.rowPending()) << "the stopped round dropped the pending row";

    const std::map<String, UInt64> next = pool.round(/*stop_at_catalog_read=*/false);
    EXPECT_EQ(metricOr(next, "suppress_destructive"), 0u);
    EXPECT_FALSE(pool.bodyPresent()) << "the round after the stopped one must delete the body";
}

/// Every life finishes its walk before the stop arrives, at the last walk of the intake. The round still deletes
/// nothing, and the next round deletes the body.
TEST(CASGcStopRequest, AStopAtTheLastWalkSuppressesARoundWhoseFrontierIsComplete)
{
    {
        HotLivesPool control;
        ASSERT_NO_FATAL_FAILURE(control.prepare(/*parts=*/5));
        const HotLivesPool::Round round = control.run(/*step_ms=*/std::nullopt);
        ASSERT_EQ(metricOr(round.reduce, "suppress_destructive"), 0u) << round.log;
        ASSERT_FALSE(objectPresent(*control.backend, control.store->layout().blobKey(idOf(control.payload))))
            << "positive control: the unstopped round deletes the pending body";
    }

    HotLivesPool pool;
    ASSERT_NO_FATAL_FAILURE(pool.prepare(/*parts=*/5));
    const String body_key = pool.store->layout().blobKey(idOf(pool.payload));
    std::atomic<bool> stop{false};
    size_t final_walks = 0;
    pool.gc->setStopSignal([&stop] { return stop.load(); });
    const uint64_t deleted_before = pool.backend->deleteTotal();
    const HotLivesPool::Round cut = pool.run(/*step_ms=*/std::nullopt, [&](const Gc::RoundPointSiteForTest & site)
    {
        if (site.point == Gc::RoundPointForTest::AfterNamespaceWalk && site.next.empty())
        {
            ++final_walks;
            stop = true;
        }
    });
    pool.gc->setStopSignal({});
    ASSERT_EQ(final_walks, 1u) << "the stop must be raised at the intake's last walk only";

    EXPECT_GT(metricOr(cut.intake, "frontier_namespaces"), 0u);
    EXPECT_EQ(metricOr(cut.intake, "frontier_proven"), metricOr(cut.intake, "frontier_namespaces"))
        << "every life must finish its walk before the stop: " << cut.log;
    EXPECT_EQ(metricOr(cut.reduce, "frontier_complete"), 1u) << cut.log;
    EXPECT_EQ(metricOr(cut.reduce, "suppress_destructive"), 1u) << cut.log;
    EXPECT_NE(cut.log.find("asked to stop"), String::npos) << cut.log;
    EXPECT_EQ(cut.report.bodies_deleted, 0u);
    EXPECT_EQ(pool.backend->deleteTotal(), deleted_before) << "the stopped round deleted an object";
    EXPECT_TRUE(objectPresent(*pool.backend, body_key));
    EXPECT_TRUE(pool.row() && pool.row()->delete_pending) << "the stopped round dropped pending(t)";

    const HotLivesPool::Round next = pool.run(/*step_ms=*/std::nullopt);
    EXPECT_EQ(metricOr(next.reduce, "suppress_destructive"), 0u) << next.log;
    EXPECT_FALSE(objectPresent(*pool.backend, body_key)) << "the round after the stopped one must delete the body";
}

/// A tick that begins as the scheduler is stopped skips the owner cleanup of `Creating` rows: it reads no catalog
/// before its round starts. The round itself still runs, and its intake ends at once.
TEST(CASGcStopRequest, ATickAfterAStopRequestSkipsTheOwnerCleanup)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    config.mount_renew_period = std::chrono::milliseconds{3'600'000};
    auto store = Pool::open(backend, std::move(config));
    const String catalog_key = store->layout().refCatalogKey();

    std::mutex mutex;
    /// Per tick, the catalog `GET`s when the tick began and when its round emitted its `Start` row.
    std::vector<uint64_t> gets_at_tick;
    std::vector<uint64_t> gets_at_round_start;
    std::promise<void> first_round_finished;
    std::promise<void> second_tick_began;
    CasGcScheduler sched(store, std::chrono::seconds(3600), "test::gc", "ca", [&](const GcRoundLogRecord & record)
    {
        if (record.trigger != GcRoundLogRecord::Trigger::Scheduled)
            return;
        std::lock_guard lock(mutex);
        if (record.event_type == GcRoundLogRecord::EventType::Start)
            gets_at_round_start.push_back(backend->getCount(catalog_key));
        else if (record.event_type == GcRoundLogRecord::EventType::Finish && gets_at_round_start.size() == 1)
            first_round_finished.set_value();
    });
    sched.setBeforeTickLockHookForTest([&]
    {
        size_t tick = 0;
        {
            std::lock_guard lock(mutex);
            gets_at_tick.push_back(backend->getCount(catalog_key));
            tick = gets_at_tick.size();
        }
        if (tick != 2)
            return;
        second_tick_began.set_value();
        while (!sched.stopRequested())
            std::this_thread::yield();
    });

    sched.start();
    sched.requestRoundSoon();
    ASSERT_EQ(first_round_finished.get_future().wait_for(std::chrono::seconds(60)), std::future_status::ready)
        << "the first tick did not finish its round";
    sched.requestRoundSoon();
    ASSERT_EQ(second_tick_began.get_future().wait_for(std::chrono::seconds(60)), std::future_status::ready)
        << "the second tick did not begin";
    /// Returns once the second tick, released by the stop request, has run to its end.
    sched.stop();

    std::lock_guard lock(mutex);
    ASSERT_EQ(gets_at_tick.size(), 2u);
    ASSERT_EQ(gets_at_round_start.size(), 2u) << "positive control: the tick after the stop request ran its round";
    EXPECT_GE(gets_at_round_start[0] - gets_at_tick[0], 1u) << "positive control: a tick reads the catalog for the owner cleanup";
    EXPECT_EQ(gets_at_round_start[1] - gets_at_tick[1], 0u) << "the owner cleanup read the catalog after the stop request";
}
