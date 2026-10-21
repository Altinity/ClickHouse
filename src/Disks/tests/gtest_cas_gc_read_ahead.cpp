#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/tests/cas_gc_marker_test_support.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::idOf;
using DB::Cas::tests::openRequestsForTest;
using DB::Cas::tests::u128Of;
using namespace DB::Cas::tests;

namespace
{

/// ============================ the class, on its own ============================

struct ReadAheadRig
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 4, /*max_free_threads*/ 4, /*queue_size*/ 0};

    void put(const String & key, const String & bytes)
    {
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, bytes, Retry::once()))) << key;
    }
};

}

TEST(CASGCReadAhead, HitReturnsTheHintedBytesWithOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    const auto got = reads.takeRead("k1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "one");
    EXPECT_EQ(reads.pending(), 0u);
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, MissReadsInlineOnTheCallersOperation)
{
    ReadAheadRig rig;
    rig.put("k2", "two");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    const auto got = reads.takeRead("k2");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "two");
    EXPECT_EQ(rig.backend->getCount("k2"), 1u);
}

TEST(CASGCReadAhead, AbsentKeyIsNulloptHintedOrNot)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);

    reads.hintRead("absent-hinted");
    EXPECT_FALSE(reads.takeRead("absent-hinted").has_value());
    EXPECT_FALSE(reads.takeRead("absent-inline").has_value());
}

TEST(CASGCReadAhead, DuplicateHintIsOneRequest)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintRead("k1");
    reads.hintRead("k1");
    EXPECT_EQ(reads.pending(), 1u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
}

TEST(CASGCReadAhead, WorkerExceptionRethrowsAtTheTakeSiteAndDoesNotPoisonTheKey)
{
    ReadAheadRig rig;
    rig.put("k3", "three");
    /// A non-Poco exception is a deterministic local failure to the engine, so it is thrown on the
    /// first attempt rather than reissued.
    rig.backend->failNextReadWith("k3", std::make_exception_ptr(std::runtime_error("injected read fault")));
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);

    reads.hintRead("k3");
    EXPECT_THROW(reads.takeRead("k3"), std::runtime_error);
    EXPECT_EQ(reads.pending(), 0u);

    const auto again = reads.takeRead("k3");   /// the fault was consumed; an inline read now answers
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->bytes, "three");
}

TEST(CASGCReadAhead, ConcurrencyOneNeverHints)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 1);
    rig.backend->resetCounts();

    EXPECT_EQ(reads.window(), 0u);
    reads.hintRead("k1");
    reads.hintHead("k1");
    EXPECT_EQ(reads.pending(), 0u);
    ASSERT_TRUE(reads.takeRead("k1").has_value());
    ASSERT_TRUE(reads.takeHead("k1").has_value());
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, DestructorWaitsForOutstandingRequests)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    rig.backend->resetCounts();
    {
        GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
        reads.hintRead("k1");
        reads.hintHead("k1");
    }
    EXPECT_EQ(rig.backend->getCount("k1"), 1u);
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, HeadHitCarriesSizeAndAbsentIsNullopt)
{
    ReadAheadRig rig;
    rig.put("k1", "one");
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 4);
    rig.backend->resetCounts();

    reads.hintHead("k1");
    const auto meta = reads.takeHead("k1");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->size, 3u);
    EXPECT_FALSE(reads.takeHead("absent").has_value());
    EXPECT_EQ(rig.backend->headCount("k1"), 1u);
}

TEST(CASGCReadAhead, WindowIsFourTimesConcurrency)
{
    ReadAheadRig rig;
    GcReadAhead reads(rig.op, rig.requests, rig.pool, 8);
    EXPECT_EQ(reads.window(), 32u);
}

/// ============================ the fold, at 1 against 8 ============================

namespace
{

const UInt128 kGc = u128Of("gc-read-ahead");

ManifestId publishPart(const PoolPtr & s, const String & ns, const String & ref, const String & payload)
{
    const RootNamespace nsr{ns};
    PartWriteInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->beginPartWrite(info);

    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of(payload))};
    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

/// Three namespaces. `wide` carries a ref-log backlog longer than any window this file uses, so the
/// same-epoch lookahead is genuinely exercised; `quiet` publishes once and never drops, so its
/// frontier is proved by the checkpoint ceiling with no read at all; `gone` is emptied entirely, so
/// its blobs reach in-degree zero and the reduce phase HEADs them. Every blob is unique to its part.
void populate(const PoolPtr & store)
{
    for (int i = 0; i < 30; ++i)
        publishPart(store, "srv1/wide", fmt::format("part_{}", i), fmt::format("wide-payload-{}", i));
    for (int i = 0; i < 15; ++i)
        store->dropRef(RootNamespace{"srv1/wide"}, fmt::format("part_{}", i));

    publishPart(store, "srv1/quiet", "only", "quiet-payload");

    publishPart(store, "srv1/gone", "a", "gone-payload-a");
    publishPart(store, "srv1/gone", "b", "gone-payload-b");
    store->dropRef(RootNamespace{"srv1/gone"}, "a");
    store->dropRef(RootNamespace{"srv1/gone"}, "b");

    store->renewWatermarkOnce();
}

/// TWO POOLS ARE NOT BYTE-COMPARABLE UNTIL THEIR IDENTITIES ARE MAPPED. A namespace's catalog
/// incarnation is minted from the process RNG at creation, and it appears BOTH inside every one of that
/// namespace's object keys and inside the fold seal's `life` rows -- so two independently created pools
/// running the identical workload produce identical decisions under different names, and the seal's
/// `ref_life` rows come out in a different order because they are keyed by that random id.
///
/// Neither fact has anything to do with the read-ahead, and hiding them by weakening the comparison
/// would hide the read-ahead's own defects too. So the identities are MAPPED instead of dropped: each
/// run reports its own incarnation-hex -> namespace-name table, every 32-hex id in a key or a seal is
/// rewritten to the namespace it names, and the `ref_life` rows are sorted once their names are stable.
/// What survives the rewrite is everything the fold decided; what it removes is only the naming.
using IdNames = std::map<String, String>;

String normalizeIds(const String & text, const IdNames & names)
{
    String out = text;
    for (const auto & [hex, name] : names)
    {
        size_t at = 0;
        while ((at = out.find(hex, at)) != String::npos)
        {
            out.replace(at, hex.size(), name);
            at += name.size();
        }
    }
    return out;
}

/// The seal with its ids named and its `ref_life` rows sorted; every other row keeps its position.
String canonicalSeal(const String & seal, const IdNames & names)
{
    const String named = normalizeIds(seal, names);
    std::vector<String> out;
    std::vector<String> lives;
    size_t pos = 0;
    while (pos <= named.size())
    {
        const size_t nl = named.find('\n', pos);
        const String line = named.substr(pos, nl == String::npos ? String::npos : nl - pos);
        if (line.find("\"kind\":\"ref_life\"") != String::npos)
        {
            lives.push_back(line);
        }
        else
        {
            if (!lives.empty())
            {
                std::sort(lives.begin(), lives.end());
                out.insert(out.end(), lives.begin(), lives.end());
                lives.clear();
            }
            out.push_back(line);
        }
        if (nl == String::npos)
            break;
        pos = nl + 1;
    }
    std::sort(lives.begin(), lives.end());
    out.insert(out.end(), lives.begin(), lives.end());

    String joined;
    for (const String & line : out)
    {
        joined += line;
        joined += '\n';
    }
    return joined;
}

struct FoldRun
{
    std::vector<String> seals;                        /// the fold seal's bytes after each round
    std::vector<std::map<String, UInt64>> intake;     /// `fold_ref_intake` metrics, per round
    std::vector<std::map<String, UInt64>> reduce;     /// `fold_reduce` metrics, per round
    std::map<String, uint64_t> gets;                  /// key -> GETs over the whole run
    std::map<String, uint64_t> heads;                 /// key -> HEADs over the whole run
    std::vector<size_t> condemned;
    std::vector<uint64_t> deleted;
    IdNames id_names;                                 /// incarnation hex -> namespace, for the comparison
};

void runFolds(uint64_t concurrency, size_t rounds, FoldRun & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = concurrency});
    populate(store);

    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const Layout & layout = store->layout();

    /// Read the id table BEFORE the rounds: a namespace the fold reclaims loses its catalog row, and its
    /// keys still have to be nameable when the two runs are compared.
    for (const CatalogEntry & entry : CasRefCatalog::read(op, layout).catalog.entries)
        out.id_names.emplace(u128ToHex(entry.incarnation), "<" + entry.ns.string() + ">");

    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & rec)
    {
        if (rec.phase == "fold_ref_intake")
            out.intake.push_back(rec.metrics);
        else if (rec.phase == "fold_reduce")
            out.reduce.push_back(rec.metrics);
    });

    /// Only the rounds' own I/O is compared; the identical population above is not part of the claim.
    backend->resetCounts();

    for (size_t round = 0; round < rounds; ++round)
    {
        const RoundReport report = gc.runRegularRound();
        ASSERT_TRUE(report.acquired_lease) << "round " << round;
        out.condemned.push_back(report.condemned);
        out.deleted.push_back(report.deleted);
        store->renewWatermarkOnce();

        const GcState st = decodeGcState(op.read(layout.gcStateKey(), Retry::once())->bytes);
        const auto seal = op.read(layout.foldSealKey(st.snap_generation, st.snap_attempt), Retry::once());
        out.seals.push_back(seal ? canonicalSeal(seal->bytes, out.id_names) : String{});
    }

    for (const String & key : backend->touchedKeys())
    {
        const String named = normalizeIds(key, out.id_names);
        if (const uint64_t n = backend->getCount(key); n != 0)
            out.gets[named] += n;
        if (const uint64_t n = backend->headCount(key); n != 0)
            out.heads[named] += n;
    }
}

}

TEST(CASGCReadAhead, FoldIsIdenticalAtConcurrencyOneAndEight)
{
    constexpr size_t kRounds = 6;
    FoldRun one;
    FoldRun eight;
    ASSERT_NO_FATAL_FAILURE(runFolds(1, kRounds, one));
    ASSERT_NO_FATAL_FAILURE(runFolds(8, kRounds, eight));

    ASSERT_EQ(one.seals.size(), kRounds);
    ASSERT_EQ(eight.seals.size(), kRounds);
    for (size_t i = 0; i < kRounds; ++i)
        EXPECT_EQ(one.seals[i], eight.seals[i]) << "fold seal differs at round " << i;

    EXPECT_EQ(one.intake, eight.intake);
    EXPECT_EQ(one.reduce, eight.reduce);
    EXPECT_EQ(one.condemned, eight.condemned);
    EXPECT_EQ(one.deleted, eight.deleted);

    /// THE REQUEST-SET CLAIM. Every namespace here is healthy, so nothing is hinted that the walk
    /// does not go on to read: the hints stop at the checkpoint ceiling the walk stops at, a quiet
    /// namespace is not hinted at all, and every decoded log's manifest edges are all folded. So the
    /// read-ahead must issue the SAME GETs against the SAME keys, not merely produce the same answer.
    EXPECT_EQ(one.gets, eight.gets);

    ASSERT_FALSE(one.intake.empty());
    EXPECT_GT(one.intake[0].at("logs_applied"), 32u)
        << "the wide namespace must carry more logs than the window, or the lookahead is untested";
}

TEST(CASGCReadAhead, ReduceCondemnsTheSameBlobsWithTheSameHeadsAtConcurrencyOneAndEight)
{
    /// `populate` gives every part its own blob and drops whole parts, so a dropped blob loses its only
    /// edge and no surviving blob has a removal: the hinted set equals the set `head_blob` takes, and the
    /// per-key HEAD counts must match exactly rather than merely producing the same verdict.
    constexpr size_t kRounds = 6;
    FoldRun one;
    FoldRun eight;
    ASSERT_NO_FATAL_FAILURE(runFolds(1, kRounds, one));
    ASSERT_NO_FATAL_FAILURE(runFolds(8, kRounds, eight));

    EXPECT_EQ(one.condemned, eight.condemned);
    EXPECT_EQ(one.heads, eight.heads);

    uint64_t condemned_total = 0;
    for (const size_t n : one.condemned)
        condemned_total += n;
    EXPECT_GT(condemned_total, 0u) << "the scenario must condemn, or the reduce read-ahead is untested";
}

namespace
{

/// Throws once on the first read issued from a thread other than the one that armed it: exactly a
/// read-ahead worker's request, never the round thread's own.
class WorkerReadFaultBackend : public CountingBackend
{
public:
    void armAgainstOtherThreads()
    {
        owner = std::this_thread::get_id();
        armed.store(true);
    }

    bool fired() const { return !armed.load(); }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (armed.load() && std::this_thread::get_id() != owner)
        {
            armed.store(false);
            throw std::runtime_error("injected worker read fault");
        }
        return CountingBackend::read(key, access);
    }

private:
    std::atomic<bool> armed{false};
    std::thread::id owner;
};

}

namespace
{

/// Proves OVERLAP, which no equality test can: it releases a read only once `k_overlap` reads are
/// inside the backend at the same time. If the fold's reads were still strictly one after another the
/// count could never reach two, so the round would block until the bounded wait expires and the flag
/// below would stay false. The wait is bounded and the last arrival wakes everyone, so nothing here can
/// hang the suite: a fold with no overlap finishes late, it does not finish never.
class OverlapWitnessBackend : public CountingBackend
{
public:
    explicit OverlapWitnessBackend(size_t k_overlap_) : k_overlap(k_overlap_) {}

    /// ARMED ONLY FOR THE ROUND. Holding reads is fatal to a WRITER: its checkpoint publication is a
    /// CAS with a bounded retry budget, and a latch on every read exhausts it long before the round
    /// under test ever starts.
    void arm() { armed.store(true); }

    bool sawOverlap() const { return saw_overlap.load(); }

    std::optional<DB::Cas::Backend::Raw> read(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (!armed.load())
            return CountingBackend::read(key, access);
        {
            std::unique_lock lock(mutex);
            ++in_flight;
            peak = std::max(peak, in_flight);
            if (in_flight >= k_overlap)
            {
                saw_overlap.store(true);
                gate.notify_all();
            }
            else
            {
                gate.wait_for(lock, std::chrono::milliseconds(250),
                              [&] { return in_flight >= k_overlap || saw_overlap.load(); });
            }
        }
        /// The count stays raised ACROSS the read, so what it measures is requests genuinely in the
        /// backend together. Releasing it before the read would leave a window of a few instructions
        /// that two threads would have to hit simultaneously to be seen -- which is a measurement of
        /// luck, not of overlap.
        std::optional<DB::Cas::Backend::Raw> raw = CountingBackend::read(key, access);
        {
            std::lock_guard lock(mutex);
            --in_flight;
        }
        return raw;
    }

    size_t peakInFlight() const
    {
        std::lock_guard lock(mutex);
        return peak;
    }

private:
    const size_t k_overlap;
    std::atomic<bool> armed{false};
    mutable std::mutex mutex;
    std::condition_variable gate;
    size_t in_flight = 0;
    size_t peak = 0;
    std::atomic<bool> saw_overlap{false};
};

}

TEST(CASGCReadAhead, TheFoldsReadsActuallyOverlap)
{
    auto backend = std::make_shared<OverlapWitnessBackend>(/*k_overlap*/ 2);
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 8});
    populate(store);

    Gc gc(store, kGc);
    backend->arm();
    ASSERT_TRUE(gc.runRegularRound().acquired_lease);

    EXPECT_TRUE(backend->sawOverlap())
        << "no two of the fold's reads were ever in the backend at the same time; peak in flight was "
        << backend->peakInFlight();
    EXPECT_GT(backend->peakInFlight(), 1u);
}

TEST(CASGCReadAhead, WorkerReadFaultFailsTheRoundAndTheNextRoundRecovers)
{
    auto backend = std::make_shared<WorkerReadFaultBackend>();
    auto store = Pool::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                   .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 8});
    populate(store);

    Gc gc(store, kGc);
    backend->armAgainstOtherThreads();
    EXPECT_ANY_THROW(gc.runRegularRound());
    EXPECT_TRUE(backend->fired()) << "no read-ahead worker ever issued a request";

    store->renewWatermarkOnce();
    const RoundReport recovered = gc.runRegularRound();
    EXPECT_TRUE(recovered.acquired_lease);
}

namespace
{

/// What one graduation round read and wrote for a cohort of ten condemned blobs.
struct GraduationRoundCounts
{
    uint64_t graduated = 0;
    uint64_t gets = 0;      /// `.meta` GETs of the cohort: the gate's and the retries'
    uint64_t writes = 0;    /// `.meta` writes of the cohort: the retries'
    uint64_t wasted = 0;
    std::map<String, uint64_t> gets_by_key;
};

enum class CohortTwist
{
    None,
    TwoClean,           /// the two smallest refs read `Clean` at the gate
    OneReReferenced,    /// the smallest ref gains an owner before the round
    Suppressed,         /// the round's universe is not authoritative
};

/// Condemns ten blobs, rebuilds the `Gc` so nothing in process stands in for a read, applies `twist`, and
/// measures the round due to graduate them under a graduation budget of three.
void measureGraduationRound(uint64_t concurrency, CohortTwist twist, GraduationRoundCounts & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 3);
    const Layout & layout = store->layout();
    std::vector<UInt128> hashes;
    for (uint64_t i = 1; i <= 10; ++i)
        hashes.push_back(DB::UInt128(0x5000 + i));
    {
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, hashes, c));
    }

    std::vector<BlobRef> refs;
    for (const UInt128 & hash : hashes)
        refs.push_back(gateRef(hash));
    std::sort(refs.begin(), refs.end());
    if (twist == CohortTwist::TwoClean)
        for (size_t i = 0; i < 2; ++i)
            ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, refs[i], cleanMarker()));
    if (twist == CohortTwist::OneReReferenced)
        publishBlobs(*backend, layout, "again", 2, {refs[0].digest.toU128()});
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    backend->resetCounts();
    const uint64_t wasted_before = readAheadWasted();
    const RoundReport report = runGateRound(store, gc,
        twist == CohortTwist::Suppressed ? UniversePolicy::StageA_Suppressed : UniversePolicy::Authoritative);

    out.graduated = report.graduated;
    out.wasted = readAheadWasted() - wasted_before;
    for (const BlobRef & ref : refs)
    {
        const String key = layout.blobMetaKey(ref);
        out.gets += backend->getCount(key);
        out.writes += backend->writeCount(key);
        out.gets_by_key[key] = backend->getCount(key);
    }
}

}

TEST(CASGCReadAhead, GraduationReadAheadRequestCounts)
{
    struct Expected
    {
        CohortTwist twist;
        uint64_t graduated;
        uint64_t gets;
        uint64_t writes;
    };
    for (const Expected & expected : {Expected{CohortTwist::None, 3, 3, 0},
                                      Expected{CohortTwist::TwoClean, 3, 7, 2},
                                      Expected{CohortTwist::OneReReferenced, 3, 3, 0},
                                      Expected{CohortTwist::Suppressed, 0, 0, 0}})
    {
        SCOPED_TRACE(static_cast<int>(expected.twist));
        GraduationRoundCounts sequential;
        GraduationRoundCounts ahead;
        ASSERT_NO_FATAL_FAILURE(measureGraduationRound(1, expected.twist, sequential));
        ASSERT_NO_FATAL_FAILURE(measureGraduationRound(8, expected.twist, ahead));

        EXPECT_EQ(ahead.graduated, expected.graduated);
        EXPECT_EQ(ahead.gets, expected.gets) << "5 gate reads and 2 retry reads when two refuse; 3 otherwise";
        EXPECT_EQ(ahead.writes, expected.writes);
        EXPECT_EQ(ahead.wasted, 0u);
        EXPECT_EQ(ahead.gets_by_key, sequential.gets_by_key);
        EXPECT_EQ(ahead.graduated, sequential.graduated);
        EXPECT_EQ(ahead.writes, sequential.writes);
    }
}

namespace
{

/// Journals every GET in arrival order, so a test can place the gate's marker read after the round's cut.
class GetJournalBackend : public CountingBackend
{
public:
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        {
            std::lock_guard lock(journal_mutex);
            journal.push_back(key);
        }
        return CountingBackend::read(key, access);
    }

    std::vector<String> takeJournal()
    {
        std::lock_guard lock(journal_mutex);
        return std::exchange(journal, {});
    }

private:
    std::mutex journal_mutex;
    std::vector<String> journal;
};

/// Once armed, rewrites `key` with `replacement` right after a GET of it returns, with no backend lock held:
/// the value the reader got is stale by the time it arrives.
class ReadCompletionHookBackend : public CountingBackend
{
public:
    void armAfterRead(const String & key, String replacement)
    {
        std::lock_guard lock(hook_mutex);
        hooked_key = key;
        replacement_bytes = std::move(replacement);
        armed = true;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto raw = CountingBackend::read(key, access);
        std::optional<String> rewrite;
        {
            std::lock_guard lock(hook_mutex);
            if (armed && raw && key == hooked_key)
            {
                armed = false;
                rewrite = replacement_bytes;
            }
        }
        if (rewrite)
            (void)InMemoryBackend::write(key, *rewrite, raw->value, access);
        return raw;
    }

private:
    std::mutex hook_mutex;
    String hooked_key;
    String replacement_bytes;
    bool armed = false;
};

}

TEST(CASGCReadAhead, GraduationReadAheadReadsEachRound)
{
    auto backend = std::make_shared<GetJournalBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();
    const UInt128 hash = DB::UInt128(0x3001);
    const BlobRef x = gateRef(hash);
    const String meta_key = layout.blobMetaKey(x);
    Gc gc(store, kGc);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));
    ASSERT_NO_FATAL_FAILURE(setMarker(*backend, layout, x, cleanMarker()));

    /// An unrelated publication gives each measured round ref-log reads to order the gate's read against.
    uint64_t noise = 100;
    const auto roundAfterPublication = [&]
    {
        ++noise;
        publishBlobs(*backend, layout, "noise" + std::to_string(noise), noise, {DB::UInt128(0x3100 + noise)});
        store->renewWatermarkOnce();
        backend->resetCounts();
        backend->takeJournal();
        const RoundReport report = runGateRound(store, gc);
        return std::make_pair(report, backend->takeJournal());
    };
    const auto expectReadAfterCut = [&](const std::vector<String> & journal)
    {
        const auto meta_at = std::find(journal.begin(), journal.end(), meta_key);
        ASSERT_NE(meta_at, journal.end());
        const auto last_log = std::find_if(journal.rbegin(), journal.rend(),
                                           [](const String & key) { return key.contains("/_log/"); });
        ASSERT_NE(last_log, journal.rend()) << "the round read no ref log";
        const size_t last_log_at = static_cast<size_t>(journal.rend() - last_log) - 1;
        EXPECT_LT(last_log_at, static_cast<size_t>(meta_at - journal.begin()))
            << "the gate read the marker before its round's cut";
    };

    const auto [refused, refused_journal] = roundAfterPublication();
    EXPECT_EQ(refused.graduated, 0u);
    EXPECT_EQ(backend->getCount(meta_key), 2u) << "the gate's read and the retry's read";
    EXPECT_EQ(backend->writeCount(meta_key), 1u);
    expectReadAfterCut(refused_journal);
    const auto marker = markerOf(*backend, layout, x);
    ASSERT_TRUE(marker.has_value());
    EXPECT_EQ(marker->meta.condemn_round, c + 1);

    const auto [accepted, accepted_journal] = roundAfterPublication();
    EXPECT_EQ(accepted.graduated, 1u);
    EXPECT_EQ(backend->getCount(meta_key), 1u) << "every round reads the marker again";
    expectReadAfterCut(accepted_journal);
}

TEST(CASGCReadAhead, GraduationReadAheadDecidesOnTheFetchedValue)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<ReadCompletionHookBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x3101);
        const BlobRef x = gateRef(hash);
        const String meta_key = layout.blobMetaKey(x);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->resetCounts();
        backend->armAfterRead(meta_key, encodeBlobMeta(cleanMarker()));
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u) << "the gate did not decide on the value it fetched";
        EXPECT_EQ(backend->getCount(meta_key), 1u);
        const auto after = markerOf(*backend, layout, x);
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->meta.state, MetaState::Clean) << "the hook never fired";
    }
}

TEST(CASGCReadAhead, GraduationReadAheadSurfacesAReadError)
{
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const UInt128 hash = DB::UInt128(0x3201);
        const String meta_key = layout.blobMetaKey(gateRef(hash));
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "tbl", 1, {hash}, c));

        backend->failNextReadWith(meta_key, std::make_exception_ptr(std::runtime_error("injected marker read fault")));
        const uint64_t anomalies_before = metaWriteAnomalies();
        const uint64_t carries_before = unconfirmedCarries();
        RoundReport report;
        ASSERT_NO_THROW(report = runGateRound(store, gc));
        EXPECT_EQ(report.graduated, 0u) << "graduated without reading the marker";
        EXPECT_EQ(metaWriteAnomalies() - anomalies_before, 1u);
        EXPECT_EQ(unconfirmedCarries() - carries_before, 1u);
        EXPECT_EQ(committedGcRound(*backend, layout), c + 1) << "a failed marker read failed the round";
        EXPECT_EQ(runGateRound(store, gc).graduated, 1u);
    }
}

namespace
{

/// Parks the first GET of a watched key until `quorum` more GETs of watched keys arrive, or a bounded wait ends.
/// Serial reads never release it; overlapping ones do.
class ParkFirstWatchedReadBackend : public CountingBackend
{
public:
    void watch(std::set<String> keys, size_t quorum_)
    {
        std::lock_guard lock(park_mutex);
        watched = std::move(keys);
        quorum = quorum_;
        armed = true;
    }

    bool timedOut() const
    {
        std::lock_guard lock(park_mutex);
        return timed_out;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        {
            std::unique_lock lock(park_mutex);
            if (armed && watched.contains(key))
            {
                if (!parked)
                {
                    parked = true;
                    if (!arrived.wait_for(lock, std::chrono::seconds(10), [&] { return arrivals >= quorum; }))
                        timed_out = true;
                    armed = false;
                }
                else
                {
                    ++arrivals;
                    arrived.notify_all();
                }
            }
        }
        return CountingBackend::read(key, access);
    }

private:
    mutable std::mutex park_mutex;
    std::condition_variable arrived;
    std::set<String> watched;
    size_t quorum = 0;
    size_t arrivals = 0;
    bool armed = false;
    bool parked = false;
    bool timed_out = false;
};

std::vector<UInt128> hashRange(uint64_t base, uint64_t count)
{
    std::vector<UInt128> out;
    for (uint64_t i = 1; i <= count; ++i)
        out.push_back(DB::UInt128(base + i));
    return out;
}

/// `.meta` GETs per key over `hashes`.
std::map<String, uint64_t> markerGets(const CountingBackend & backend, const Layout & layout, const std::vector<UInt128> & hashes)
{
    std::map<String, uint64_t> out;
    for (const UInt128 & hash : hashes)
        out[layout.blobMetaKey(gateRef(hash))] = backend.getCount(layout.blobMetaKey(gateRef(hash)));
    return out;
}

struct SupersedeRun
{
    std::vector<uint64_t> graduated;
    std::map<String, uint64_t> superseded_gets;
    uint64_t wasted = 0;
};

/// Twenty condemned blobs; before each of three rounds a writer republishes ten of them under a new token and
/// drops them again, so those ten are superseded every round.
void runSupersedeRounds(uint64_t concurrency, SupersedeRun & out)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend, concurrency);
    const Layout & layout = store->layout();
    const std::vector<UInt128> cohort = hashRange(0x6000, 20);
    const std::vector<UInt128> superseded(cohort.begin() + 10, cohort.end());
    Gc gc(store, kGc);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));

    backend->resetCounts();
    const uint64_t wasted_before = readAheadWasted();
    for (uint64_t round = 0; round < 3; ++round)
    {
        for (const UInt128 & hash : superseded)
            displaceBlobToken(*backend, layout, gateRef(hash));
        const String ref_name = "touch" + std::to_string(round);
        const ManifestRef touch = publishBlobs(*backend, layout, ref_name, 10 + round, superseded);
        dropBlobs(*backend, layout, ref_name, touch);
        store->renewWatermarkOnce();
        out.graduated.push_back(runGateRound(store, gc).graduated);
    }
    out.wasted = readAheadWasted() - wasted_before;
    out.superseded_gets = markerGets(*backend, layout, superseded);
}

}

TEST(CASGCReadAhead, GraduationReadAheadSkipsSupersededEntries)
{
    SupersedeRun sequential;
    SupersedeRun ahead;
    ASSERT_NO_FATAL_FAILURE(runSupersedeRounds(1, sequential));
    ASSERT_NO_FATAL_FAILURE(runSupersedeRounds(8, ahead));

    EXPECT_EQ(ahead.superseded_gets, sequential.superseded_gets) << "a superseded entry's marker was read ahead";
    EXPECT_EQ(ahead.wasted, 0u);
    EXPECT_EQ(ahead.graduated, sequential.graduated);
    EXPECT_EQ(sequential.graduated.front(), 10u);
}

TEST(CASGCReadAhead, GraduationReadAheadIgnoresUnusedHeadHints)
{
    auto backend = std::make_shared<ParkFirstWatchedReadBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();

    /// Roles by key order: the smallest blob is condemned fresh in the measured round, so its HEAD tops the head
    /// hints up with `kept` (which keep an edge, so nobody takes them) before any graduation candidate settles.
    std::vector<BlobRef> refs;
    for (const UInt128 & hash : hashRange(0x7000, 61))
        refs.push_back(gateRef(hash));
    std::sort(refs.begin(), refs.end());
    const auto hashesOf = [&](size_t from, size_t to)
    {
        std::vector<UInt128> out;
        for (size_t i = from; i < to; ++i)
            out.push_back(refs[i].digest.toU128());
        return out;
    };
    const std::vector<UInt128> fresh = hashesOf(0, 1);
    const std::vector<UInt128> graduating = hashesOf(1, 21);
    const std::vector<UInt128> kept = hashesOf(21, 61);

    const ManifestRef kept_a = publishBlobs(*backend, layout, "kept_a", 1, kept);
    publishBlobs(*backend, layout, "kept_b", 2, kept);
    {
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "graduating", 3, graduating, c));
    }
    const ManifestRef fresh_manifest = publishBlobs(*backend, layout, "fresh", 4, fresh);
    dropBlobs(*backend, layout, "fresh", fresh_manifest);
    dropBlobs(*backend, layout, "kept_a", kept_a);
    store->renewWatermarkOnce();

    std::set<String> graduation_keys;
    for (const UInt128 & hash : graduating)
        graduation_keys.insert(layout.blobMetaKey(gateRef(hash)));
    backend->watch(graduation_keys, /*quorum*/ 8);

    Gc gc(store, kGc);
    const uint64_t wasted_before = readAheadWasted();
    const RoundReport report = runGateRound(store, gc);
    EXPECT_FALSE(backend->timedOut()) << "the graduation reads never overlapped";
    EXPECT_EQ(report.graduated, 20u);
    EXPECT_EQ(report.condemned, 1u);
    EXPECT_GE(readAheadWasted() - wasted_before, 31u) << "the fixture must leave the head window full of untaken hints";
}

TEST(CASGCReadAhead, GraduationReadAheadAcrossShards)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 10, {}, /*gc_shards*/ 4);
        const Layout & layout = store->layout();
        /// The shard is chosen by the digest's high 64 bits, so spread the cohort over them.
        std::vector<UInt128> cohort = hashRange(0x8000, 40);
        for (size_t i = 0; i < cohort.size(); ++i)
            cohort[i] = (UInt128(i) << 64) + cohort[i];
        std::set<uint64_t> shards;
        for (const UInt128 & hash : cohort)
            shards.insert(blobShard(gateRef(hash), 4));
        ASSERT_GT(shards.size(), 1u) << "the cohort must span shards";
        {
            Gc gc(store, kGc);
            uint64_t c = 0;
            ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));
        }

        Gc gc(store, kGc);
        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 10u);
        const auto by_key = markerGets(*backend, layout, cohort);
        uint64_t total = 0;
        for (const auto & [key, n] : by_key)
            total += n;
        EXPECT_EQ(total, 10u) << "the shards together read past the shared budget";
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

TEST(CASGCReadAhead, GraduationReadAheadReadsALargeCohortOnce)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const std::vector<UInt128> cohort = hashRange(0x8800, 100);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));

        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        EXPECT_EQ(runGateRound(store, gc).graduated, 100u);
        const auto by_key = markerGets(*backend, layout, cohort);
        for (const auto & [key, n] : by_key)
            EXPECT_EQ(n, 1u) << key;
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u);
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

namespace
{

/// Samples, at the start of every watched read, how many jobs the process's thread pools hold queued or
/// running beyond `baseline`: the read-ahead's outstanding hints. The pool has fewer threads than the window,
/// so requests inside the backend cannot show the bound; each read takes `latency` so the scan has issued
/// its hints before the first reads finish.
class OutstandingHintsBackend : public CountingBackend
{
public:
    void watch(std::set<String> keys, std::chrono::microseconds latency_)
    {
        std::lock_guard lock(sample_mutex);
        watched = std::move(keys);
        latency = latency_;
        baseline = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled);
        armed = true;
    }

    int64_t peakOutstanding() const
    {
        std::lock_guard lock(sample_mutex);
        return peak;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::chrono::microseconds delay{0};
        {
            std::lock_guard lock(sample_mutex);
            if (armed && watched.contains(key))
            {
                peak = std::max<int64_t>(peak, CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled) - baseline);
                delay = latency;
            }
        }
        std::this_thread::sleep_for(delay);
        return CountingBackend::read(key, access);
    }

private:
    mutable std::mutex sample_mutex;
    std::set<String> watched;
    std::chrono::microseconds latency{0};
    int64_t baseline = 0;
    int64_t peak = 0;
    bool armed = false;
};

}

TEST(CASGCReadAhead, GraduationReadAheadHintsAtMostAWindowAhead)
{
    auto backend = std::make_shared<OutstandingHintsBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();
    const std::vector<UInt128> cohort = hashRange(0x8900, 200);
    Gc gc(store, kGc);
    uint64_t c = 0;
    ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));

    std::set<String> meta_keys;
    for (const UInt128 & hash : cohort)
        meta_keys.insert(layout.blobMetaKey(gateRef(hash)));
    backend->watch(meta_keys, std::chrono::milliseconds(2));

    const uint64_t io_concurrency = 8;
    const uint64_t window = 4 * io_concurrency;
    EXPECT_EQ(runGateRound(store, gc).graduated, cohort.size());
    EXPECT_GT(backend->peakOutstanding(), 1) << "no read was ever hinted ahead of its gate";
    /// A worker still counts as scheduled between its set_value and its exit, after take() freed the slot and the
    /// round hinted a replacement, so the sample may run one past the window per worker.
    EXPECT_LE(backend->peakOutstanding(), static_cast<int64_t>(window + io_concurrency)) << "hints ran past the read-ahead window";
}

TEST(CASGCReadAhead, GraduationReadAheadNeverHintsPendingRows)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency);
        const Layout & layout = store->layout();
        const std::vector<UInt128> eligible = hashRange(0x9100, 10);
        const std::vector<UInt128> pending = hashRange(0x9200, 10);
        const ManifestRef eligible_manifest = publishBlobs(*backend, layout, "eligible", 1, eligible);
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "pending", 2, pending, c));
        dropBlobs(*backend, layout, "eligible", eligible_manifest);
        store->renewWatermarkOnce();
        const RoundReport setup = runGateRound(store, gc);
        ASSERT_EQ(setup.graduated, 10u);
        ASSERT_EQ(setup.condemned, 10u);

        backend->resetCounts();
        const uint64_t wasted_before = readAheadWasted();
        const RoundReport report = runGateRound(store, gc);
        EXPECT_EQ(report.redeleted, 10u);
        EXPECT_EQ(report.graduated, 10u);
        EXPECT_EQ(readAheadWasted() - wasted_before, 0u) << "a pending row was hinted and never taken";
        auto by_key = markerGets(*backend, layout, pending);
        by_key.merge(markerGets(*backend, layout, eligible));
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

TEST(CASGCReadAhead, GraduationReadAheadTouchedRowKeepsItsBudgetSlot)
{
    std::vector<std::map<String, uint64_t>> gets;
    for (const uint64_t concurrency : std::initializer_list<uint64_t>{1, 8})
    {
        SCOPED_TRACE(concurrency);
        auto backend = std::make_shared<CountingBackend>();
        auto store = openGatePool(backend, concurrency, /*graduation_budget*/ 3);
        const Layout & layout = store->layout();

        /// A, T, B, C in key order. T is published and dropped again within one round under its own token:
        /// net-zero deltas, so T is touched yet not superseded, and still reaches the gate.
        std::vector<BlobRef> refs;
        for (const UInt128 & hash : hashRange(0x9300, 4))
            refs.push_back(gateRef(hash));
        std::sort(refs.begin(), refs.end());
        std::vector<UInt128> cohort;
        for (const BlobRef & ref : refs)
            cohort.push_back(ref.digest.toU128());

        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "cohort", 1, cohort, c));
        const std::vector<UInt128> touched{cohort[1]};
        const ManifestRef touch = publishBlobs(*backend, layout, "touch", 10, touched);
        dropBlobs(*backend, layout, "touch", touch);
        store->renewWatermarkOnce();

        backend->resetCounts();
        EXPECT_EQ(runGateRound(store, gc).graduated, 3u);
        const auto by_key = markerGets(*backend, layout, cohort);
        EXPECT_EQ(by_key.at(layout.blobMetaKey(refs[0])), 1u) << "A";
        EXPECT_EQ(by_key.at(layout.blobMetaKey(refs[1])), 1u) << "T";
        EXPECT_EQ(by_key.at(layout.blobMetaKey(refs[2])), 1u) << "B";
        EXPECT_EQ(by_key.at(layout.blobMetaKey(refs[3])), 0u) << "C";
        gets.push_back(by_key);
    }
    EXPECT_EQ(gets[0], gets[1]);
}

TEST(CASGCReadAhead, OutstandingGraduationReadsDoNotStopHeadHints)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openGatePool(backend, /*io_concurrency*/ 8);
    const Layout & layout = store->layout();

    /// Key order: five graduating rows, the fresh condemn, forty `kept` blobs (head candidates nobody takes),
    /// then thirty-five more graduating rows. When the fresh HEAD is taken, graduation reads fill the window,
    /// so a head throttle that counted them would hint nothing past the fresh blob.
    std::vector<BlobRef> refs;
    for (const UInt128 & hash : hashRange(0xA000, 81))
        refs.push_back(gateRef(hash));
    std::sort(refs.begin(), refs.end());
    std::vector<UInt128> graduating;
    std::vector<UInt128> fresh;
    std::vector<UInt128> kept;
    for (size_t i = 0; i < refs.size(); ++i)
    {
        const UInt128 hash = refs[i].digest.toU128();
        (i == 5 ? fresh : (i < 46 && i > 5 ? kept : graduating)).push_back(hash);
    }

    const ManifestRef kept_a = publishBlobs(*backend, layout, "kept_a", 1, kept);
    publishBlobs(*backend, layout, "kept_b", 2, kept);
    {
        Gc gc(store, kGc);
        uint64_t c = 0;
        ASSERT_NO_FATAL_FAILURE(condemnBlobs(store, gc, *backend, "graduating", 3, graduating, c));
    }
    const ManifestRef fresh_manifest = publishBlobs(*backend, layout, "fresh", 4, fresh);
    dropBlobs(*backend, layout, "fresh", fresh_manifest);
    dropBlobs(*backend, layout, "kept_a", kept_a);
    store->renewWatermarkOnce();

    Gc gc(store, kGc);
    const uint64_t wasted_before = readAheadWasted();
    const RoundReport report = runGateRound(store, gc);
    EXPECT_EQ(report.graduated, 40u);
    EXPECT_EQ(report.condemned, 1u);
    EXPECT_GE(readAheadWasted() - wasted_before, 20u) << "outstanding graduation reads kept the head hints from being issued";
}
