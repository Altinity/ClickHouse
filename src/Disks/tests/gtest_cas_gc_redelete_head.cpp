#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"

/// The `HEAD` that precedes each pending body delete: its requests and outcomes per row, and its order against
/// the round's cut (`cut_complete`) and the round's run writes.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");

/// Records each body delete's precondition and the raw token each `HEAD` answered; fails a `HEAD` on
/// request; holds latched `HEAD`s and reads until enough of them are in flight together.
class RedeleteHeadBackend : public CountingBackend
{
public:
    /// Set before the round to return a different wire spelling of the same body token.
    std::optional<String> quoted_head_key;

    void armHeadFault(const String & key)
    {
        std::lock_guard lock(mutex);
        fault_keys.insert(key);
    }

    bool headFaultFired() const
    {
        std::lock_guard lock(mutex);
        return fault_fired;
    }

    /// A `HEAD` or a read of one of `keys` waits, bounded, until `together` of them are in flight.
    void latchHeads(std::set<String> keys, size_t together)
    {
        std::lock_guard lock(mutex);
        latch_keys = std::move(keys);
        latch_together = together;
    }

    size_t peakLatchedInFlight() const
    {
        std::lock_guard lock(mutex);
        return peak_in_flight;
    }

    /// `(key, expected token)` of every delete, in issue order.
    std::vector<std::pair<String, String>> removes() const
    {
        std::lock_guard lock(mutex);
        return remove_calls;
    }

    /// `HEAD`s of `keys` sent on the thread that built this backend, which runs the round: not read ahead.
    std::vector<String> inlineHeads(const std::set<String> & keys) const
    {
        std::lock_guard lock(mutex);
        std::vector<String> out;
        for (const String & key : inline_head_keys)
            if (keys.contains(key))
                out.push_back(key);
        return out;
    }

    std::optional<String> lastHeadValue(const String & key) const
    {
        std::lock_guard lock(mutex);
        const auto it = head_values.find(key);
        return it == head_values.end() ? std::nullopt : std::optional<String>(it->second);
    }

    void clearRecords()
    {
        clearJournal();
        std::lock_guard lock(mutex);
        remove_calls.clear();
        head_values.clear();
        inline_head_keys.clear();
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        bool latched = false;
        {
            std::unique_lock lock(mutex);
            if (fault_keys.erase(key) > 0)
            {
                fault_fired = true;
                throw std::runtime_error("injected HEAD fault for key " + key);
            }
            latched = enterLatch(lock, key);
        }
        std::optional<RawMeta> meta = CountingBackend::head(key, access);
        std::lock_guard lock(mutex);
        if (latched)
            --in_flight;
        if (meta)
        {
            if (key == quoted_head_key)
                meta->value = '"' + meta->value + '"';
            head_values[key] = meta->value;
        }
        if (std::this_thread::get_id() == owner_thread)
            inline_head_keys.push_back(key);
        return meta;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        bool latched = false;
        {
            std::unique_lock lock(mutex);
            latched = enterLatch(lock, key);
        }
        std::optional<Raw> raw = CountingBackend::read(key, access);
        std::lock_guard lock(mutex);
        if (latched)
            --in_flight;
        return raw;
    }

    std::vector<RawBatchRemoveResult> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        {
            std::lock_guard lock(mutex);
            for (const auto & removal : removals)
                remove_calls.emplace_back(removal.key, removal.expected_token);
        }
        return CountingBackend::removeManyByConditional(removals, access);
    }

private:
    bool enterLatch(std::unique_lock<std::mutex> & lock, const String & key)
    {
        if (!latch_keys.contains(key))
            return false;
        ++in_flight;
        peak_in_flight = std::max(peak_in_flight, in_flight);
        latch_cv.notify_all();
        latch_cv.wait_for(lock, std::chrono::seconds(10), [&] { return in_flight >= latch_together || latch_released; });
        if (in_flight >= latch_together)
            latch_released = true;
        return true;
    }

    mutable std::mutex mutex;
    std::condition_variable latch_cv;
    std::set<String> fault_keys;
    bool fault_fired = false;
    std::set<String> latch_keys;
    size_t latch_together = 0;
    size_t in_flight = 0;
    size_t peak_in_flight = 0;
    bool latch_released = false;
    std::vector<std::pair<String, String>> remove_calls;
    std::map<String, String> head_values;
    const std::thread::id owner_thread = std::this_thread::get_id();
    std::vector<String> inline_head_keys;
};

PoolPtr openPool(const std::shared_ptr<RedeleteHeadBackend> & backend, uint64_t concurrency, uint64_t gc_shards = 1)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = concurrency;
    config.gc_shards = gc_shards;
    return Pool::open(backend, std::move(config));
}

String blobKeyOf(const Pool & store, uint64_t blob)
{
    return store.layout().blobKey(blobRefOf(blob));
}

/// The intent key of the body now at blob `blob`'s key.
String intentKeyOf(Backend & backend, const Pool & store, uint64_t blob)
{
    return *intentKeyOfCurrentBody(backend, store.layout(), blobRefOf(blob));
}

bool exists(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).head(key, Retry::standard()).has_value();
}

/// Publishes `blobs` bodies under one committed ref, drops the ref, and runs the condemning round: every blob
/// is `pending` with its intent confirmed.
void makeAllPending(RedeleteHeadBackend & backend, const PoolPtr & store, Gc & gc, uint64_t blobs)
{
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    std::vector<ManifestEntry> entries;
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        writeBlobBody(backend, store->layout(), numberedBlobHash(b));
        entries.push_back(blobEntryFor("f" + std::to_string(b), numberedBlobHash(b)));
    }
    writeManifestRaw(backend, store->layout(), ns, r, entries);
    publishCommittedTransition(backend, store->layout(), ns, "tbl", std::nullopt, r);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(backend, store->layout(), ns, "tbl", r);

    const RoundReport condemning = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    ASSERT_EQ(condemning.bodies_deleted, 0u) << "no body may be deleted in the condemning round";
    ASSERT_EQ(condemning.intent_confirmed, blobs) << "the blobs never became pending together";
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        ASSERT_TRUE(exists(backend, blobKeyOf(*store, b))) << "blob " << b;
        ASSERT_TRUE(exists(backend, intentKeyOf(backend, *store, b))) << "intent of blob " << b;
    }
}

std::map<String, std::vector<JournalVerb>> blobVerbs(const std::vector<JournalEntry> & journal, const std::set<String> & keys)
{
    std::map<String, std::vector<JournalVerb>> out;
    for (const JournalEntry & entry : journal)
        for (const String & key : entry.keys)
            if (keys.contains(key))
                out[key].push_back(entry.verb);
    return out;
}

std::set<String> blobKeys(const Pool & store, uint64_t blobs)
{
    std::set<String> keys;
    for (uint64_t b = 1; b <= blobs; ++b)
        keys.insert(blobKeyOf(store, b));
    return keys;
}

size_t indexOfMark(const std::vector<JournalEntry> & journal, const String & label)
{
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Mark && journal[i].detail == label)
            return i;
    return journal.size();
}

/// The first write to a key containing `substr` at or after `from`; `journal.size()` when there is none.
size_t firstWriteContaining(const std::vector<JournalEntry> & journal, const String & substr, size_t from = 0)
{
    for (size_t i = from; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Write && journal[i].keys.front().find(substr) != String::npos)
            return i;
    return journal.size();
}

std::vector<size_t> headIndices(const std::vector<JournalEntry> & journal, const String & key)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Head && journal[i].keys.front() == key)
            out.push_back(i);
    return out;
}

uint64_t committedRound(Backend & backend, const Pool & store)
{
    OperationForTest op(backend);
    const auto state = (*op).read(store.layout().gcStateKey(), Retry::standard());
    return state ? decodeGcState(state->bytes).round : 0;
}

std::set<String> pendingKeys(Gc & gc)
{
    std::set<String> out;
    for (const Gc::PreviewEntry & entry : gc.previewDeletes().deletes)
        if (entry.reason == "pending")
            out.insert(entry.key);
    return out;
}

/// Three pending rows: blob 1 still holds its token, blob 2's body is gone, blob 3 holds another token.
struct MixedRound
{
    /// The intent of each blob's condemned token, blob 1 first.
    std::vector<String> intents;
    RoundReport report;
    std::vector<JournalEntry> journal;
    std::vector<std::pair<String, String>> removes;
};

void runMixedRound(RedeleteHeadBackend & backend, const PoolPtr & store, Gc & gc, MixedRound & out)
{
    ASSERT_NO_FATAL_FAILURE(makeAllPending(backend, store, gc, 3));
    for (uint64_t b = 1; b <= 3; ++b)
        out.intents.push_back(intentKeyOf(backend, *store, b));
    {
        OperationForTest op(backend);
        const auto current = (*op).head(blobKeyOf(*store, 2), Retry::standard());
        ASSERT_TRUE(current);
        ASSERT_EQ((*op).remove(blobKeyOf(*store, 2), current->etag, Retry::standard()), Removal::Removed);
    }
    displaceBlobToken(backend, store->layout(), blobRefOf(3));
    backend.clearRecords();

    out.report = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    out.journal = backend.journal();
    out.removes = backend.removes();
}

/// The pins run with the sequential setting, which reads every `HEAD` inline, and with read-ahead.
class CASGcRedeleteHeadPins : public ::testing::TestWithParam<uint64_t>
{
};

}

INSTANTIATE_TEST_SUITE_P(CASGcIo, CASGcRedeleteHeadPins, ::testing::Values(uint64_t{1}, uint64_t{4}),
    [](const ::testing::TestParamInfo<uint64_t> & param) { return param.param == 1 ? String("Sequential") : "Concurrency" + std::to_string(param.param); });

/// One round over the three pending rows of `runMixedRound`: its requests, its rows and its intents.
TEST_P(CASGcRedeleteHeadPins, RequestsPerPendingRowAreUnchanged)
{
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, GetParam());
    Gc gc(store, kGc);
    MixedRound round;
    ASSERT_NO_FATAL_FAILURE(runMixedRound(*backend, store, gc, round));

    /// Blob 1 holds its token: one delete. Blob 2 is gone: `standing`, no delete. Blob 3 holds another token:
    /// the row moves to it, no delete.
    EXPECT_EQ(round.report.bodies_deleted, 1u);
    EXPECT_EQ(round.report.deleted, 1u);
    EXPECT_EQ(round.report.absent, 0u);
    EXPECT_EQ(round.report.replaced, 0u);

    const auto verbs = blobVerbs(round.journal, blobKeys(*store, 3));
    const std::map<String, std::vector<JournalVerb>> expected{
        {blobKeyOf(*store, 1), {JournalVerb::Head, JournalVerb::Remove}},
        {blobKeyOf(*store, 2), {JournalVerb::Head}},
        {blobKeyOf(*store, 3), {JournalVerb::Head}}};
    EXPECT_EQ(verbs, expected);

    std::vector<std::pair<String, String>> blob_removes;
    for (const auto & remove : round.removes)
        if (blobKeys(*store, 3).contains(remove.first))
            blob_removes.push_back(remove);
    ASSERT_EQ(blob_removes.size(), 1u);
    EXPECT_EQ(blob_removes[0].first, blobKeyOf(*store, 1));
    EXPECT_EQ(std::optional<String>(blob_removes[0].second), backend->lastHeadValue(blobKeyOf(*store, 1)))
        << "the delete must carry the token its HEAD observed";
    EXPECT_EQ(pendingKeys(gc), (std::set<String>{blobKeyOf(*store, 3)})) << "only the moved row is pending";

    for (const uint64_t b : {1, 2})
        EXPECT_FALSE(exists(*backend, blobKeyOf(*store, b))) << "blob " << b;
    /// Only blob 3's move deletes an intent, the one of its replaced token; the bodies observed gone keep theirs.
    std::vector<String> intent_deletes;
    for (const auto & entry : round.journal)
        for (const String & key : entry.keys)
            if ((entry.verb == JournalVerb::RemoveMany || entry.verb == JournalVerb::Remove) && key.find("/blob-intents/") != String::npos)
                intent_deletes.push_back(key);
    EXPECT_EQ(intent_deletes, (std::vector<String>{round.intents[2]}));
    EXPECT_TRUE(exists(*backend, round.intents[0])) << "blob 1's intent";
    EXPECT_TRUE(exists(*backend, round.intents[1])) << "blob 2's intent";
    EXPECT_FALSE(exists(*backend, round.intents[2])) << "the intent of blob 3's replaced token";

    /// Blob 3's row moved to its new token and deleted no body.
    EXPECT_TRUE(exists(*backend, blobKeyOf(*store, 3)));
    EXPECT_TRUE(exists(*backend, intentKeyOf(*backend, *store, 3))) << "the row's new token has its intent";
    EXPECT_EQ(backend->deleteCount(blobKeyOf(*store, 3)), 0u);
}

TEST_P(CASGcRedeleteHeadPins, BodyPublishedAfterAnAbsentHeadSurvives)
{
    constexpr uint64_t blobs = 3;
    constexpr uint64_t republished = 2;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, GetParam());
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    const String key = blobKeyOf(*store, republished);
    const String old_intent = intentKeyOf(*backend, *store, republished);
    {
        OperationForTest op(*backend);
        const auto current = (*op).head(key, Retry::standard());
        ASSERT_TRUE(current);
        ASSERT_EQ((*op).remove(key, current->etag, Retry::standard()), Removal::Removed);
    }
    backend->clearRecords();

    /// A writer publishes after the fold's `HEAD` read the body absent and before the round sends its deletes.
    bool fired = false;
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase != "fold_reduce" || fired)
            return;
        fired = true;
        writeBlobBody(*backend, store->layout(), numberedBlobHash(republished));
    });

    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    gc.setPhaseSink({});

    ASSERT_TRUE(fired);
    EXPECT_EQ(rep.deleted, blobs - 1) << "positive control: the other pending rows were reclaimed";
    for (const auto & remove : backend->removes())
        EXPECT_NE(remove.first, key) << "the absent HEAD led to a body delete";
    EXPECT_TRUE(exists(*backend, old_intent)) << "the intent was removed in the round that saw its body absent";
    EXPECT_TRUE(exists(*backend, key)) << "the replacement was deleted";

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_TRUE(exists(*backend, key)) << "the replacement was deleted by the next round";
}

TEST_P(CASGcRedeleteHeadPins, UnansweredHeadKeepsTheRowForTheNextRound)
{
    constexpr uint64_t blobs = 6;
    constexpr uint64_t faulted = 3;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, GetParam());
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    const uint64_t round_before = committedRound(*backend, *store);

    backend->armHeadFault(blobKeyOf(*store, faulted));
    EXPECT_ANY_THROW(runRegularRoundReclaiming(gc));
    store->renewWatermarkOnce();

    ASSERT_TRUE(backend->headFaultFired());
    EXPECT_EQ(committedRound(*backend, *store), round_before) << "a round with an unanswered HEAD commits nothing";
    EXPECT_TRUE(exists(*backend, blobKeyOf(*store, faulted)));
    EXPECT_TRUE(exists(*backend, intentKeyOf(*backend, *store, faulted)));
    EXPECT_TRUE(pendingKeys(gc).contains(blobKeyOf(*store, faulted)));

    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.bodies_deleted, blobs);
    for (uint64_t b = 1; b <= blobs; ++b)
        EXPECT_FALSE(exists(*backend, blobKeyOf(*store, b))) << "blob " << b;
}

TEST_P(CASGcRedeleteHeadPins, SuppressedRoundSendsNoBlobRequest)
{
    constexpr uint64_t blobs = 6;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, GetParam());
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();

    const RoundReport suppressed = gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::StageA_Suppressed);
    store->renewWatermarkOnce();
    ASSERT_FALSE(suppressed.deferred) << "the suppressed round must fold, or it decides nothing";
    EXPECT_EQ(suppressed.bodies_deleted, 0u);
    EXPECT_TRUE(blobVerbs(backend->journal(), blobKeys(*store, blobs)).empty());
    EXPECT_EQ(pendingKeys(gc), blobKeys(*store, blobs)) << "a suppressed round carries every pending row";

    /// Positive control: the next unsuppressed round deletes every body.
    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.deleted, blobs);
}

TEST_P(CASGcRedeleteHeadPins, ShardedRoundSendsTheSameRequestsPerRow)
{
    constexpr uint64_t blobs = 6;
    constexpr uint64_t shards = 4;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, GetParam(), shards);
    Gc gc(store, kGc);
    std::set<uint64_t> used_shards;
    for (uint64_t b = 1; b <= blobs; ++b)
        used_shards.insert(blobShard(blobRefOf(b), shards));
    ASSERT_GT(used_shards.size(), 1u) << "the blobs must spread over several shards";
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();

    const RoundReport rep = runRegularRoundReclaiming(gc);
    EXPECT_EQ(rep.bodies_deleted, blobs);
    EXPECT_EQ(rep.deleted, blobs);

    const auto verbs = blobVerbs(backend->journal(), blobKeys(*store, blobs));
    const auto removes = backend->removes();
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        const String key = blobKeyOf(*store, b);
        const auto it = verbs.find(key);
        ASSERT_NE(it, verbs.end()) << "blob " << b;
        EXPECT_EQ(it->second, (std::vector<JournalVerb>{JournalVerb::Head, JournalVerb::Remove})) << "blob " << b;
        const auto remove = std::find_if(removes.begin(), removes.end(), [&](const auto & r) { return r.first == key; });
        ASSERT_NE(remove, removes.end()) << "blob " << b;
        EXPECT_EQ(std::optional<String>(remove->second), backend->lastHeadValue(key)) << "blob " << b;
        EXPECT_FALSE(exists(*backend, key)) << "blob " << b;
    }
}

namespace
{

/// The ordering tests run with one shard and with several.
class CASGcRedeleteHeadOrder : public ::testing::TestWithParam<uint64_t>
{
};

}

INSTANTIATE_TEST_SUITE_P(CASShards, CASGcRedeleteHeadOrder, ::testing::Values(uint64_t{1}, uint64_t{4}));

TEST_P(CASGcRedeleteHeadOrder, DeleteHeadFollowsTheCutAndPrecedesItsShardsRunWrite)
{
    constexpr uint64_t blobs = 6;
    const uint64_t shards = GetParam();
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, 4, shards);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    markCutComplete(gc, *backend);
    backend->clearRecords();

    const RoundReport rep = runRegularRoundReclaiming(gc);
    ASSERT_EQ(rep.deleted, blobs) << "positive control: the round reclaimed every pending row";

    const auto journal = backend->journal();
    const size_t cut = indexOfMark(journal, "cut_complete");
    ASSERT_LT(cut, journal.size());
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        const String shard_runs = "/blob_target/" + std::to_string(blobShard(blobRefOf(b), shards)) + "/";
        const size_t shard_run_write = firstWriteContaining(journal, shard_runs, cut);
        ASSERT_LT(shard_run_write, journal.size()) << "blob " << b << ": the round wrote no run for its shard";
        const auto heads = headIndices(journal, blobKeyOf(*store, b));
        ASSERT_EQ(heads.size(), 1u) << "blob " << b;
        EXPECT_GT(heads[0], cut) << "blob " << b << ": the delete HEAD precedes the cut";
        EXPECT_LT(heads[0], shard_run_write) << "blob " << b << ": the delete HEAD follows its shard's run write";
    }
}

TEST(CASGcRedeleteHead, PendingHeadsAreReadAhead)
{
    constexpr uint64_t blobs = 4;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, 8);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    markCutComplete(gc, *backend);
    backend->clearRecords();
    backend->latchHeads(blobKeys(*store, blobs), 2);

    const RoundReport rep = runRegularRoundReclaiming(gc);
    ASSERT_EQ(rep.deleted, blobs);
    EXPECT_GT(backend->peakLatchedInFlight(), 1u) << "the pending HEADs never overlapped";

    const auto journal = backend->journal();
    const size_t first_run_write = firstWriteContaining(journal, "/blob_target/", indexOfMark(journal, "cut_complete"));
    ASSERT_LT(first_run_write, journal.size());
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        const auto heads = headIndices(journal, blobKeyOf(*store, b));
        ASSERT_EQ(heads.size(), 1u) << "blob " << b;
        EXPECT_LT(heads[0], first_run_write) << "blob " << b << ": the HEAD did not run in the fold";
    }
}

/// The read-ahead hints no `HEAD` past the redelete budget: rows over it are carried with no request.
TEST(CASGcRedeleteHead, ReadAheadSendsNoHeadPastTheRedeleteBudget)
{
    constexpr uint64_t blobs = 6;
    constexpr uint64_t budget = 2;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = 8;
    config.gc_round_redelete_budget = budget;
    auto store = Pool::open(backend, std::move(config));
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();

    const RoundReport rep = runRegularRoundReclaiming(gc);
    EXPECT_EQ(rep.deleted, budget);
    size_t heads = 0;
    for (const auto & [key, verbs] : blobVerbs(backend->journal(), blobKeys(*store, blobs)))
        heads += std::count(verbs.begin(), verbs.end(), JournalVerb::Head);
    EXPECT_EQ(heads, budget);
    EXPECT_EQ(pendingKeys(gc).size(), blobs - budget);
}

TEST(CASGcRedeleteHead, FailedHeadFailsTheRoundBeforeAnyDelete)
{
    constexpr uint64_t blobs = 6;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();
    backend->armHeadFault(blobKeyOf(*store, 3));

    EXPECT_ANY_THROW(runRegularRoundReclaiming(gc));
    store->renewWatermarkOnce();
    ASSERT_TRUE(backend->headFaultFired());
    for (const auto & [verb_key, verbs] : blobVerbs(backend->journal(), blobKeys(*store, blobs)))
        EXPECT_EQ(std::count(verbs.begin(), verbs.end(), JournalVerb::Remove), 0) << verb_key;
    for (uint64_t b = 1; b <= blobs; ++b)
        EXPECT_TRUE(exists(*backend, blobKeyOf(*store, b))) << "blob " << b;

    /// Positive control: with the HEAD answered, the same rows delete every body.
    const RoundReport next = runRegularRoundReclaiming(gc);
    EXPECT_EQ(next.deleted, blobs);
}

namespace
{

/// Replaces blob `replaced`'s body when the round emits `phase`, on the round thread; marks the cut.
struct ReplaceAtPhase
{
    ReplaceAtPhase(RedeleteHeadBackend & backend_, const Pool & store_, String phase_, uint64_t replaced_)
        : backend(backend_), store(store_), phase(std::move(phase_)), replaced(replaced_)
    {
    }

    RedeleteHeadBackend & backend;
    const Pool & store;
    String phase;
    uint64_t replaced;
    /// The token the last `HEAD` of the blob answered before the replacement; empty when none was sent.
    std::optional<String> token_before;
    bool fired = false;

    GcPhaseSink sink()
    {
        return [this](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_ref_intake")
                backend.mark("cut_complete");
            if (record.phase != phase || fired)
                return;
            fired = true;
            token_before = backend.lastHeadValue(blobKeyOf(store, replaced));
            displaceBlobToken(backend, store.layout(), blobRefOf(replaced));
        };
    }
};

}

/// The body lands as the round's intake ends, after its ref listing. The delete `HEAD`, sent after the cut,
/// sees it, so no delete is sent; a `HEAD` sent before the cut would see the old token and send a delete.
TEST(CASGcRedeleteHead, DeleteHeadSeesABodyThatLandsDuringIntake)
{
    constexpr uint64_t blobs = 3;
    constexpr uint64_t replaced = 2;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();
    ReplaceAtPhase replace(*backend, *store, "fold_ref_intake", replaced);
    gc.setPhaseSink(replace.sink());

    const RoundReport rep = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(replace.fired);
    EXPECT_FALSE(replace.token_before.has_value()) << "the round sent the delete HEAD before its cut";
    EXPECT_EQ(rep.deleted, blobs - 1) << "positive control: the other pending rows were reclaimed";
    EXPECT_EQ(backend->deleteCount(blobKeyOf(*store, replaced)), 0u);
    EXPECT_TRUE(exists(*backend, blobKeyOf(*store, replaced)));
    EXPECT_TRUE(pendingKeys(gc).contains(blobKeyOf(*store, replaced))) << "the row moved to the landed body";
    EXPECT_TRUE(exists(*backend, intentKeyOf(*backend, *store, replaced)));
}

/// The body lands after the fold's delete `HEAD` and before the batch's delete. The delete carries the
/// token the `HEAD` observed, so the store refuses it and the new body stays.
TEST(CASGcRedeleteHead, BodyReplacedAfterTheDeleteHeadIsKeptByTheTokenCondition)
{
    constexpr uint64_t blobs = 3;
    constexpr uint64_t replaced = 2;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    const auto preview = gc.previewDeletes();
    const auto row = std::find_if(preview.deletes.begin(), preview.deletes.end(), [&](const auto & entry)
    {
        return entry.reason == "pending" && entry.ref == blobRefOf(replaced);
    });
    ASSERT_NE(row, preview.deletes.end());
    backend->quoted_head_key = blobKeyOf(*store, replaced);
    backend->clearRecords();
    ReplaceAtPhase replace(*backend, *store, "fold_reduce", replaced);
    gc.setPhaseSink(replace.sink());

    const RoundReport rep = runRegularRoundReclaiming(gc);
    ASSERT_TRUE(replace.fired);
    ASSERT_TRUE(replace.token_before.has_value()) << "the delete HEAD was not sent in the fold";
    EXPECT_NE(*replace.token_before, row->token.value) << "the fold must supply the wire token of its own HEAD";
    EXPECT_EQ(rep.deleted, blobs - 1);
    EXPECT_EQ(rep.replaced, 1u);

    const auto removes = backend->removes();
    const auto remove = std::find_if(removes.begin(), removes.end(),
        [&](const auto & r) { return r.first == blobKeyOf(*store, replaced); });
    ASSERT_NE(remove, removes.end()) << "the delete was not sent";
    EXPECT_EQ(remove->second, *replace.token_before);
    EXPECT_TRUE(exists(*backend, blobKeyOf(*store, replaced)));
}

/// The settling row's slot is counted once: with the window smaller than the redelete budget, every `HEAD`
/// within the budget goes to the pool, the first row's included.
TEST(CASGcRedeleteHead, ReadAheadFillsTheRedeleteBudget)
{
    constexpr uint64_t blobs = 12;
    constexpr uint64_t budget = 10;
    auto backend = std::make_shared<RedeleteHeadBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_io_concurrency = 2;
    config.gc_round_redelete_budget = budget;
    auto store = Pool::open(backend, std::move(config));
    Gc gc(store, kGc);
    ASSERT_NO_FATAL_FAILURE(makeAllPending(*backend, store, gc, blobs));
    backend->clearRecords();

    const RoundReport rep = runRegularRoundReclaiming(gc);
    EXPECT_EQ(rep.deleted, budget) << "positive control: the budget's rows were reclaimed";
    size_t heads = 0;
    for (const auto & [key, verbs] : blobVerbs(backend->journal(), blobKeys(*store, blobs)))
        heads += std::count(verbs.begin(), verbs.end(), JournalVerb::Head);
    EXPECT_EQ(heads, budget) << "no HEAD past the budget";
    EXPECT_EQ(backend->inlineHeads(blobKeys(*store, blobs)), std::vector<String>{})
        << "a HEAD within the budget was sent inline";
}

