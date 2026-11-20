#include <gtest/gtest.h>

#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h>
#include <Common/CurrentMetrics.h>
#include <Common/ProfileEvents.h>

#include <base/scope_guard.h>
#include <Poco/Exception.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <vector>

namespace CurrentMetrics
{
extern const Metric LocalThreadScheduled;
}

namespace ProfileEvents
{
extern const Event CASGCIntentAdopted;
extern const Event CASGCIntentConfirmedExisting;
extern const Event CASGCIntentTokenRejected;
extern const Event CASGCIntentUnconfirmedCarry;
extern const Event CASGCUndecodableBlobBody;
extern const Event CASGCIntentStandingAnomaly;
extern const Event CASGCIntentDeleted;
extern const Event CASGCIntentDeletedOnSpare;
extern const Event CASGCIntentLeftOnSpare;
extern const Event CASGCReadAheadHit;
}

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int NOT_IMPLEMENTED;
extern const int READONLY;
}

/// The deletion-intent GC driven through `Gc::runRegularRound`: a blob's row is `candidate`, `pending` or
/// `standing`; the condemning round creates the intent, a later zero cut deletes the body, the next one the
/// intent. Journals are read for requests on blob and intent keys only.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Blob ids whose rows live in `shard` of `gc_shards`, starting the search at `from`.
std::vector<uint64_t> blobsInShard(uint64_t shard, uint64_t gc_shards, size_t count, uint64_t from = 1)
{
    std::vector<uint64_t> out;
    for (uint64_t b = from; out.size() < count; ++b)
        if (blobShard(blobRefOf(b), gc_shards) == shard)
            out.push_back(b);
    return out;
}

/// The in-memory store with the seams these tests need, each off by default.
class IntentBackend : public HeadHookBackend
{
public:
    /// Tokens of blob bodies derived from the bytes, so equal bytes written again repeat a token.
    std::atomic<bool> content_tokens{false};
    /// Intent creates thrown as unanswered, never landing.
    std::atomic<bool> lose_intent_creates{false};
    /// Intent creates refused the way a read-only store refuses a write: answered, never applied.
    std::atomic<bool> refuse_intent_creates{false};
    /// Intent deletes refused.
    std::atomic<bool> refuse_intent_deletes{false};
    /// Body deletes thrown as unanswered, never applied.
    std::atomic<bool> lose_body_deletes{false};
    /// Blob `HEAD`s answered with the stored token wrapped in one pair of quotes.
    std::atomic<bool> quote_blob_tokens{false};
    /// Blob `HEAD`s answered with this token while set.
    std::optional<String> blob_head_value;
    /// Runs once before the first body delete reaches the store.
    std::function<void(const String &)> before_body_delete;
    /// Runs once before the first blob `HEAD` is answered, after the store read it.
    std::function<void(const String &)> after_first_blob_head;
    /// Runs before every write reaches the store; it may throw to refuse the write.
    std::function<void(const String &)> before_write;
    /// Runs before every body delete reaches the store; it may block or throw.
    std::function<void(const String &)> on_body_delete;
    /// Runs before every blob `HEAD` reaches the store; it may block.
    std::function<void(const String &)> on_blob_head;
    /// Runs before every intent create reaches the store; it may block or throw.
    std::function<void(const String &)> on_intent_create;
    /// Runs once before the first read of an intent key reaches the store.
    std::function<void(const String &)> before_intent_read;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key.find("/blob-intents/") != String::npos)
            if (auto hook = takeOnce(before_intent_read))
                hook(key);
        return HeadHookBackend::read(key, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        if (on_blob_head && isBody(key))
            on_blob_head(key);
        auto meta = HeadHookBackend::head(key, access);
        if (!isBody(key))
            return meta;
        if (meta && content_tokens)
            meta->value = contentValue(InMemoryBackend::read(key, access)->bytes);
        if (meta && quote_blob_tokens)
            meta->value = "\"" + meta->value + "\"";
        if (meta && blob_head_value)
            meta->value = *blob_head_value;
        if (auto hook = takeOnce(after_first_blob_head))
            hook(key);
        return meta;
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (before_write)
            before_write(key);
        if (on_intent_create && key.find("/blob-intents/") != String::npos)
            on_intent_create(key);
        if (lose_intent_creates && key.find("/blob-intents/") != String::npos)
        {
            CountingBackend::write(key + ".never", bytes, expected_value, access);
            throw Poco::TimeoutException("the intent create was never answered");
        }
        if (refuse_intent_creates && key.find("/blob-intents/") != String::npos)
        {
            CountingBackend::write(key + ".refused", bytes, expected_value, access);
            throw DB::Exception(DB::ErrorCodes::READONLY, "the store is read-only");
        }
        return HeadHookBackend::write(key, bytes, expected_value, access);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        if (!isBody(key))
            return HeadHookBackend::remove(key, expected_value, access);
        if (auto hook = takeOnce(before_body_delete))
            hook(key);
        if (on_body_delete)
            on_body_delete(key);
        if (lose_body_deletes)
        {
            CountingBackend::remove(key + ".never", expected_value, access);
            throw Poco::TimeoutException("the body delete was never answered");
        }
        const auto current = InMemoryBackend::read(key, access);
        if (!current)
            return HeadHookBackend::remove(key, expected_value, access);
        String stored_form = expected_value;
        if (quote_blob_tokens && stored_form.size() >= 2 && stored_form.front() == '"')
            stored_form = stored_form.substr(1, stored_form.size() - 2);
        if (content_tokens && stored_form == contentValue(current->bytes))
            stored_form = current->value;
        return HeadHookBackend::remove(key, stored_form, access);
    }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        if (refuse_intent_deletes)
        {
            CountingBackend::removeManyWriteOnce({}, access);
            throw std::runtime_error("the store refused the intent delete");
        }
        HeadHookBackend::removeManyWriteOnce(keys, access);
    }

private:
    /// Requests of a round arrive on several threads: clearing a once-hook is a write they must not race.
    std::function<void(const String &)> takeOnce(std::function<void(const String &)> & hook)
    {
        std::lock_guard lock(once_hook_mutex);
        return std::exchange(hook, {});
    }

    std::mutex once_hook_mutex;

    static bool isBody(const String & key) { return key.find("/blobs/") != String::npos && !key.ends_with(".meta"); }
    static String contentValue(const String & bytes) { return "c" + hexOf(bytes); }
};

struct PoolOptions
{
    uint64_t gc_shards = 1;
    uint64_t create_budget = 0;
    uint64_t redelete_budget = 0;
    uint64_t io_concurrency = 1;
    uint64_t intent_sweep_page = PoolConfig{}.intent_sweep_list_budget_keys;
    uint64_t max_defer_rounds = PoolConfig{}.gc_fold_max_defer_rounds;
};

/// One pool, one GC, one namespace. Blobs are committed under named refs and dropped by name.
template <typename B = IntentBackend>
struct IntentPool
{
    std::shared_ptr<B> backend;
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    const RootNamespace ns{"00/aa@cas@"};
    uint64_t next_ordinal = 0xA0;
    std::map<String, ManifestRef> refs;

    explicit IntentPool(std::shared_ptr<B> backend_, PoolOptions options = {}) : backend(std::move(backend_))
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
        config.gc_shards = options.gc_shards;
        config.gc_round_intent_create_budget = options.create_budget;
        config.gc_round_redelete_budget = options.redelete_budget;
        config.gc_io_concurrency = options.io_concurrency;
        config.intent_sweep_list_budget_keys = options.intent_sweep_page;
        config.gc_fold_max_defer_rounds = options.max_defer_rounds;
        store = Pool::open(backend, std::move(config));
        auto now = std::make_shared<std::atomic<uint64_t>>(0);
        store->openRequests().setNowFnForTest([now] { return now->load(); });
        store->openRequests().setSleepFnForTest([now](uint64_t ms) { now->fetch_add(ms + 1); });
        gc = std::make_unique<Gc>(store, hexToU128("00000000000000000000000000000001"));
    }

    const Layout & layout() const { return store->layout(); }
    String bodyKey(uint64_t b) const { return layout().blobKey(blobRefOf(b)); }

    void writeBodies(const std::vector<uint64_t> & blobs)
    {
        for (uint64_t b : blobs)
            writeBlobBody(*backend, layout(), numberedBlobHash(b));
    }

    void commit(const String & name, const std::vector<uint64_t> & blobs)
    {
        const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = static_cast<uint32_t>(next_ordinal++)};
        std::vector<ManifestEntry> entries;
        for (uint64_t b : blobs)
            entries.push_back(blobEntryFor("f" + std::to_string(b), numberedBlobHash(b)));
        writeManifestRaw(*backend, layout(), ns, r, entries);
        publishCommittedTransition(*backend, layout(), ns, name, std::nullopt, r);
        refs[name] = r;
    }

    void drop(const String & name)
    {
        dropRefTransition(*backend, layout(), ns, name, refs.at(name));
        refs.erase(name);
    }

    /// A reference added and removed inside one window: the key is touched and stays at zero.
    void touch(const std::vector<uint64_t> & blobs)
    {
        const String name = "touch" + std::to_string(next_ordinal);
        commit(name, blobs);
        drop(name);
    }

    RoundReport round(UniversePolicy policy = UniversePolicy::Authoritative)
    {
        const RoundReport report = gc->runRegularRound({}, /*allow_steal*/ true, policy);
        store->renewWatermarkOnce();
        return report;
    }

    std::optional<RetiredEntry> row(uint64_t b) const
    {
        for (uint64_t shard = 0; shard < store->poolMeta().gc_shards; ++shard)
            for (const RetiredEntry & e : currentRetiredSet(*backend, layout(), shard))
                if (e.ref == blobRefOf(b))
                    return e;
        return std::nullopt;
    }

    std::optional<Meta> bodyMeta(uint64_t b) const
    {
        OperationForTest op(*backend);
        return (*op).head(bodyKey(b), Retry::standard());
    }

    bool exists(const String & key) const
    {
        OperationForTest op(*backend);
        return (*op).read(key, Retry::standard()).has_value();
    }

    String intentKey(uint64_t b, const PersistedEtag & token) const { return layout().blobIntentKey(blobRefOf(b), *token.canonical()); }
    String intentKey(uint64_t b, const Etag & token) const { return layout().blobIntentKey(blobRefOf(b), *token.canonical()); }
    String currentIntentKey(uint64_t b) const { return intentKey(b, bodyMeta(b)->etag); }

    /// Bodies, one committed ref, the ref dropped: the next round is the condemning one.
    void commitThenDrop(const std::vector<uint64_t> & blobs, const String & name = "tbl")
    {
        writeBodies(blobs);
        commit(name, blobs);
        round();
        drop(name);
    }

    void makePending(const std::vector<uint64_t> & blobs)
    {
        commitThenDrop(blobs);
        const RoundReport condemning = round();
        ASSERT_EQ(condemning.intent_confirmed, blobs.size());
        for (uint64_t b : blobs)
        {
            ASSERT_TRUE(row(b) && row(b)->delete_pending && !row(b)->standing) << "blob " << b;
            ASSERT_TRUE(exists(intentKey(b, row(b)->token))) << "intent of blob " << b;
        }
    }

    void makeStanding(const std::vector<uint64_t> & blobs)
    {
        makePending(blobs);
        round();
        for (uint64_t b : blobs)
            ASSERT_TRUE(row(b) && row(b)->standing) << "blob " << b;
    }
};

IntentPool<> makePool(PoolOptions options = {})
{
    return IntentPool<>(std::make_shared<IntentBackend>(), options);
}

using Request = std::pair<JournalVerb, String>;

/// Requests on blob and intent keys, in order; a batch delete contributes one entry per key. A `LIST` names a
/// prefix, not a key, and is left out.
std::vector<Request> blobRequests(const std::vector<JournalEntry> & journal)
{
    std::vector<Request> out;
    for (const JournalEntry & entry : journal)
        for (const String & key : entry.keys)
            if (entry.verb != JournalVerb::List)
                if (key.find("/blobs/") != String::npos || key.find("/blob-intents/") != String::npos)
                    out.emplace_back(entry.verb, key);
    return out;
}

size_t countOf(const std::vector<Request> & requests, JournalVerb verb, const String & key)
{
    return static_cast<size_t>(std::count(requests.begin(), requests.end(), Request{verb, key}));
}

size_t intentDeletes(const std::vector<Request> & requests)
{
    size_t n = 0;
    for (const auto & [verb, key] : requests)
        n += (verb == JournalVerb::RemoveMany || verb == JournalVerb::Remove) && key.find("/blob-intents/") != String::npos;
    return n;
}

size_t intentCreates(const std::vector<Request> & requests)
{
    size_t n = 0;
    for (const auto & [verb, key] : requests)
        n += verb == JournalVerb::Write && key.find("/blob-intents/") != String::npos;
    return n;
}

size_t indexOfMark(const std::vector<JournalEntry> & journal, const String & label)
{
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Mark && journal[i].detail == label)
            return i;
    return journal.size();
}

uint64_t counter(ProfileEvents::Event event)
{
    return static_cast<uint64_t>(ProfileEvents::global_counters[event]);
}

uint64_t committedRound(Backend & backend, const Layout & layout)
{
    OperationForTest op(backend);
    const auto state = (*op).read(layout.gcStateKey(), Retry::standard());
    return state ? decodeGcState(state->bytes).round : 0;
}

DB::UInt128 storedLeaseOwner(Backend & backend, const Layout & layout)
{
    OperationForTest op(backend);
    return decodeGcState((*op).read(layout.gcStateKey(), Retry::standard())->bytes).lease.owner;
}

/// `blobs` in the order of their rows in a run.
std::vector<uint64_t> inRunOrder(std::vector<uint64_t> blobs)
{
    std::sort(blobs.begin(), blobs.end(), [](uint64_t a, uint64_t b) { return blobRefOf(a) < blobRefOf(b); });
    return blobs;
}

/// A gate that blocks its waiters until opened; the bounded wait only protects against a hang.
class Gate
{
public:
    void wait()
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(20), [this] { return opened; }))
            throw std::runtime_error("the test gate was never opened");
    }

    void open()
    {
        std::lock_guard lock(mutex);
        opened = true;
        cv.notify_all();
    }

    /// False when `timeout` passed with the gate closed.
    bool waitFor(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, timeout, [this] { return opened; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool opened = false;
};

}

/// ---- States ----

TEST(CASGcIntents, CondemnCreatesInTheSameRound)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    const String body = pool.bodyKey(1);
    const String intent = pool.currentIntentKey(1);
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, body}, {JournalVerb::Write, intent}}));
    EXPECT_EQ(report.condemned, 1u);
    EXPECT_EQ(report.intent_confirmed, 1u);
    EXPECT_TRUE(pool.bodyMeta(1).has_value()) << "nothing is deleted in the condemning round";
}

TEST(CASGcIntents, ConfirmedCreateCommitsPending)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    const Etag token = pool.bodyMeta(1)->etag;
    pool.round();

    const auto row = pool.row(1);
    ASSERT_TRUE(row.has_value());
    EXPECT_TRUE(row->delete_pending);
    EXPECT_FALSE(row->standing);
    EXPECT_TRUE(sameIncarnation(row->token, token));
    OperationForTest op(*pool.backend);
    const auto intent = (*op).read(pool.intentKey(1, token), Retry::standard());
    ASSERT_TRUE(intent.has_value());
    EXPECT_EQ(intent->bytes, blobIntentRecord());
}

TEST(CASGcIntents, EqualOccupantConfirms)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    {
        OperationForTest op(*pool.backend);
        (*op).create(pool.currentIntentKey(1), blobIntentRecord(), Retry::standard());
    }
    const uint64_t before = counter(ProfileEvents::CASGCIntentConfirmedExisting);

    const RoundReport report = pool.round();

    EXPECT_EQ(report.intent_confirmed, 1u);
    ASSERT_TRUE(pool.row(1));
    EXPECT_TRUE(pool.row(1)->delete_pending);
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentConfirmedExisting) - before, 1u);
}

TEST(CASGcIntents, UnansweredCreateCommitsCandidate)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->lose_intent_creates = true;
    const uint64_t before = counter(ProfileEvents::CASGCIntentUnconfirmedCarry);

    const RoundReport report = pool.round();

    EXPECT_EQ(report.condemned, 1u);
    EXPECT_EQ(report.intent_confirmed, 0u);
    ASSERT_TRUE(pool.row(1));
    EXPECT_FALSE(pool.row(1)->delete_pending) << "an unanswered create was counted as confirmed";
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentUnconfirmedCarry) - before, 1u);
    EXPECT_FALSE(pool.exists(pool.currentIntentKey(1)));
}

TEST(CASGcIntents, StoreThatRefusesIntentCreatesFailsTheRoundAtOnce)
{
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.io_concurrency = concurrency});
        const std::vector<uint64_t> blobs{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        pool.commitThenDrop(blobs);
        const uint64_t round_before = committedRound(*pool.backend, pool.layout());
        pool.backend->refuse_intent_creates = true;
        pool.backend->clearJournal();

        try
        {
            pool.round();
            ADD_FAILURE() << "the round committed over a store that refuses every intent create";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::READONLY) << e.message();
            EXPECT_NE(e.message().find("the store is read-only"), String::npos) << e.message();
        }

        const size_t creates = intentCreates(blobRequests(pool.backend->journal()));
        EXPECT_GE(creates, 1u);
        EXPECT_LE(creates, concurrency) << "the round kept creating after a definite refusal";
        EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);

        /// Positive control: the same round on a store that accepts the creates confirms every one.
        pool.backend->refuse_intent_creates = false;
        EXPECT_EQ(pool.round().intent_confirmed, blobs.size());
    }
}

TEST(CASGcIntents, CandidateRetriesTheCreate)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->lose_intent_creates = true;
    pool.round();
    ASSERT_TRUE(pool.row(1) && !pool.row(1)->delete_pending);
    pool.backend->lose_intent_creates = false;
    const String intent = pool.currentIntentKey(1);
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Write, intent), 1u);
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(1)), 0u) << "the retry needs no HEAD";
    EXPECT_EQ(report.intent_confirmed, 1u);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending);
}

TEST(CASGcIntents, PendingDeletesTheBodyOnlyOnALaterCut)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->clearJournal();
    pool.round();
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(1)), 0u)
        << "the condemning round deleted the body";
    ASSERT_TRUE(pool.bodyMeta(1).has_value());

    pool.backend->clearJournal();
    const RoundReport report = pool.round();
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(1)), 1u);
    EXPECT_EQ(report.bodies_deleted, 1u);
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

TEST(CASGcIntents, BodyDeleteCommitsStandingAndKeepsTheIntent)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const PersistedEtag token = pool.row(1)->token;
    pool.backend->clearJournal();

    pool.round();

    ASSERT_TRUE(pool.row(1));
    EXPECT_TRUE(pool.row(1)->standing);
    EXPECT_TRUE(pool.exists(pool.intentKey(1, token)));
    EXPECT_EQ(intentDeletes(blobRequests(pool.backend->journal())), 0u) << "the intent left with its body";
}

TEST(CASGcIntents, AbsentBodyAtTheDeleteCommitsStanding)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    {
        OperationForTest op(*pool.backend);
        ASSERT_EQ((*op).remove(pool.bodyKey(1), pool.bodyMeta(1)->etag, Retry::standard()), Removal::Removed);
    }
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(1)), 1u);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(1)), 0u);
    EXPECT_EQ(intentDeletes(requests), 0u);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
}

TEST(CASGcIntents, StandingRemovesTheIntentOnALaterZeroCut)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::RemoveMany, intent}})) << "the intent removal sends no HEAD";
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_FALSE(pool.exists(intent));
}

TEST(CASGcIntents, SuppressedRoundDeletesNoIntent)
{
    auto pool = makePool();
    /// Blob 1 standing and blob 2 pending at the same time.
    pool.writeBodies({1, 2});
    pool.commit("a", {1});
    pool.commit("b", {2});
    pool.round();
    pool.drop("a");
    pool.round();
    pool.drop("b");
    pool.round();
    ASSERT_TRUE(pool.row(1) && pool.row(1)->standing);
    ASSERT_TRUE(pool.row(2) && pool.row(2)->delete_pending && !pool.row(2)->standing);
    const String intent = pool.intentKey(1, pool.row(1)->token);
    pool.backend->clearJournal();

    pool.round(UniversePolicy::StageA_Suppressed);

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(intentDeletes(requests), 0u);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(2)), 0u);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
    EXPECT_TRUE(pool.row(2) && pool.row(2)->delete_pending && !pool.row(2)->standing);

    /// Positive control: the next unsuppressed round runs both.
    pool.round();
    EXPECT_FALSE(pool.exists(intent));
    EXPECT_FALSE(pool.bodyMeta(2).has_value());
}

/// The body is absent: a writer between its `HEAD` and its intent read may still rely on the intent.
TEST(CASGcIntents, SpareDropsStandingAndLeavesAnOrphanIntent)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    pool.commit("again", {1});
    const uint64_t kept_before = counter(ProfileEvents::CASGCIntentLeftOnSpare);
    pool.backend->clearJournal();

    const RoundReport report = pool.round();
    const auto requests = blobRequests(pool.backend->journal());

    EXPECT_EQ(report.spared, 1u);
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(pool.exists(intent));
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}})) << "an absent body deletes no intent";
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentLeftOnSpare) - kept_before, 1u);
}

TEST(CASGcIntents, SpareDropsPendingAndKeepsTheIntent)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    pool.commit("again", {1});
    pool.backend->clearJournal();

    pool.round();
    const auto requests = blobRequests(pool.backend->journal());

    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(pool.exists(intent));
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}})) << "the row's own token deletes no intent";
}

TEST(CASGcIntents, StandingRowWhoseHeadShowsItsTokenIsCarried)
{
    auto pool = makePool();
    pool.backend->content_tokens = true;
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    const RetiredEntry standing = *pool.row(1);
    /// The same bytes land again, and a content-derived token repeats.
    pool.writeBodies({1});
    ASSERT_TRUE(sameIncarnation(standing.token, pool.bodyMeta(1)->etag));
    pool.touch({1});
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(1)), 1u) << "positive control: the touched key was observed";
    EXPECT_EQ(intentDeletes(requests), 0u);
    EXPECT_EQ(intentCreates(requests), 0u);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(1)), 0u);
    ASSERT_TRUE(pool.row(1));
    EXPECT_TRUE(pool.row(1)->standing);
    EXPECT_TRUE(sameIncarnation(standing.token, pool.bodyMeta(1)->etag));
    EXPECT_EQ(pool.row(1)->condemn_round, standing.condemn_round);
}

TEST(CASGcIntents, TouchedPendingRowWithAnAbsentBodyGoesStandingWithOneHead)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    {
        OperationForTest op(*pool.backend);
        ASSERT_EQ((*op).remove(pool.bodyKey(1), pool.bodyMeta(1)->etag, Retry::standard()), Removal::Removed);
    }
    pool.touch({1});
    pool.backend->clearJournal();

    pool.round();

    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 1u);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
}

TEST(CASGcIntents, IdlePoolFoldsForEachRowState)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->lose_intent_creates = true;
    pool.round();
    pool.backend->lose_intent_creates = false;
    ASSERT_TRUE(pool.row(1) && !pool.row(1)->delete_pending);

    const auto gcState = [&]
    {
        OperationForTest op(*pool.backend);
        return decodeGcState((*op).read(pool.layout().gcStateKey(), Retry::standard())->bytes);
    };
    GcState state = gcState();
    EXPECT_TRUE(pool.gc->rowDecisionDueForTest(state, state.round + 1)) << "a lone candidate must force a fold";
    pool.round();
    ASSERT_TRUE(pool.row(1) && pool.row(1)->delete_pending) << "the candidate's create did not run on an idle pool";

    pool.round();
    ASSERT_TRUE(pool.row(1) && pool.row(1)->standing);
    state = gcState();
    EXPECT_TRUE(pool.gc->rowDecisionDueForTest(state, state.round + 1)) << "a lone standing row must force a fold";
    pool.round();
    EXPECT_FALSE(pool.row(1).has_value()) << "the intent removal did not run on an idle pool";
}

/// ---- Cut order ----

TEST(CASGcIntents, DecisionHeadsAreSentAfterTheCut)
{
    /// Only above concurrency 1 is a condemn-time `HEAD` read ahead.
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.io_concurrency = concurrency});
        /// The committed part names a blob whose body is not there yet.
        pool.commit("tbl", {1});
        pool.round();
        pool.drop("tbl");
        /// The body lands at the end of intake, after the ref-log reads that made the key reach zero.
        pool.gc->setPhaseSink([&](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_ref_intake")
            {
                pool.writeBodies({1});
                pool.backend->mark("cut_complete");
            }
        });
        pool.backend->clearJournal();

        pool.round();

        const auto journal = pool.backend->journal();
        const size_t cut = indexOfMark(journal, "cut_complete");
        ASSERT_LT(cut, journal.size());
        size_t heads = 0;
        for (size_t i = 0; i < journal.size(); ++i)
            if (journal[i].verb == JournalVerb::Head && journal[i].keys.front().find("/blobs/") != String::npos)
            {
                ++heads;
                EXPECT_GT(i, cut) << "a decision HEAD was sent before the cut";
            }
        EXPECT_GE(heads, 1u);
        ASSERT_TRUE(pool.row(1)) << "the landed body was not condemned";
        EXPECT_TRUE(sameIncarnation(pool.row(1)->token, pool.bodyMeta(1)->etag));
    }
}

TEST(CASGcIntents, RowMintedInARoundGetsNoSecondDecision)
{
    /// The minting HEAD answers, then a writer replaces the body: a second decision in the same round would
    /// see another token and delete the intent the round just created.
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.io_concurrency = concurrency});
        pool.commitThenDrop({1});
        const Etag minted = pool.bodyMeta(1)->etag;
        pool.backend->after_first_blob_head = [&](const String &)
        {
            displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(1));
        };
        pool.backend->clearJournal();

        pool.round();

        const auto requests = blobRequests(pool.backend->journal());
        EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(1)), 1u);
        EXPECT_EQ(intentDeletes(requests), 0u) << "the intent of the minted token was deleted";
        EXPECT_EQ(intentCreates(requests), 1u);
        ASSERT_TRUE(pool.row(1));
        EXPECT_TRUE(sameIncarnation(pool.row(1)->token, minted));
        EXPECT_TRUE(pool.exists(pool.intentKey(1, minted)));
    }
}

/// ---- Budget and shards ----

TEST(CASGcIntents, CreatesOverTheBudgetAreCandidates)
{
    auto pool = makePool({.create_budget = 2});
    pool.commitThenDrop({1, 2, 3, 4, 5});
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.condemned, 5u);
    EXPECT_EQ(report.intent_confirmed, 2u);
    EXPECT_EQ(intentCreates(blobRequests(pool.backend->journal())), 2u);
    size_t pending = 0;
    size_t candidates = 0;
    for (uint64_t b = 1; b <= 5; ++b)
    {
        ASSERT_TRUE(pool.row(b)) << b;
        (pool.row(b)->delete_pending ? pending : candidates) += 1;
    }
    EXPECT_EQ(pending, 2u);
    EXPECT_EQ(candidates, 3u);
}

TEST(CASGcIntents, CreateBudgetIsSharedByEveryShardOfARound)
{
    auto pool = makePool({.gc_shards = 4, .create_budget = 3});
    std::vector<uint64_t> blobs;
    for (uint64_t shard = 0; shard < 4; ++shard)
        for (uint64_t b : blobsInShard(shard, 4, 2))
            blobs.push_back(b);
    pool.commitThenDrop(blobs);
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.condemned, 8u);
    EXPECT_EQ(report.intent_confirmed, 3u);
    EXPECT_EQ(intentCreates(blobRequests(pool.backend->journal())), 3u);
    size_t candidates = 0;
    for (uint64_t b : blobs)
        candidates += pool.row(b) && !pool.row(b)->delete_pending;
    EXPECT_EQ(candidates, 5u);
}

TEST(CASGcIntents, PendingHeadReadAheadKeepsAtMostAWindowOutstanding)
{
    constexpr uint64_t concurrency = 4;
    constexpr int64_t window = 4 * concurrency;
    std::vector<uint64_t> cohort;
    for (uint64_t b = 1; b <= 40; ++b)
        cohort.push_back(b);
    const std::vector<uint64_t> ordered = inRunOrder(cohort);
    auto pool = makePool({.io_concurrency = concurrency});
    ASSERT_NO_FATAL_FAILURE(pool.makePending(cohort));

    /// The first pending row's `HEAD` is scheduled before the rows behind it and waits until one of them reaches the
    /// store. Their workers wait until that `HEAD` is sent, so every job the scan scheduled is still counted there.
    std::set<String> read_ahead;
    for (size_t i = 1; i < ordered.size(); ++i)
        read_ahead.insert(pool.bodyKey(ordered[i]));
    auto workers = std::make_shared<Gate>();
    auto first_read_ahead = std::make_shared<Gate>();
    pool.backend->on_blob_head = [workers, first_read_ahead, read_ahead](const String & key)
    {
        if (!read_ahead.contains(key))
            return;
        first_read_ahead->open();
        workers->wait();
    };
    SCOPE_EXIT({ first_read_ahead->open(); workers->open(); });
    int64_t scheduled_at_cut = 0;
    std::optional<int64_t> outstanding;
    pool.gc->setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_ref_intake")
            scheduled_at_cut = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled);
    });
    pool.backend->armOnHead(pool.bodyKey(ordered.front()), [&]
    {
        first_read_ahead->wait();
        outstanding = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled) - scheduled_at_cut;
        workers->open();
    });

    const RoundReport report = pool.round();

    EXPECT_EQ(report.bodies_deleted, cohort.size()) << "positive control: every pending row was decided";
    ASSERT_TRUE(outstanding.has_value()) << "the first pending row never sent its HEAD";
    EXPECT_GE(*outstanding, 2) << "no pending HEAD was read ahead";
    EXPECT_LE(*outstanding, window) << "the read-ahead ran past its window";
}

TEST(CASGcIntents, TouchedPendingRowSpendsOneRedeleteSlot)
{
    /// A, T, B, C in run order, a budget of three. T is referenced and released within one window: touched, at
    /// zero, and decided inline. Its body is gone, so it goes `standing` and spends a slot the read-ahead must not
    /// hand to C.
    const std::vector<uint64_t> rows = inRunOrder({1, 2, 3, 4});
    std::vector<std::map<String, size_t>> heads_per_setting;
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.redelete_budget = 3, .io_concurrency = concurrency});
        ASSERT_NO_FATAL_FAILURE(pool.makePending(rows));
        pool.touch({rows[1]});
        {
            OperationForTest op(*pool.backend);
            ASSERT_EQ((*op).remove(pool.bodyKey(rows[1]), pool.bodyMeta(rows[1])->etag, Retry::standard()), Removal::Removed);
        }
        pool.backend->clearJournal();

        const RoundReport report = pool.round();

        const auto requests = blobRequests(pool.backend->journal());
        std::map<String, size_t> heads;
        for (uint64_t b : rows)
            heads[pool.bodyKey(b)] = countOf(requests, JournalVerb::Head, pool.bodyKey(b));
        EXPECT_EQ(heads[pool.bodyKey(rows[0])], 1u) << "A";
        EXPECT_EQ(heads[pool.bodyKey(rows[1])], 1u) << "T";
        EXPECT_EQ(heads[pool.bodyKey(rows[2])], 1u) << "B";
        EXPECT_EQ(heads[pool.bodyKey(rows[3])], 0u) << "C is over the budget";
        EXPECT_EQ(report.bodies_deleted, 2u);
        EXPECT_TRUE(pool.row(rows[1]) && pool.row(rows[1])->standing);
        EXPECT_TRUE(pool.row(rows[3]) && pool.row(rows[3])->delete_pending && !pool.row(rows[3])->standing);
        heads_per_setting.push_back(heads);
    }
    EXPECT_EQ(heads_per_setting[0], heads_per_setting[1]);
}

TEST(CASGcIntents, CarriedTouchedPendingRowSpendsNoRedeleteSlot)
{
    /// A, T, B, C in run order, a budget of three, T touched at zero with its body in place: T is carried and spends
    /// no slot, so C takes the third.
    const std::vector<uint64_t> rows = inRunOrder({1, 2, 3, 4});
    std::vector<std::map<String, size_t>> heads_per_setting;
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.redelete_budget = 3, .io_concurrency = concurrency});
        ASSERT_NO_FATAL_FAILURE(pool.makePending(rows));
        const PersistedEtag t = pool.row(rows[1])->token;
        pool.touch({rows[1]});
        pool.backend->clearJournal();

        const RoundReport report = pool.round();

        const auto requests = blobRequests(pool.backend->journal());
        std::map<String, size_t> heads;
        for (uint64_t b : rows)
            heads[pool.bodyKey(b)] = countOf(requests, JournalVerb::Head, pool.bodyKey(b));
        for (uint64_t b : rows)
            EXPECT_EQ(heads[pool.bodyKey(b)], 1u) << "blob " << b;
        EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(rows[1])), 0u) << "T";
        EXPECT_EQ(report.bodies_deleted, 3u);
        ASSERT_TRUE(pool.row(rows[1]).has_value());
        EXPECT_TRUE(pool.row(rows[1])->delete_pending && !pool.row(rows[1])->standing);
        EXPECT_EQ(pool.row(rows[1])->token.canonical(), t.canonical());
        EXPECT_TRUE(pool.row(rows[3]) && pool.row(rows[3])->standing) << "C";
        heads_per_setting.push_back(heads);
    }
    EXPECT_EQ(heads_per_setting[0], heads_per_setting[1]);
}

TEST(CASGcIntents, CandidatesBehindCappedPendingRowsStillCreate)
{
    const std::vector<uint64_t> ordered = inRunOrder({1, 2, 3, 4, 5, 6, 7, 8});
    const std::vector<uint64_t> pending(ordered.begin(), ordered.begin() + 5);
    const std::vector<uint64_t> candidates(ordered.begin() + 5, ordered.end());
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.redelete_budget = 2, .io_concurrency = concurrency});
        pool.writeBodies(ordered);
        pool.commit("pending", pending);
        pool.commit("candidates", candidates);
        pool.round();
        pool.drop("pending");
        pool.round();
        /// A suppressed round condemns the candidates and deletes nothing; their creates are lost.
        pool.drop("candidates");
        pool.backend->lose_intent_creates = true;
        pool.round(UniversePolicy::StageA_Suppressed);
        pool.backend->lose_intent_creates = false;
        for (uint64_t b : pending)
            ASSERT_TRUE(pool.row(b) && pool.row(b)->delete_pending && !pool.row(b)->standing) << b;
        for (uint64_t b : candidates)
            ASSERT_TRUE(pool.row(b) && !pool.row(b)->delete_pending) << b;
        pool.backend->clearJournal();

        const RoundReport report = pool.round();

        const auto requests = blobRequests(pool.backend->journal());
        size_t body_deletes = 0;
        for (uint64_t b : pending)
            body_deletes += countOf(requests, JournalVerb::Remove, pool.bodyKey(b));
        EXPECT_EQ(body_deletes, 2u) << "the redelete budget caps the body deletes";
        EXPECT_EQ(intentCreates(requests), candidates.size());
        EXPECT_EQ(report.intent_confirmed, candidates.size());
        for (uint64_t b : candidates)
            EXPECT_TRUE(pool.row(b) && pool.row(b)->delete_pending) << "candidate " << b << " made no progress";
    }
}

TEST(CASGcIntents, CreatesOfARoundRunConcurrentlyWithinTheIoBound)
{
    constexpr uint64_t concurrency = 4;
    std::vector<uint64_t> blobs;
    for (uint64_t b = 1; b <= 8; ++b)
        blobs.push_back(b);
    auto pool = makePool({.io_concurrency = concurrency});
    pool.commitThenDrop(blobs);

    /// The first creates wait until `concurrency` of them are in flight; a round that sends them one at a time
    /// never gets there, and its first create goes on after the bounded wait.
    std::atomic<size_t> in_flight{0};
    std::atomic<size_t> max_in_flight{0};
    auto wave = std::make_shared<Gate>();
    pool.backend->on_intent_create = [&in_flight, &max_in_flight, wave](const String &)
    {
        const size_t now = ++in_flight;
        size_t seen = max_in_flight.load();
        while (now > seen && !max_in_flight.compare_exchange_weak(seen, now))
        {
        }
        if (now >= concurrency)
            wave->open();
        if (!wave->waitFor(std::chrono::seconds(10)))
            wave->open();
        --in_flight;
    };
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.intent_confirmed, blobs.size()) << "positive control: every create was sent and confirmed";
    EXPECT_EQ(intentCreates(blobRequests(pool.backend->journal())), blobs.size());
    EXPECT_GT(max_in_flight.load(), 1u) << "the creates of the round ran one at a time";
    EXPECT_LE(max_in_flight.load(), concurrency) << "more creates were in flight than gc_io_concurrency";
    for (uint64_t b : blobs)
        EXPECT_TRUE(pool.row(b) && pool.row(b)->delete_pending && pool.exists(pool.intentKey(b, pool.row(b)->token))) << b;
}

TEST(CASGcIntents, EachRowIsRecordedFromItsOwnCreate)
{
    /// Unanswered creates alternate with answered ones in run order; each row must follow its own answer.
    const std::vector<uint64_t> ordered = inRunOrder({1, 2, 3, 4, 5, 6});
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.io_concurrency = concurrency});
        pool.commitThenDrop(ordered);
        std::set<String> lost;
        for (size_t i = 0; i < ordered.size(); i += 2)
            lost.insert(pool.currentIntentKey(ordered[i]));
        pool.backend->on_intent_create = [lost](const String & key)
        {
            if (lost.contains(key))
                throw Poco::TimeoutException("the intent create was never answered");
        };

        const RoundReport report = pool.round();

        EXPECT_EQ(report.intent_confirmed, ordered.size() / 2);
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            ASSERT_TRUE(pool.row(ordered[i])) << i;
            EXPECT_EQ(pool.row(ordered[i])->delete_pending, i % 2 == 1) << "row " << i << " took another create's answer";
        }
    }
}

TEST(CASGcIntents, ThrowingRoundDrainsItsCreatesBeforeReturning)
{
    const std::vector<uint64_t> rows = inRunOrder({1, 2, 3});
    auto pool = makePool({.io_concurrency = 4});
    pool.commitThenDrop(rows);
    const uint64_t round_before = committedRound(*pool.backend, pool.layout());
    const String failing = pool.currentIntentKey(rows.front());
    const String holding = pool.currentIntentKey(rows.back());

    /// The first create is refused once the last one is in flight, and the last one is held. A round that returned
    /// on the refusal would leave the held create running past it.
    ManualBarrier held;
    std::atomic<bool> held_create_done{false};
    pool.backend->on_intent_create = [&](const String & key)
    {
        if (key == failing)
        {
            held.waitUntilArrived();
            throw DB::Exception(DB::ErrorCodes::READONLY, "the store is read-only");
        }
        if (key == holding)
        {
            held.arriveAndWait();
            held_create_done = true;
        }
    };

    /// The outcome is returned rather than asserted on the round's thread, where a failure is not reported.
    auto round = std::async(std::launch::async, [&]
    {
        try
        {
            pool.round();
            return false;
        }
        catch (...)
        {
            return true;
            /// Ok: a failed round is the outcome this waits for.
        }
    });
    /// Before the future's destructor, which waits for the round.
    SCOPE_EXIT({ held.release(); });
    try
    {
        held.waitUntilArrived();
    }
    catch (...) // Ok: reported as a test failure
    {
        ADD_FAILURE() << "the last create was never in flight beside the first one";
    }
    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a create was still held";
    EXPECT_FALSE(held_create_done.load());

    held.release();
    EXPECT_TRUE(round.get()) << "the refused create did not fail the round";
    EXPECT_TRUE(held_create_done.load());
    EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);
    for (uint64_t b : rows)
        EXPECT_FALSE(pool.row(b).has_value()) << b;
}

TEST(CASGcIntents, UnboundedCreateBudgetCreatesEveryCandidate)
{
    auto pool = makePool({.create_budget = 0});
    pool.commitThenDrop({1, 2, 3, 4, 5});
    const RoundReport report = pool.round();
    EXPECT_EQ(report.intent_confirmed, 5u);
}

TEST(CASGcIntents, IntentDeletesShareABatchRequest)
{
    IntentPool<BatchCapabilityBackend> pool(std::make_shared<BatchCapabilityBackend>(), {.gc_shards = 4});
    std::vector<uint64_t> blobs;
    for (uint64_t shard = 0; shard < 4; ++shard)
        blobs.push_back(blobsInShard(shard, 4, 1).front());
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding(blobs));
    pool.backend->clearJournal();

    pool.round();

    size_t batches = 0;
    for (const JournalEntry & entry : pool.backend->journal())
        if (entry.verb == JournalVerb::RemoveMany)
        {
            ++batches;
            EXPECT_EQ(entry.keys.size(), blobs.size());
        }
    EXPECT_EQ(batches, 1u);
    for (uint64_t b : blobs)
        EXPECT_FALSE(pool.row(b).has_value()) << b;
}

/// ---- Deletes ----

TEST(CASGcIntents, DeleteHeadDecidesTheRowInTheFold)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    pool.backend->clearJournal();

    pool.round();

    const auto journal = pool.backend->journal();
    size_t head = journal.size();
    size_t run_write = journal.size();
    size_t remove = journal.size();
    for (size_t i = 0; i < journal.size(); ++i)
    {
        const String & key = journal[i].keys.empty() ? String{} : journal[i].keys.front();
        if (journal[i].verb == JournalVerb::Head && key == pool.bodyKey(1) && head == journal.size())
            head = i;
        if (journal[i].verb == JournalVerb::Write && key.find("blob_target") != String::npos && run_write == journal.size())
            run_write = i;
        if (journal[i].verb == JournalVerb::Remove && key == pool.bodyKey(1))
            remove = i;
    }
    ASSERT_LT(remove, journal.size());
    EXPECT_LT(head, run_write);
    EXPECT_LT(run_write, remove) << "the run holding `standing` must be written before the body delete";
}

TEST(CASGcIntents, MismatchCommitsStanding)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    pool.backend->before_body_delete = [&](const String &) { displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(1)); };

    const RoundReport report = pool.round();

    EXPECT_EQ(report.bodies_deleted, 1u);
    EXPECT_EQ(report.replaced, 1u);
    EXPECT_TRUE(pool.bodyMeta(1).has_value()) << "the republished body was deleted";
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
}

TEST(CASGcIntents, UnansweredBodyDeleteFailsTheRoundAndKeepsPending)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const uint64_t round_before = committedRound(*pool.backend, pool.layout());
    pool.backend->lose_body_deletes = true;

    EXPECT_ANY_THROW(pool.round());

    EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending && !pool.row(1)->standing);
    pool.backend->lose_body_deletes = false;
    pool.round();
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

TEST(CASGcIntents, FailedIntentDeleteFailsTheRound)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    const uint64_t round_before = committedRound(*pool.backend, pool.layout());
    pool.backend->refuse_intent_deletes = true;

    EXPECT_ANY_THROW(pool.round());

    EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
    EXPECT_TRUE(pool.exists(intent));
}

class CASGcIntentsSharded : public ::testing::TestWithParam<uint64_t>
{
protected:
    uint64_t shards() const { return GetParam(); }
    /// The blob under test: in the last shard when sharded.
    uint64_t blob() const { return shards() == 1 ? 1 : blobsInShard(3, 4, 1).front(); }
};

TEST_P(CASGcIntentsSharded, FiveRequestsPerReclaimedBlob)
{
    auto pool = makePool({.gc_shards = shards()});
    pool.commitThenDrop({blob()});
    const String intent = pool.currentIntentKey(blob());
    pool.backend->clearJournal();
    uint64_t reclaimed = 0;
    for (int i = 0; i < 3; ++i)
        reclaimed += pool.round().bodies_deleted;
    EXPECT_EQ(reclaimed, 1u);

    const auto journal = pool.backend->journal();
    for (const JournalEntry & entry : journal)
        for (const String & key : entry.keys)
            EXPECT_FALSE(key.ends_with(".meta")) << key;
    const auto requests = blobRequests(journal);
    EXPECT_EQ(requests, (std::vector<Request>{
        {JournalVerb::Head, pool.bodyKey(blob())}, {JournalVerb::Write, intent},
        {JournalVerb::Head, pool.bodyKey(blob())}, {JournalVerb::Remove, pool.bodyKey(blob())},
        {JournalVerb::RemoveMany, intent}}));
    EXPECT_FALSE(pool.row(blob()).has_value());
    EXPECT_FALSE(pool.bodyMeta(blob()).has_value());
    EXPECT_FALSE(pool.exists(intent));
}

/// ---- Re-created key ----

TEST_P(CASGcIntentsSharded, RecreatedKeyMovesStandingToPendingInOneCommit)
{
    auto pool = makePool({.gc_shards = shards()});
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({blob()}));
    const String old_intent = pool.intentKey(blob(), pool.row(blob())->token);
    pool.writeBodies({blob()});
    const String new_intent = pool.currentIntentKey(blob());
    ASSERT_NE(new_intent, old_intent);
    pool.touch({blob()});
    const uint64_t round_before = committedRound(*pool.backend, pool.layout());
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(requests, (std::vector<Request>{
        {JournalVerb::Head, pool.bodyKey(blob())}, {JournalVerb::Write, new_intent}, {JournalVerb::RemoveMany, old_intent}}));
    EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before + 1);
    ASSERT_TRUE(pool.row(blob()));
    EXPECT_TRUE(pool.row(blob())->delete_pending);
    EXPECT_FALSE(pool.row(blob())->standing);
    EXPECT_TRUE(sameIncarnation(pool.row(blob())->token, pool.bodyMeta(blob())->etag));
}

TEST(CASGcIntents, TransitionRunsBeforeThePlainIntentDelete)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    /// A writer republishes and its part is removed again before the next round: the key's last transition.
    pool.writeBodies({1});
    pool.touch({1});

    pool.round();

    ASSERT_TRUE(pool.row(1)) << "the new incarnation was never condemned";
    EXPECT_TRUE(sameIncarnation(pool.row(1)->token, pool.bodyMeta(1)->etag));
    pool.round();
    EXPECT_FALSE(pool.bodyMeta(1).has_value()) << "the new incarnation leaked";
}

TEST_P(CASGcIntentsSharded, PendingWithAnotherTokenAtTheDeleteHeadTransitions)
{
    auto pool = makePool({.gc_shards = shards()});
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob()}));
    const String old_intent = pool.intentKey(blob(), pool.row(blob())->token);
    displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(blob()));
    const String new_intent = pool.currentIntentKey(blob());
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(requests, (std::vector<Request>{
        {JournalVerb::Head, pool.bodyKey(blob())}, {JournalVerb::Write, new_intent}, {JournalVerb::RemoveMany, old_intent}}));
    EXPECT_TRUE(pool.bodyMeta(blob()).has_value());
    ASSERT_TRUE(pool.row(blob()));
    EXPECT_TRUE(pool.row(blob())->delete_pending);
    EXPECT_TRUE(sameIncarnation(pool.row(blob())->token, pool.bodyMeta(blob())->etag));
}

INSTANTIATE_TEST_SUITE_P(CASShards, CASGcIntentsSharded, ::testing::Values(uint64_t{1}, uint64_t{4}));

TEST(CASGcIntents, RespelledTokenAtTheDeleteHeadIsTheSameIncarnation)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    pool.backend->quote_blob_tokens = true;
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(1)), 1u);
    EXPECT_EQ(intentCreates(requests), 0u) << "a respelled token was read as another incarnation";
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);
}

TEST(CASGcIntents, UncanonicalRowTokenIsCarriedAtTheDeleteHead)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const RetiredEntry pending = *pool.row(1);
    pool.backend->blob_head_value = "W/\"x\"";
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}}));
    ASSERT_TRUE(pool.row(1));
    EXPECT_TRUE(pool.row(1)->delete_pending && !pool.row(1)->standing);
    EXPECT_TRUE(pool.row(1)->token.canonical() == pending.token.canonical());
    EXPECT_EQ(pool.row(1)->condemn_round, pending.condemn_round);
}

TEST(CASGcIntents, NoIntentDeleteOnAnAbsentBodyWithoutACut)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    {
        OperationForTest op(*pool.backend);
        ASSERT_EQ((*op).remove(pool.bodyKey(1), pool.bodyMeta(1)->etag, Retry::standard()), Removal::Removed);
    }
    pool.backend->clearJournal();
    pool.round(UniversePolicy::StageA_Suppressed);
    pool.round();
    EXPECT_EQ(intentDeletes(blobRequests(pool.backend->journal())), 0u)
        << "an absent body alone deleted the intent";
    EXPECT_TRUE(pool.row(1) && pool.row(1)->standing);

    pool.round();
    EXPECT_FALSE(pool.exists(intent)) << "positive control: the next zero cut removes the intent";
}

TEST(CASGcIntents, PreviewNamesEachRowState)
{
    auto pool = makePool();
    /// Blob 1 standing, blob 2 pending, blob 3 candidate, all at once.
    pool.writeBodies({1, 2, 3});
    pool.commit("a", {1});
    pool.commit("b", {2});
    pool.commit("c", {3});
    pool.round();
    pool.drop("a");
    pool.round();
    pool.drop("b");
    pool.round();
    /// A suppressed round condemns blob 3 and decides nothing destructive for the other two.
    pool.drop("c");
    pool.backend->lose_intent_creates = true;
    pool.round(UniversePolicy::StageA_Suppressed);
    pool.backend->lose_intent_creates = false;
    ASSERT_TRUE(pool.row(1) && pool.row(1)->standing);
    ASSERT_TRUE(pool.row(2) && pool.row(2)->delete_pending && !pool.row(2)->standing);
    ASSERT_TRUE(pool.row(3) && !pool.row(3)->delete_pending);

    std::map<String, String> reasons;
    for (const Gc::PreviewEntry & e : pool.gc->previewDeletes())
        if (e.reason != "unreachable")
            reasons[e.key] = e.reason;
    EXPECT_EQ(reasons[pool.bodyKey(1)], "standing");
    EXPECT_EQ(reasons[pool.bodyKey(2)], "pending");
    EXPECT_EQ(reasons[pool.bodyKey(3)], "candidate");
}

/// ---- Leader changes and failure schedules ----

TEST(CASGcIntents, NewLeaderDeletesTheBodyOfAPendingRow)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    Gc successor(pool.store, hexToU128("00000000000000000000000000000002"));
    RoundReport report;
    for (int i = 0; i < 4 && !report.acquired_lease; ++i)
        report = successor.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_EQ(report.bodies_deleted, 1u);
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

TEST(CASGcIntents, DeposedLeaderBodyDeleteMissesARepublishedBody)
{
    for (const bool steal_before_republish : {false, true})
    {
        SCOPED_TRACE(steal_before_republish);
        auto pool = makePool();
        ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
        const DB::UInt128 successor_id = hexToU128("00000000000000000000000000000002");
        Gc successor(pool.store, successor_id);
        std::vector<RoundReport> successor_rounds;
        std::optional<DB::UInt128> owner_at_release;
        /// The leader's lease stays unchanged while it waits here, so the successor's first round observes it and
        /// its second round steals it.
        const auto takeOver = [&]
        {
            for (int i = 0; i < 2; ++i)
                successor_rounds.push_back(successor.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative));
        };
        /// Between the leader's HEAD and its delete, the successor takes the lease and a writer republishes.
        pool.backend->before_body_delete = [&](const String &)
        {
            if (steal_before_republish)
                takeOver();
            /// After a steal the successor has deleted the body itself, so the writer finds no body.
            if (pool.bodyMeta(1))
                displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(1));
            else
                pool.writeBodies({1});
            if (!steal_before_republish)
                takeOver();
            owner_at_release = storedLeaseOwner(*pool.backend, pool.layout());
        };

        try
        {
            pool.round();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            /// Ok: the deposed commit may be refused; only the store's state matters here.
        }
        ASSERT_EQ(successor_rounds.size(), 2u) << "the leader never reached its body delete";
        EXPECT_FALSE(successor_rounds[0].acquired_lease) << "the first round must only observe the incumbent";
        EXPECT_TRUE(successor_rounds[1].acquired_lease) << "the successor never took the lease";
        ASSERT_TRUE(owner_at_release.has_value());
        EXPECT_EQ(*owner_at_release, successor_id) << "the leader was not deposed before its delete";
        EXPECT_EQ(storedLeaseOwner(*pool.backend, pool.layout()), successor_id);
        EXPECT_TRUE(pool.bodyMeta(1).has_value()) << "the deposed delete removed the republished body";
    }
}

TEST(CASGcIntents, DeposedLeaderLateCreateIsConfirmedByTheNextLeader)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->lose_intent_creates = true;
    pool.round();
    ASSERT_TRUE(pool.row(1) && !pool.row(1)->delete_pending);
    pool.backend->lose_intent_creates = false;
    const String intent = pool.currentIntentKey(1);

    Gc successor(pool.store, hexToU128("00000000000000000000000000000002"));
    RoundReport report;
    for (int i = 0; i < 4 && !report.acquired_lease; ++i)
        report = successor.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_EQ(report.intent_confirmed, 1u);
    /// The deposed leader's create lands late: the key already holds the same record.
    {
        OperationForTest op(*pool.backend);
        const WriteResult late = (*op).create(intent, blobIntentRecord(), Retry::standard());
        EXPECT_TRUE(std::holds_alternative<Conflict>(late));
    }
    successor.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

TEST(CASGcIntents, SparedThenRecondemnedBlobIsDeleted)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    pool.commit("again", {1});
    pool.round();
    ASSERT_FALSE(pool.row(1).has_value());
    pool.drop("again");
    const uint64_t before = counter(ProfileEvents::CASGCIntentConfirmedExisting);

    const RoundReport condemning = pool.round();
    EXPECT_EQ(condemning.intent_confirmed, 1u);
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentConfirmedExisting) - before, 1u) << "the standing intent confirms";
    pool.round();
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

TEST(CASGcIntents, ThrowingRoundDrainsItsDeleteJobsBeforeReturning)
{
    const std::vector<uint64_t> rows = inRunOrder({1, 2, 3});
    auto pool = makePool({.io_concurrency = 4});
    ASSERT_NO_FATAL_FAILURE(pool.makePending(rows));
    const uint64_t round_before = committedRound(*pool.backend, pool.layout());

    /// The first job fails at once; the last one is held. A round that returned on the first failure would leave
    /// the held job running past it.
    ManualBarrier failed;
    failed.release();
    ManualBarrier held;
    std::atomic<bool> held_job_done{false};
    pool.backend->on_body_delete = [&](const String & key)
    {
        if (key == pool.bodyKey(rows.front()))
        {
            failed.arriveAndWait();
            throw Poco::TimeoutException("the body delete was never answered");
        }
        if (key == pool.bodyKey(rows.back()))
        {
            held.arriveAndWait();
            held_job_done = true;
        }
    };

    /// The outcome is returned rather than asserted on the round's thread, where a failure is not reported.
    auto round = std::async(std::launch::async, [&]
    {
        try
        {
            pool.round();
            return false;
        }
        catch (...)
        {
            return true;
            /// Ok: a failed round is the outcome this waits for.
        }
    });
    /// Before the future's destructor, which waits for the round.
    SCOPE_EXIT({ held.release(); });
    held.waitUntilArrived();
    failed.waitUntilArrived();
    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a delete job was still held";
    EXPECT_FALSE(held_job_done.load());

    held.release();
    EXPECT_TRUE(round.get()) << "the failed delete did not fail the round";
    EXPECT_TRUE(held_job_done.load());
    EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);
    for (uint64_t b : rows)
        EXPECT_TRUE(pool.row(b) && pool.row(b)->delete_pending && !pool.row(b)->standing) << b;
}

/// ---- A late intent delete ----

namespace
{

/// Holds the first batch delete that names `parked_key` before it reaches the store, until released.
class IntentDeleteParkingBackend : public HeadHookBackend
{
public:
    void park(const String & key)
    {
        std::lock_guard lock(park_mutex);
        parked_key = key;
    }

    ManualBarrier parked;

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        bool hold = false;
        {
            std::lock_guard lock(park_mutex);
            if (parked_key && std::any_of(keys.begin(), keys.end(), [&](const WriteOnceKey & k) { return k.str() == *parked_key; }))
            {
                parked_key.reset();
                hold = true;
            }
        }
        if (hold)
            parked.arriveAndWait();
        HeadHookBackend::removeManyWriteOnce(keys, access);
    }

private:
    std::mutex park_mutex;
    std::optional<String> parked_key;
};

const UInt128 kGateLeaderA = hexToU128("000000000000000000000000000000a1");
const UInt128 kGateLeaderB = hexToU128("000000000000000000000000000000b2");

/// A pool that folds every round, so a round's number is the number of rounds run.
PoolPtr openGatePool(std::shared_ptr<IntentDeleteParkingBackend> backend)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    return Pool::open(std::move(backend), std::move(config));
}

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

/// Runs rounds until `done` holds after one of them; fails the test after `max_rounds`.
void runRoundsUntil(const PoolPtr & store, Gc & gc, const std::function<bool()> & done, size_t max_rounds = 12)
{
    for (size_t i = 0; i < max_rounds; ++i)
    {
        runGateRound(store, gc);
        if (done())
            return;
    }
    FAIL() << "the condition did not hold within " << max_rounds << " rounds";
}

/// Writes one part holding `payload` through the writer. `Observed` means the writer adopted the stored
/// incarnation; `Published` means it uploaded a fresh one.
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

enum class OldDeleteLands
{
    BeforeTheSuccessorCommitsPending,
    AfterTheSuccessorCommitsPending,
};

/// Leader A removes the intent of `standing(t)` and its delete is held. A successor takes the lease, removes the
/// same intent, a writer publishes the next incarnation `u` and the successor condemns it. A's delete then lands:
/// it must not remove `I(B,u)`, so a writer arriving at the successor's body delete publishes instead of adopting.
void lateIntentDeleteSparesTheSuccessorsIntent(OldDeleteLands when)
{
    auto backend = std::make_shared<IntentDeleteParkingBackend>();
    auto store = openGatePool(backend);
    const Layout & layout = store->layout();
    const String ns = "test/tbl";
    const String payload = "late-intent-delete-payload";
    const BlobRef x = idOf(payload);
    const String body_key = layout.blobKey(x);
    const auto row = [&] { return retiredEntryFor(*backend, layout, x); };

    ASSERT_EQ(writeOnePart(store, ns, "p0", payload), BlobMaterializationAction::Published);
    store->dropRef(RootNamespace{ns}, "p0");
    store->renewWatermarkOnce();
    Gc gc_a(store, kGateLeaderA);
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_a, [&] { return row() && row()->standing; }));
    const String old_intent = layout.blobIntentKey(x, *row()->token.canonical());
    ASSERT_TRUE(objectPresent(*backend, old_intent));

    backend->park(old_intent);
    std::thread leader_a([&]
    {
        try
        {
            runGateRound(store, gc_a);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            /// Ok: A's commit loses to the successor; only the store's state is asserted.
        }
    });
    SCOPE_EXIT({
        backend->parked.release();
        if (leader_a.joinable())
            leader_a.join();
    });
    backend->parked.waitUntilArrived();
    const auto releaseLeaderA = [&]
    {
        backend->parked.release();
        leader_a.join();
    };

    /// A's lease stays unchanged while it is held, so the successor's first round observes it and the second
    /// steals it, removing the same intent and the row.
    Gc gc_b(store, kGateLeaderB);
    ASSERT_FALSE(runGateRound(store, gc_b).acquired_lease);
    ASSERT_TRUE(runGateRound(store, gc_b).acquired_lease);
    ASSERT_EQ(storedLeaseOwner(*backend, layout), kGateLeaderB);
    ASSERT_FALSE(row().has_value());
    ASSERT_FALSE(objectPresent(*backend, old_intent));

    ASSERT_EQ(writeOnePart(store, ns, "p1", payload), BlobMaterializationAction::Published);
    String new_intent;
    {
        OperationForTest op(*backend);
        new_intent = layout.blobIntentKey(x, *(*op).head(body_key, Retry::standard())->etag.canonical());
    }
    store->dropRef(RootNamespace{ns}, "p1");
    store->renewWatermarkOnce();

    bool released = false;
    bool released_before_the_commit = false;
    if (when == OldDeleteLands::BeforeTheSuccessorCommitsPending)
        gc_b.setPhaseSink([&](const GcPhaseRecord & record)
        {
            /// The condemning round created `I(B,u)` in its fold and has not committed `pending(u)` yet.
            if (record.phase != "pending_deletes" || released || !objectPresent(*backend, new_intent))
                return;
            released = true;
            released_before_the_commit = !row().has_value();
            releaseLeaderA();
        });
    ASSERT_NO_FATAL_FAILURE(runRoundsUntil(store, gc_b, [&] { return row() && row()->delete_pending; }));
    gc_b.setPhaseSink({});
    if (when == OldDeleteLands::BeforeTheSuccessorCommitsPending)
        ASSERT_TRUE(released_before_the_commit) << "A's delete was not released between the create and the commit";
    else
        releaseLeaderA();
    ASSERT_FALSE(row()->standing);
    ASSERT_EQ(storedLeaseOwner(*backend, layout), kGateLeaderB) << "A's attempt committed after losing its lease";
    EXPECT_TRUE(objectPresent(*backend, new_intent)) << "A's late delete removed the successor's intent";

    std::optional<BlobMaterializationAction> w2;
    backend->armOnHead(body_key, [&] { w2 = writeOnePart(store, ns, "p2", payload); });
    runGateRound(store, gc_b);
    ASSERT_TRUE(w2.has_value()) << "the successor's body delete never observed the blob";
    EXPECT_EQ(*w2, BlobMaterializationAction::Published) << "the second writer adopted the incarnation the successor deletes";
    OperationForTest op(*backend);
    const auto body = (*op).read(body_key, Retry::standard());
    ASSERT_TRUE(body.has_value()) << "the second writer's part names a blob with no body";
    EXPECT_TRUE(body->bytes.ends_with(payload));
}

}

TEST(CASGcIntents, LateIntentDeleteBeforeTheSuccessorsPendingCommit)
{
    lateIntentDeleteSparesTheSuccessorsIntent(OldDeleteLands::BeforeTheSuccessorCommitsPending);
}

TEST(CASGcIntents, LateIntentDeleteAfterTheSuccessorsPendingCommit)
{
    lateIntentDeleteSparesTheSuccessorsIntent(OldDeleteLands::AfterTheSuccessorCommitsPending);
}

TEST(CASGcIntents, UndecodableIntentBytesCarryTheRow)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    {
        OperationForTest op(*pool.backend);
        (*op).create(pool.currentIntentKey(1), "not an intent record", Retry::standard());
    }
    const uint64_t before = counter(ProfileEvents::CASGCIntentUnconfirmedCarry);

    const RoundReport report = pool.round();

    EXPECT_EQ(report.intent_confirmed, 0u);
    ASSERT_TRUE(pool.row(1));
    EXPECT_FALSE(pool.row(1)->delete_pending);
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentUnconfirmedCarry) - before, 1u);
    pool.round();
    EXPECT_TRUE(pool.bodyMeta(1).has_value()) << "a row confirmed by foreign bytes deleted the body";
}

/// ---- Orphan adoption ----

namespace
{

/// A pool whose every round folds, so an idle round still runs the intent sweep.
IntentPool<> makeSweepingPool(PoolOptions options = {})
{
    options.max_defer_rounds = 0;
    return makePool(options);
}

/// An identity no body of these tests ever carries.
CanonicalIdentity staleIdentity(uint64_t n)
{
    return *canonicalIdentity(Dialect::Emulated, std::to_string(900000 + n));
}

/// An intent with no row, as a lost commit or a rebuild leaves it.
String plantIntent(IntentPool<> & pool, uint64_t b, const CanonicalIdentity & identity)
{
    const String key = pool.layout().blobIntentKey(blobRefOf(b), identity);
    OperationForTest op(*pool.backend);
    const WriteResult written = (*op).create(key, blobIntentRecord(), Retry::standard());
    if (!std::holds_alternative<Committed>(written))
        throw std::runtime_error("cannot plant the intent " + key);
    return key;
}

String plantCurrentIntent(IntentPool<> & pool, uint64_t b)
{
    return plantIntent(pool, b, *pool.bodyMeta(b)->etag.canonical());
}

size_t intentLists(const IntentPool<> & pool, const std::vector<JournalEntry> & journal)
{
    return static_cast<size_t>(std::count_if(journal.begin(), journal.end(), [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::List && e.keys.front() == pool.layout().blobIntentsPrefix();
    }));
}

size_t firstIndex(const std::vector<JournalEntry> & journal, JournalVerb verb, const String & key)
{
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == verb && !journal[i].keys.empty() && journal[i].keys.front() == key)
            return i;
    return journal.size();
}

String intentSweepCursor(const IntentPool<> & pool)
{
    OperationForTest op(*pool.backend);
    return decodeGcState((*op).read(pool.layout().gcStateKey(), Retry::standard())->bytes).intent_sweep_cursor;
}

bool isCandidate(const std::optional<RetiredEntry> & row) { return row && !row->delete_pending && !row->standing; }
bool isPending(const std::optional<RetiredEntry> & row) { return row && row->delete_pending && !row->standing; }
bool isStanding(const std::optional<RetiredEntry> & row) { return row && row->standing; }

bool tokenIs(const std::optional<RetiredEntry> & row, const CanonicalIdentity & identity)
{
    return row && row->token.canonical() == identity;
}

}

TEST(CASGcIntents, OrphanOverTheCurrentBodyBecomesACandidate)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    const Etag current = pool.bodyMeta(1)->etag;
    const String intent = plantCurrentIntent(pool, 1);
    markCutComplete(*pool.gc, *pool.backend);
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);
    pool.backend->clearJournal();

    pool.round();

    const auto journal = pool.backend->journal();
    const size_t list = firstIndex(journal, JournalVerb::List, pool.layout().blobIntentsPrefix());
    const size_t head = firstIndex(journal, JournalVerb::Head, pool.bodyKey(1));
    const size_t cut = indexOfMark(journal, "cut_complete");
    ASSERT_LT(list, journal.size()) << "positive control: the round listed blob-intents/";
    ASSERT_LT(head, journal.size()) << "the adoption HEAD was not sent";
    ASSERT_LT(cut, journal.size());
    EXPECT_GT(head, list) << "the adoption HEAD preceded the listing that named the key";
    EXPECT_GT(head, cut) << "the adoption HEAD preceded the cut";
    EXPECT_GT(list, cut) << "the listing preceded the cut";

    const auto row = pool.row(1);
    ASSERT_TRUE(isCandidate(row));
    EXPECT_TRUE(sameIncarnation(row->token, current));
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 1u);
    const auto requests = blobRequests(journal);
    EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}})) << "adoption sends one HEAD";
    EXPECT_TRUE(pool.exists(intent));
}

/// A strong ETag whose tag starts with `W/` is spelled quoted on the wire; the intent of that body is the same
/// incarnation, so it is adopted once as a candidate and not again by later sweeps.
TEST(CASGcIntents, OrphanOverAQuotedTagStartingWithTheWeakPrefixBecomesACandidate)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    pool.backend->blob_head_value = "\"W/tag\"";
    const Etag current = pool.bodyMeta(1)->etag;
    ASSERT_TRUE(current.canonical()) << "positive control: the quoted tag is a strong ETag";
    plantCurrentIntent(pool, 1);
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);

    pool.round();
    const auto row = pool.row(1);
    ASSERT_TRUE(isCandidate(row)) << "the body was adopted as another incarnation";
    EXPECT_TRUE(sameIncarnation(row->token, current));

    pool.round();
    pool.round();
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 1u) << "a later sweep adopted the intent again";
}

TEST(CASGcIntents, OrphanOverAnAbsentBodyBecomesStanding)
{
    auto pool = makeSweepingPool();
    const String intent = plantIntent(pool, 1, staleIdentity(1));

    pool.round();

    const auto row = pool.row(1);
    ASSERT_TRUE(isStanding(row));
    EXPECT_TRUE(row->delete_pending);
    EXPECT_TRUE(tokenIs(row, staleIdentity(1)));
    EXPECT_TRUE(pool.exists(intent)) << "adoption deletes nothing in its round";

    /// The next zero cut removes the intent and drops the row.
    pool.backend->clearJournal();
    pool.round();
    EXPECT_EQ(blobRequests(pool.backend->journal()), (std::vector<Request>{{JournalVerb::RemoveMany, intent}}));
    EXPECT_FALSE(pool.exists(intent));
    EXPECT_FALSE(pool.row(1).has_value());
}

TEST(CASGcIntents, OrphanOverAnotherTokenBecomesStanding)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    const String intent = plantIntent(pool, 1, staleIdentity(1));

    pool.round();

    ASSERT_TRUE(isStanding(pool.row(1)));
    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(1)));
    pool.round();
    EXPECT_FALSE(pool.exists(intent));
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(pool.bodyMeta(1).has_value()) << "the body under another token is not adoption's to delete";
}

TEST(CASGcIntents, AdoptionDeletesNothingInItsRound)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1, 3});
    plantCurrentIntent(pool, 1);
    plantIntent(pool, 2, staleIdentity(2));
    plantIntent(pool, 3, staleIdentity(3));
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);
    pool.backend->clearJournal();

    pool.round();

    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 3u) << "positive control: three adoptions";
    EXPECT_TRUE(isCandidate(pool.row(1)));
    EXPECT_TRUE(isStanding(pool.row(2)));
    EXPECT_TRUE(isStanding(pool.row(3)));
    for (const auto & [verb, key] : blobRequests(pool.backend->journal()))
        EXPECT_TRUE(verb != JournalVerb::Remove && verb != JournalVerb::RemoveMany && verb != JournalVerb::Write) << key;
}

TEST(CASGcIntents, SweepSkipsAKeyWithARow)
{
    auto pool = makeSweepingPool();
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    const String own = pool.intentKey(1, pool.row(1)->token);
    const String other = plantIntent(pool, 1, staleIdentity(1));
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);
    pool.backend->clearJournal();

    pool.round();

    EXPECT_EQ(intentLists(pool, pool.backend->journal()), 1u) << "positive control: the round listed both intents";
    EXPECT_FALSE(pool.exists(own)) << "the intent removal of the standing row did not run";
    EXPECT_FALSE(pool.row(1).has_value()) << "the key had a row in the adopted state; adoption must skip it";
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted), adopted_before);
    EXPECT_TRUE(pool.exists(other));

    /// A later sweep meets the other intent.
    pool.round();
    EXPECT_TRUE(isStanding(pool.row(1)));
    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(1)));
}

TEST_P(CASGcIntentsSharded, SweepSkipsAKeyTheFoldGivesARow)
{
    for (const bool lose_creates : {false, true})
    {
        SCOPED_TRACE(lose_creates);
        auto pool = makeSweepingPool({.gc_shards = shards(), .create_budget = 1});
        /// One key sorts before the other in every listing and every run; the budget leaves the second a candidate.
        const std::vector<uint64_t> blobs = shards() == 1 ? std::vector<uint64_t>{1, 2} : blobsInShard(3, 4, 2);
        pool.commitThenDrop(blobs);
        for (const uint64_t b : blobs)
            plantIntent(pool, b, staleIdentity(b));
        pool.backend->lose_intent_creates = lose_creates;
        const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);
        pool.backend->clearJournal();

        pool.round();

        EXPECT_EQ(intentLists(pool, pool.backend->journal()), 1u) << "positive control: the stale intents were listed";
        EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted), adopted_before);
        const auto requests = blobRequests(pool.backend->journal());
        for (const uint64_t b : blobs)
        {
            const auto row = pool.row(b);
            ASSERT_TRUE(row) << b;
            EXPECT_TRUE(sameIncarnation(row->token, pool.bodyMeta(b)->etag)) << "the fold's row lost to adoption: " << b;
            EXPECT_FALSE(row->standing) << b;
            EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(b)), 1u) << "a second HEAD for an adoption: " << b;
            EXPECT_TRUE(pool.exists(pool.layout().blobIntentKey(blobRefOf(b), staleIdentity(b)))) << b;
        }
        EXPECT_FALSE(pool.row(blobs[1])->delete_pending) << "positive control: the second key is a candidate";
    }
}

TEST(CASGcIntents, TouchedKeyTheFoldGivesNoRowIsAdopted)
{
    auto pool = makeSweepingPool();
    /// The committed part names a blob whose body never landed: the fresh transition finds it absent.
    pool.commit("tbl", {1});
    pool.round();
    pool.drop("tbl");
    plantIntent(pool, 1, staleIdentity(1));
    pool.backend->clearJournal();

    pool.round();

    EXPECT_TRUE(isStanding(pool.row(1)));
    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(1)));
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 2u)
        << "the fresh transition's HEAD, then the adoption's";
}

TEST(CASGcIntents, SweepSkipsAKeyAtPositiveInDegree)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1, 2});
    pool.commit("tbl", {1});
    pool.round();
    const String held = plantCurrentIntent(pool, 1);
    plantIntent(pool, 2, staleIdentity(2));
    pool.backend->clearJournal();

    pool.round();

    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 0u);
    EXPECT_TRUE(isStanding(pool.row(2))) << "positive control: the unreferenced key was adopted";
    EXPECT_TRUE(pool.exists(held));

    /// Once the reference goes, the fold condemns the key and confirms on the standing intent.
    pool.drop("tbl");
    const uint64_t before = counter(ProfileEvents::CASGCIntentConfirmedExisting);
    pool.round();
    EXPECT_TRUE(isPending(pool.row(1)));
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentConfirmedExisting) - before, 1u);
}

TEST(CASGcIntents, OneOrphanPerKeyPerRound)
{
    auto pool = makeSweepingPool();
    const String first = plantIntent(pool, 1, staleIdentity(1));
    const String second = plantIntent(pool, 1, staleIdentity(2));
    ASSERT_LT(first, second);
    pool.backend->clearJournal();

    pool.round();

    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(1))) << "the first listed intent is adopted";
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 1u);
    pool.round();
    EXPECT_FALSE(pool.exists(first));
    EXPECT_FALSE(pool.row(1).has_value()) << "the key had a row in the adopted state this round";
    EXPECT_TRUE(pool.exists(second));
    pool.round();
    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(2)));
    pool.round();
    EXPECT_FALSE(pool.exists(second));
}

TEST(CASGcIntents, SweepCursorAdvancesOnePagePerRound)
{
    auto pool = makeSweepingPool({.intent_sweep_page = 2});
    std::vector<String> keys;
    for (uint64_t b = 1; b <= 5; ++b)
        keys.push_back(plantIntent(pool, b, staleIdentity(b)));
    ASSERT_TRUE(std::is_sorted(keys.begin(), keys.end()));
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);

    pool.round();
    EXPECT_EQ(intentSweepCursor(pool), keys[1]);
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 2u);
    pool.round();
    EXPECT_EQ(intentSweepCursor(pool), keys[3]);
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 4u);
    pool.round();
    EXPECT_EQ(intentSweepCursor(pool), "") << "the last page wraps the cursor";
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 5u);
    for (uint64_t b = 1; b <= 5; ++b)
        EXPECT_TRUE(pool.row(b) || !pool.exists(keys[b - 1])) << "blob " << b << " was never adopted";
}

TEST(CASGcIntents, RebuildResetsTheIntentCursor)
{
    auto pool = makeSweepingPool({.intent_sweep_page = 1});
    pool.writeBodies({1, 2});
    plantCurrentIntent(pool, 1);
    plantCurrentIntent(pool, 2);
    pool.round();
    ASSERT_FALSE(intentSweepCursor(pool).empty()) << "positive control: the cursor moved";

    const RebuildReport rebuilt = pool.gc->rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rebuilt.refusal.empty()) << rebuilt.refusal;
    EXPECT_EQ(intentSweepCursor(pool), "");
    OperationForTest op(*pool.backend);
    EXPECT_EQ(decodeGcState((*op).read(pool.layout().gcStateKey(), Retry::standard())->bytes).header_generation, G_BLOB_INTENTS);
}

TEST(CASGcIntents, RebuildOnAPoolWithIntentsReadoptsEveryIntent)
{
    auto pool = makeSweepingPool();
    /// Blob 1 standing and blob 2 pending at the same time.
    pool.writeBodies({1, 2});
    pool.commit("a", {1});
    pool.commit("b", {2});
    pool.round();
    pool.drop("a");
    pool.round();
    pool.drop("b");
    pool.round();
    ASSERT_TRUE(isStanding(pool.row(1)));
    ASSERT_TRUE(isPending(pool.row(2)));
    const std::set<String> intents{pool.intentKey(1, pool.row(1)->token), pool.intentKey(2, pool.row(2)->token)};

    ASSERT_TRUE(pool.gc->rebuildBaseline(/*force*/ true).refusal.empty());
    ASSERT_FALSE(pool.row(1).has_value());
    ASSERT_FALSE(pool.row(2).has_value());
    const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);

    pool.round();
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 2u);
    EXPECT_TRUE(isStanding(pool.row(1)));
    EXPECT_TRUE(isCandidate(pool.row(2)));
    for (int i = 0; i < 5; ++i)
        pool.round();
    for (const String & key : intents)
        EXPECT_FALSE(pool.exists(key)) << key;
    EXPECT_FALSE(pool.bodyMeta(2).has_value()) << "the re-adopted candidate's body was not reclaimed";
}

TEST(CASGcIntents, SuppressedRoundListsNothing)
{
    auto pool = makeSweepingPool({.intent_sweep_page = 1});
    plantIntent(pool, 1, staleIdentity(1));
    plantIntent(pool, 2, staleIdentity(2));
    pool.round();
    ASSERT_TRUE(isStanding(pool.row(1)));
    const String cursor = intentSweepCursor(pool);
    ASSERT_FALSE(cursor.empty());
    pool.backend->clearJournal();

    pool.round(UniversePolicy::StageA_Suppressed);

    EXPECT_EQ(intentLists(pool, pool.backend->journal()), 0u);
    EXPECT_FALSE(pool.row(2).has_value());
    EXPECT_EQ(intentSweepCursor(pool), cursor);
    EXPECT_EQ(intentDeletes(blobRequests(pool.backend->journal())), 0u);

    /// Positive control: the next unsuppressed round lists and adopts.
    pool.round();
    EXPECT_TRUE(isStanding(pool.row(2)));
}

TEST(CASGcIntents, DeferredRoundListsNothing)
{
    auto pool = makePool();
    plantIntent(pool, 1, staleIdentity(1));
    pool.backend->clearJournal();

    const RoundReport deferred = pool.round();

    ASSERT_TRUE(deferred.deferred) << "an idle round defers";
    EXPECT_EQ(intentLists(pool, pool.backend->journal()), 0u);
    EXPECT_FALSE(pool.row(1).has_value());

    /// Positive control: a round with a delta folds, lists and adopts.
    pool.touch({7});
    const RoundReport folded = pool.round();
    ASSERT_FALSE(folded.deferred);
    EXPECT_TRUE(isStanding(pool.row(1)));
}

TEST(CASGcIntents, UncanonicalTokenWritesNoRow)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    plantIntent(pool, 1, staleIdentity(1));
    pool.backend->blob_head_value = "W/\"weak\"";
    const uint64_t rejected_before = counter(ProfileEvents::CASGCIntentTokenRejected);
    pool.backend->clearJournal();

    pool.round();

    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 1u)
        << "positive control: the key was observed";
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentTokenRejected) - rejected_before, 1u);
}

TEST(CASGcIntents, NominationDoesNotTriggerAFreshCondemn)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    plantIntent(pool, 1, staleIdentity(1));
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_TRUE(isStanding(pool.row(1))) << "positive control: the nomination was decided";
    EXPECT_TRUE(tokenIs(pool.row(1), staleIdentity(1))) << "the current body was condemned";
    EXPECT_EQ(intentCreates(blobRequests(pool.backend->journal())), 0u);
    EXPECT_EQ(report.condemned, 0u);
}

TEST(CASGcIntents, SweepPageOfZeroIsRefusedByTheGc)
{
    auto backend = std::make_shared<IntentBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.intent_sweep_list_budget_keys = 0;
    const PoolPtr store = Pool::open(backend, std::move(config));
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { Gc(store, hexToU128("00000000000000000000000000000001")); });

    PoolConfig valid{.pool_prefix = "q", .server_root_id = "test"};
    const PoolPtr other = Pool::open(std::make_shared<IntentBackend>(), std::move(valid));
    EXPECT_NO_THROW(Gc(other, hexToU128("00000000000000000000000000000001"))) << "positive control";
}

TEST(CASGcIntents, OrphanAdoptionWithFourShards)
{
    auto pool = makeSweepingPool({.gc_shards = 4});
    const uint64_t lone = blobsInShard(2, 4, 1).front();
    const uint64_t busy = blobsInShard(0, 4, 1).front();
    plantIntent(pool, lone, staleIdentity(lone));
    pool.touch({busy});

    pool.round();

    EXPECT_TRUE(isStanding(pool.row(lone))) << "the shard whose only input was a nomination was carried";
    bool in_its_shard = false;
    for (const RetiredEntry & e : currentRetiredSet(*pool.backend, pool.layout(), blobShard(blobRefOf(lone), 4)))
        in_its_shard = in_its_shard || e.ref == blobRefOf(lone);
    EXPECT_TRUE(in_its_shard) << "the row is routed to the shard of its blob";
    EXPECT_TRUE(tokenIs(pool.row(lone), staleIdentity(lone)));
}

TEST(CASGcIntents, LeaderDiesAfterTheCreateThenAdoptionConfirms)
{
    /// The commit is refused after the fold created the intent: the next round folds the transition again, and its
    /// create confirms on the equal occupant.
    {
        auto pool = makeSweepingPool();
        pool.commitThenDrop({1});
        const String intent = pool.currentIntentKey(1);
        bool refuse_commit = false;
        pool.gc->setPhaseSink([&](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_reduce")
                refuse_commit = true;
        });
        pool.backend->before_write = [&](const String & key)
        {
            if (refuse_commit && key == pool.layout().gcStateKey())
            {
                refuse_commit = false;
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "the round commit was refused");
            }
        };
        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED, [&] { pool.round(); });
        pool.backend->before_write = {};
        pool.gc->setPhaseSink({});
        ASSERT_TRUE(pool.exists(intent)) << "positive control: the create landed";
        ASSERT_FALSE(pool.row(1).has_value());

        const uint64_t confirmed_before = counter(ProfileEvents::CASGCIntentConfirmedExisting);
        pool.round();
        EXPECT_TRUE(isPending(pool.row(1)));
        EXPECT_EQ(counter(ProfileEvents::CASGCIntentConfirmedExisting) - confirmed_before, 1u);
        pool.round();
        EXPECT_FALSE(pool.bodyMeta(1).has_value());
    }
    /// The transition is already folded when the row is lost: only the sweep finds the intent. It adopts it as a
    /// candidate, and the next create confirms on the equal occupant.
    {
        auto pool = makeSweepingPool();
        pool.commitThenDrop({1});
        pool.round();
        ASSERT_TRUE(isPending(pool.row(1)));
        ASSERT_TRUE(pool.gc->rebuildBaseline(/*force*/ true).refusal.empty());
        ASSERT_FALSE(pool.row(1).has_value());
        const uint64_t confirmed_before = counter(ProfileEvents::CASGCIntentConfirmedExisting);
        const uint64_t adopted_before = counter(ProfileEvents::CASGCIntentAdopted);

        pool.round();
        EXPECT_TRUE(isCandidate(pool.row(1)));
        EXPECT_EQ(counter(ProfileEvents::CASGCIntentAdopted) - adopted_before, 1u);
        EXPECT_TRUE(pool.bodyMeta(1).has_value());
        pool.round();
        EXPECT_TRUE(isPending(pool.row(1)));
        EXPECT_EQ(counter(ProfileEvents::CASGCIntentConfirmedExisting) - confirmed_before, 1u);
        pool.round();
        EXPECT_FALSE(pool.bodyMeta(1).has_value());
    }
}

TEST(CASGcIntents, OrphanOverTheCurrentBodyLeadsToItsDeletion)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    const String intent = plantCurrentIntent(pool, 1);
    const String body = pool.bodyKey(1);
    const auto roundRequests = [&]
    {
        pool.backend->clearJournal();
        pool.round();
        return blobRequests(pool.backend->journal());
    };

    EXPECT_EQ(roundRequests(), (std::vector<Request>{{JournalVerb::Head, body}})) << "adoption";
    ASSERT_TRUE(isCandidate(pool.row(1)));
    EXPECT_EQ(roundRequests(), (std::vector<Request>{{JournalVerb::Write, intent}, {JournalVerb::Read, intent}}))
        << "the create is refused and its resolve read finds the equal occupant";
    ASSERT_TRUE(isPending(pool.row(1)));
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
    EXPECT_EQ(roundRequests(), (std::vector<Request>{{JournalVerb::Head, body}, {JournalVerb::Remove, body}})) << "body delete";
    ASSERT_TRUE(isStanding(pool.row(1)));
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
    EXPECT_EQ(roundRequests(), (std::vector<Request>{{JournalVerb::RemoveMany, intent}})) << "intent delete";
    EXPECT_FALSE(pool.exists(intent));
    EXPECT_FALSE(pool.row(1).has_value());
}

TEST(CASGcIntents, OrphanOverTheCurrentBodyIsSparedByAReference)
{
    auto pool = makeSweepingPool();
    pool.writeBodies({1});
    const String intent = plantCurrentIntent(pool, 1);
    pool.round();
    ASSERT_TRUE(isCandidate(pool.row(1)));

    pool.commit("tbl", {1});
    const RoundReport spared = pool.round();
    EXPECT_EQ(spared.spared, 1u);
    EXPECT_FALSE(pool.row(1).has_value());
    pool.backend->clearJournal();
    for (int i = 0; i < 3; ++i)
        pool.round();
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Head, pool.bodyKey(1)), 0u)
        << "a referenced key is never adopted";
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
    EXPECT_TRUE(pool.exists(intent));
    EXPECT_FALSE(pool.row(1).has_value());
}

/// A re-referenced blob keeps the intent of its current body; after a rebuild only the rebuilt in-degree
/// stops the sweep from adopting it and the protocol from deleting a body that is still referenced.
TEST_P(CASGcIntentsSharded, RebuildThenSweepNeverAdoptsAReferencedBody)
{
    auto pool = makeSweepingPool({.gc_shards = shards(), .intent_sweep_page = 1});
    const uint64_t referenced = blob();
    const uint64_t orphan = shards() == 1 ? 2 : blobsInShard(2, 4, 1).front();
    pool.writeBodies({referenced, orphan});
    pool.commit("tbl", {referenced});
    pool.round();
    const String held = plantCurrentIntent(pool, referenced);
    const String orphan_intent = plantCurrentIntent(pool, orphan);

    ASSERT_TRUE(pool.gc->rebuildBaseline(/*force*/ true).refusal.empty());
    ASSERT_EQ(intentSweepCursor(pool), "");
    pool.backend->clearJournal();

    bool listed = false;
    bool wrapped = false;
    for (int i = 0; i < 12; ++i)
    {
        pool.round();
        const bool empty = intentSweepCursor(pool).empty();
        wrapped = wrapped || (listed && empty);
        listed = listed || !empty;
        EXPECT_FALSE(pool.row(referenced).has_value()) << "round " << i;
    }
    ASSERT_TRUE(wrapped) << "positive control: the cursor wrapped";

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(referenced)), 0u);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(referenced)), 0u);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, held), 0u);
    EXPECT_EQ(countOf(requests, JournalVerb::RemoveMany, held), 0u);
    EXPECT_TRUE(pool.bodyMeta(referenced).has_value());
    EXPECT_TRUE(pool.exists(held));
    EXPECT_TRUE(pool.exists(pool.layout().manifestKey(ManifestId{pool.ns, pool.refs.at("tbl")})));

    EXPECT_FALSE(pool.bodyMeta(orphan).has_value()) << "positive control: the unreferenced orphan was reclaimed";
    EXPECT_FALSE(pool.exists(orphan_intent));
}

/// ---- A body too short for the pool's envelope ----

namespace
{

/// Replaces the body of `b` with bytes shorter than the pool's blob envelope.
void truncateBody(IntentPool<> & pool, uint64_t b)
{
    OperationForTest op(*pool.backend);
    if (const auto meta = (*op).head(pool.bodyKey(b), Retry::standard()))
        ASSERT_EQ((*op).remove(pool.bodyKey(b), meta->etag, Retry::standard()), Removal::Removed);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(pool.bodyKey(b), "short", Retry::standard())));
}

/// Runs a round and fails the test, not the binary, when it throws.
std::optional<RoundReport> roundWithoutThrowing(IntentPool<> & pool)
{
    try
    {
        return pool.round();
    }
    catch (const DB::Exception & e)
    {
        ADD_FAILURE() << "the round threw: " << e.message();
    }
    return std::nullopt;
}

}

TEST(CASGcIntents, UndecodableBodyAtAPendingRowIsCarried)
{
    for (const bool touched : {false, true})
    {
        SCOPED_TRACE(touched ? "touched" : "untouched");
        auto pool = makePool();
        ASSERT_NO_FATAL_FAILURE(pool.makePending({1, 2}));
        const RetiredEntry pending = *pool.row(1);
        ASSERT_NO_FATAL_FAILURE(truncateBody(pool, 1));
        if (touched)
            pool.touch({1});
        const uint64_t round_before = committedRound(*pool.backend, pool.layout());
        const uint64_t before = counter(ProfileEvents::CASGCUndecodableBlobBody);
        pool.backend->clearJournal();

        const auto report = roundWithoutThrowing(pool);

        ASSERT_TRUE(report.has_value());
        EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before + 1);
        EXPECT_EQ(report->bodies_deleted, 1u) << "positive control: the other pending row went on";
        EXPECT_FALSE(pool.bodyMeta(2).has_value());
        EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(1)), 0u);
        EXPECT_TRUE(pool.exists(pool.bodyKey(1))) << "the short body was deleted";
        ASSERT_TRUE(isPending(pool.row(1))) << "the row was not carried";
        EXPECT_EQ(pool.row(1)->token.value, pending.token.value);
        EXPECT_EQ(counter(ProfileEvents::CASGCUndecodableBlobBody) - before, 1u);
    }
}

TEST(CASGcIntents, UndecodableBodyAtAFreshTransitionWritesNoRow)
{
    auto pool = makePool();
    pool.writeBodies({1, 2});
    pool.commit("tbl", {1, 2});
    pool.round();
    ASSERT_NO_FATAL_FAILURE(truncateBody(pool, 1));
    pool.drop("tbl");
    const uint64_t before = counter(ProfileEvents::CASGCUndecodableBlobBody);
    pool.backend->clearJournal();

    const auto report = roundWithoutThrowing(pool);

    ASSERT_TRUE(report.has_value());
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(isPending(pool.row(2))) << "positive control: the other blob was condemned";
    EXPECT_EQ(intentCreates(blobRequests(pool.backend->journal())), 1u) << "an intent was created for the short body";
    EXPECT_TRUE(pool.exists(pool.bodyKey(1)));
    EXPECT_EQ(counter(ProfileEvents::CASGCUndecodableBlobBody) - before, 1u);
}

TEST(CASGcIntents, UndecodableBodyUnderAnOrphanIntentIsSkippedAndTheSweepGoesOn)
{
    auto pool = makeSweepingPool({.intent_sweep_page = 1});
    pool.writeBodies({9});
    ASSERT_NO_FATAL_FAILURE(truncateBody(pool, 9));
    const String bad = plantIntent(pool, 9, staleIdentity(9));
    const String good = plantIntent(pool, 10, staleIdentity(10));
    ASSERT_LT(bad, good) << "the short body's intent is listed first";
    const uint64_t before = counter(ProfileEvents::CASGCUndecodableBlobBody);

    ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
    EXPECT_EQ(intentSweepCursor(pool), bad) << "the sweep cursor did not pass the short body";
    EXPECT_FALSE(pool.row(9).has_value());
    EXPECT_EQ(counter(ProfileEvents::CASGCUndecodableBlobBody) - before, 1u);

    ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
    EXPECT_TRUE(isStanding(pool.row(10))) << "positive control: the next orphan was adopted";
    EXPECT_TRUE(pool.exists(pool.bodyKey(9)));
    EXPECT_TRUE(pool.exists(bad)) << "GC deleted the intent over the short body";
}

/// ---- Idle rounds with leftover intents ----

namespace
{

UInt64 metricOr(const std::map<String, UInt64> & metrics, const String & name)
{
    const auto it = metrics.find(name);
    return it == metrics.end() ? 0 : it->second;
}

/// The `intent_sweep_list` and `fold_reduce` metrics of one round that folded.
struct FoldMetrics
{
    std::map<String, UInt64> sweep;
    std::map<String, UInt64> reduce;
};

void recordFolds(IntentPool<> & pool, std::vector<FoldMetrics> & folds)
{
    auto current = std::make_shared<FoldMetrics>();
    pool.gc->setPhaseSink([&folds, current](const GcPhaseRecord & record)
    {
        if (record.phase == "intent_sweep_list")
            current->sweep = record.metrics;
        if (record.phase == "fold_reduce")
        {
            current->reduce = record.metrics;
            folds.push_back(*current);
            *current = {};
        }
    });
}

}

TEST_P(CASGcIntentsSharded, IdleRoundsWithAnIntentOverAReferencedBlobWriteNoRun)
{
    /// Of `max_defer_rounds + 1` consecutive folds, one merges the shard whose only input is a nomination.
    constexpr uint64_t max_defer = 2;
    auto pool = makePool({.gc_shards = shards(), .max_defer_rounds = max_defer});
    pool.writeBodies({blob()});
    pool.commit("tbl", {blob()});
    pool.round();
    const String held = plantCurrentIntent(pool, blob());
    std::vector<FoldMetrics> folds;
    recordFolds(pool, folds);

    for (size_t i = 0; i < 40 && folds.size() < 2 * (max_defer + 1); ++i)
        pool.round();

    ASSERT_EQ(folds.size(), 2 * (max_defer + 1));
    size_t idle = 0;
    size_t merging = 0;
    for (const FoldMetrics & fold : folds)
    {
        EXPECT_EQ(metricOr(fold.sweep, "nominated"), 1u) << "positive control: the intent was listed";
        if (metricOr(fold.reduce, "shards_pure_carry") == shards())
        {
            ++idle;
            EXPECT_EQ(metricOr(fold.reduce, "nominations_waiting"), 1u);
        }
        else
        {
            ++merging;
            EXPECT_EQ(metricOr(fold.reduce, "shards_pure_carry"), shards() - 1);
        }
    }
    EXPECT_EQ(idle, 2 * max_defer) << "a nomination alone took a shard off the pure carry";
    EXPECT_EQ(merging, 2u);
    EXPECT_FALSE(pool.row(blob()).has_value());
    EXPECT_TRUE(pool.exists(held));
}

TEST_P(CASGcIntentsSharded, IdleRoundsAdoptOrphansWithinTheNominationPeriod)
{
    constexpr uint64_t max_defer = 2;
    auto pool = makePool({.gc_shards = shards(), .intent_sweep_page = 1, .max_defer_rounds = max_defer});
    const std::vector<uint64_t> blobs = shards() == 1 ? std::vector<uint64_t>{1, 2} : blobsInShard(3, 4, 2);
    std::map<String, uint64_t> orphans;
    for (uint64_t b : blobs)
        orphans.emplace(plantIntent(pool, b, staleIdentity(b)), b);
    std::vector<FoldMetrics> folds;
    recordFolds(pool, folds);

    /// An adopted orphan over an absent body is `standing`; a later fold removes its intent and the row.
    std::set<uint64_t> adopted;
    for (size_t i = 0; i < 60 && adopted.size() < blobs.size(); ++i)
    {
        const size_t folds_before = folds.size();
        pool.round();
        if (folds.size() == folds_before)
            continue;
        for (uint64_t b : blobs)
            if (isStanding(pool.row(b)))
                adopted.insert(b);
        /// The cursor never passes a listed intent that was not adopted.
        const String cursor = intentSweepCursor(pool);
        for (const auto & [key, b] : orphans)
            if (!cursor.empty() && key <= cursor)
                EXPECT_TRUE(adopted.contains(b)) << "the cursor passed the unadopted intent " << key;
    }

    EXPECT_EQ(adopted.size(), blobs.size()) << folds.size() << " folds";
    EXPECT_LE(folds.size(), blobs.size() * (max_defer + 1)) << "an orphan waited longer than its nomination period";
}

TEST(CASGcIntents, CandidateWhoseBodyIsGoneIsDropped)
{
    auto pool = makePool();
    pool.commitThenDrop({1, 2});
    pool.backend->lose_intent_creates = true;
    pool.round();
    pool.backend->lose_intent_creates = false;
    ASSERT_TRUE(isCandidate(pool.row(1)));
    ASSERT_TRUE(isCandidate(pool.row(2)));
    {
        OperationForTest op(*pool.backend);
        ASSERT_EQ((*op).remove(pool.bodyKey(1), pool.bodyMeta(1)->etag, Retry::standard()), Removal::Removed);
    }
    pool.touch({1, 2});
    pool.backend->clearJournal();

    pool.round();

    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(1)), 1u) << "positive control: the decision HEAD was sent";
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_EQ(intentCreates(requests), 1u) << "an intent was created for a body that is gone";
    EXPECT_TRUE(isPending(pool.row(2))) << "positive control: the candidate with a body was created";
}

/// ---- Observability ----

TEST(CASGcIntents, StandingAnomalyIsCountedAndNamesItsBlob)
{
    auto pool = makePool();
    pool.backend->content_tokens = true;
    ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
    pool.writeBodies({1});
    ASSERT_TRUE(sameIncarnation(pool.row(1)->token, pool.bodyMeta(1)->etag));
    pool.touch({1});
    const uint64_t before = counter(ProfileEvents::CASGCIntentStandingAnomaly);
    ScopedLogCapture capture("CasGc", "error");

    pool.round();

    EXPECT_TRUE(isStanding(pool.row(1))) << "positive control: the row was carried as an anomaly";
    EXPECT_EQ(counter(ProfileEvents::CASGCIntentStandingAnomaly) - before, 1u);
    EXPECT_NE(capture.captured().find(pool.bodyKey(1)), String::npos) << capture.captured();
}

TEST(CASGcIntents, UncanonicalTokenErrorNamesItsBlob)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->blob_head_value = "W/\"weak\"";
    ScopedLogCapture capture("CasGc", "error");

    pool.round();

    EXPECT_FALSE(pool.row(1).has_value()) << "positive control: the uncanonical observation wrote no row";
    EXPECT_NE(capture.captured().find("names no incarnation"), String::npos) << capture.captured();
    EXPECT_NE(capture.captured().find(pool.bodyKey(1)), String::npos) << capture.captured();
}

namespace
{

/// The events of `type` for blob `b`, in emission order.
std::vector<CasEvent> eventsOf(const std::vector<CasEvent> & events, CasEventType type, uint64_t b)
{
    std::vector<CasEvent> out;
    for (const CasEvent & e : events)
        if (e.type == type && e.object_hash == blobIdOf(blobRefOf(b)))
            out.push_back(e);
    return out;
}

/// Installs a sink that keeps every event the pool emits.
std::shared_ptr<std::vector<CasEvent>> recordEvents(IntentPool<> & pool)
{
    auto events = std::make_shared<std::vector<CasEvent>>();
    pool.store->setEventSink([events](CasEvent e) { events->push_back(std::move(e)); });
    return events;
}

}

TEST(CASGcIntents, StandingAndIntentDeleteReachTheEventLog)
{
    auto pool = makePool();
    const auto events = recordEvents(pool);
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    const String token = pool.row(1)->token.dialect + ":" + pool.row(1)->token.value;

    pool.round();
    ASSERT_TRUE(isStanding(pool.row(1)));
    std::vector<CasEvent> standing;
    for (const CasEvent & e : eventsOf(*events, CasEventType::GcRecheckVerdict, 1))
        if (e.outcome == "standing")
            standing.push_back(e);
    ASSERT_EQ(standing.size(), 1u);
    EXPECT_EQ(standing.front().token, token);

    pool.round();
    ASSERT_FALSE(pool.row(1).has_value()) << "positive control: the intent removal ran";
    const auto deletes = eventsOf(*events, CasEventType::BlobIntentDelete, 1);
    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes.front().detail.at("key"), intent);
    EXPECT_EQ(toString(CasEventType::BlobIntentDelete), "blob_intent_delete");
}

TEST(CASGcIntents, OrphanAdoptionReachesTheEventLog)
{
    auto pool = makeSweepingPool();
    const auto events = recordEvents(pool);
    const String intent = plantIntent(pool, 1, staleIdentity(1));

    pool.round();

    ASSERT_TRUE(isStanding(pool.row(1))) << "positive control: the orphan was adopted";
    const auto adoptions = eventsOf(*events, CasEventType::BlobIntentAdopt, 1);
    ASSERT_EQ(adoptions.size(), 1u);
    EXPECT_EQ(adoptions.front().outcome, "standing");
    EXPECT_EQ(adoptions.front().detail.at("key"), intent);
    EXPECT_EQ(toString(CasEventType::BlobIntentAdopt), "blob_intent_adopt");
}

TEST(CASGcIntents, OpenLogsThePoolGenerationAndTokenDialect)
{
    ScopedLogCapture capture("CasPool", "information");
    auto pool = makePool();
    EXPECT_NE(capture.captured().find("reader generation 4, token dialect emulated"), String::npos) << capture.captured();
}

TEST(CASGcIntents, AStuckCreateDoesNotStarveLaterCandidatesUnderABudget)
{
    const std::vector<uint64_t> ordered = inRunOrder({1, 2, 3});
    auto pool = makePool({.create_budget = 1});
    pool.commitThenDrop(ordered);
    /// The first key in run order holds other bytes: its create stays unconfirmed in every round.
    const String stuck = pool.currentIntentKey(ordered[0]);
    {
        OperationForTest op(*pool.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(stuck, "not an intent record", Retry::standard())));
    }
    const uint64_t unconfirmed_before = counter(ProfileEvents::CASGCIntentUnconfirmedCarry);
    ScopedLogCapture capture("CasGc", "error");

    /// A pending row goes on to `standing` in the next round, so each later key is checked when it got there.
    std::set<uint64_t> confirmed;
    size_t rounds = 0;
    for (; rounds < 6 && confirmed.size() < 2; ++rounds)
    {
        pool.backend->clearJournal();
        pool.round();
        EXPECT_LE(intentCreates(blobRequests(pool.backend->journal())), 1u) << "the budget of one create was exceeded";
        for (uint64_t b : {ordered[1], ordered[2]})
            if (pool.row(b) && pool.row(b)->delete_pending)
                confirmed.insert(b);
    }

    EXPECT_EQ(confirmed.size(), 2u) << "a later candidate starved behind the stuck create";
    EXPECT_LE(rounds, 3u);
    EXPECT_TRUE(isCandidate(pool.row(ordered[0])));
    EXPECT_GE(counter(ProfileEvents::CASGCIntentUnconfirmedCarry) - unconfirmed_before, 1u);
    EXPECT_NE(capture.captured().find(stuck), String::npos) << capture.captured();
}

/// The capability probe's scratch object under `blob-intents/_probe/` is not an intent: the sweep skips it, counts it
/// nowhere and leaves it alone.
TEST(CASGcIntents, SweepSkipsTheProbeScratchObject)
{
    auto pool = makeSweepingPool();
    plantIntent(pool, 1, staleIdentity(1));
    const String scratch = pool.layout().blobIntentsProbePrefix() + "0123456789abcdef0123456789abcdef/intent";
    {
        OperationForTest op(*pool.backend);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(scratch, blobIntentRecord(), Retry::standard())));
    }
    std::vector<FoldMetrics> folds;
    recordFolds(pool, folds);

    pool.round();

    ASSERT_EQ(folds.size(), 1u);
    EXPECT_EQ(metricOr(folds[0].sweep, "listed"), 2u) << "positive control: the page held both keys";
    EXPECT_EQ(metricOr(folds[0].sweep, "nominated"), 1u) << "positive control: the orphan was nominated";
    EXPECT_EQ(metricOr(folds[0].sweep, "unparsed"), 0u) << "the scratch object is skipped, not counted as unparsed";
    EXPECT_TRUE(pool.exists(scratch));
}

/// ---- A spare that sees another incarnation ----

namespace
{

enum class SparedRow : uint8_t { Pending, Standing };

const char * name(SparedRow state)
{
    return state == SparedRow::Pending ? "pending" : "standing";
}

/// `b` gets a `pending(t)` or `standing(t)` row with `I(B,t)`, a writer that met the intent replaces the body with
/// `u`, and a new reference to `b` is committed: the next round spares the row. `old_intent` is `I(B,t)`.
void prepareSpare(IntentPool<> & pool, uint64_t b, SparedRow state, String & old_intent)
{
    if (state == SparedRow::Pending)
        ASSERT_NO_FATAL_FAILURE(pool.makePending({b}));
    else
        ASSERT_NO_FATAL_FAILURE(pool.makeStanding({b}));
    old_intent = pool.intentKey(b, pool.row(b)->token);
    if (state == SparedRow::Pending)
        displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(b));
    else
        pool.writeBodies({b});
    ASSERT_NE(pool.currentIntentKey(b), old_intent) << "fixture: the body has another incarnation";
    pool.commit("again", {b});
}

size_t indexOfRequest(const std::vector<JournalEntry> & journal, JournalVerb verb, const String & key)
{
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == verb && std::find(journal[i].keys.begin(), journal[i].keys.end(), key) != journal[i].keys.end())
            return i;
    return journal.size();
}

size_t indexOfLastRequest(const std::vector<JournalEntry> & journal, JournalVerb verb, const String & key)
{
    for (size_t i = journal.size(); i > 0; --i)
        if (journal[i - 1].verb == verb && std::find(journal[i - 1].keys.begin(), journal[i - 1].keys.end(), key) != journal[i - 1].keys.end())
            return i - 1;
    return journal.size();
}

/// Counter deltas over one scope: intents deleted at a spare, all intents deleted, intents kept at a spare.
struct SpareCounters
{
    uint64_t deleted_on_spare = counter(ProfileEvents::CASGCIntentDeletedOnSpare);
    uint64_t deleted = counter(ProfileEvents::CASGCIntentDeleted);
    uint64_t kept = counter(ProfileEvents::CASGCIntentLeftOnSpare);

    uint64_t deletedOnSpare() const { return counter(ProfileEvents::CASGCIntentDeletedOnSpare) - deleted_on_spare; }
    uint64_t deletedAll() const { return counter(ProfileEvents::CASGCIntentDeleted) - deleted; }
    uint64_t keptOnSpare() const { return counter(ProfileEvents::CASGCIntentLeftOnSpare) - kept; }
};

}

/// The spare `HEAD` is sent after the cut and shows `u`: `I(B,t)` is deleted before the commit; no row is written,
/// and no intent is created for `u`.
TEST_P(CASGcIntentsSharded, SpareOverAnotherIncarnationDeletesTheIntent)
{
    for (const SparedRow state : {SparedRow::Pending, SparedRow::Standing})
    {
        SCOPED_TRACE(name(state));
        auto pool = makePool({.gc_shards = shards()});
        String old_intent;
        ASSERT_NO_FATAL_FAILURE(prepareSpare(pool, blob(), state, old_intent));
        const String body = pool.bodyKey(blob());
        const String new_intent = pool.currentIntentKey(blob());
        const SpareCounters counters;
        markCutComplete(*pool.gc, *pool.backend);
        pool.backend->clearJournal();

        const RoundReport report = pool.round();

        const auto journal = pool.backend->journal();
        EXPECT_EQ(blobRequests(journal), (std::vector<Request>{{JournalVerb::Head, body}, {JournalVerb::RemoveMany, old_intent}}));
        const size_t cut = indexOfMark(journal, "cut_complete");
        const size_t head = indexOfRequest(journal, JournalVerb::Head, body);
        const size_t remove = indexOfRequest(journal, JournalVerb::RemoveMany, old_intent);
        const size_t commit = indexOfLastRequest(journal, JournalVerb::Write, pool.layout().gcStateKey());
        ASSERT_LT(cut, journal.size()) << "positive control: the cut was marked";
        ASSERT_LT(commit, journal.size()) << "positive control: the round committed";
        EXPECT_LT(cut, head) << "the spare HEAD was sent before the cut";
        EXPECT_LT(head, remove);
        EXPECT_LT(remove, commit) << "the intent delete was sent after the commit";
        EXPECT_EQ(report.spared, 1u);
        EXPECT_FALSE(pool.row(blob()).has_value());
        EXPECT_FALSE(pool.exists(old_intent));
        EXPECT_FALSE(pool.exists(new_intent));
        EXPECT_TRUE(pool.bodyMeta(blob()).has_value());
        EXPECT_EQ(counters.deletedOnSpare(), 1u);
        EXPECT_EQ(counters.deletedAll(), 1u);
        EXPECT_EQ(counters.keptOnSpare(), 0u);
    }
}

TEST(CASGcIntents, SpareOverATokenWithNoIdentityKeepsTheIntent)
{
    auto pool = makePool();
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const String intent = pool.intentKey(1, pool.row(1)->token);
    pool.commit("again", {1});
    pool.backend->blob_head_value = "W/\"x\"";
    const SpareCounters counters;
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(blobRequests(pool.backend->journal()), (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}}));
    EXPECT_EQ(report.spared, 1u);
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(pool.exists(intent));
    EXPECT_EQ(counters.deletedOnSpare(), 0u);
    EXPECT_EQ(counters.keptOnSpare(), 1u);
}

/// A body shorter than the envelope decides nothing, but a spare never carries its row: the row is dropped beside the
/// live edge, the intent stays, and the body is counted.
TEST(CASGcIntents, SpareOverAShortBodyDropsTheRow)
{
    for (const SparedRow state : {SparedRow::Pending, SparedRow::Standing})
    {
        SCOPED_TRACE(name(state));
        auto pool = makePool();
        if (state == SparedRow::Pending)
            ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
        else
            ASSERT_NO_FATAL_FAILURE(pool.makeStanding({1}));
        const String intent = pool.intentKey(1, pool.row(1)->token);
        ASSERT_NO_FATAL_FAILURE(truncateBody(pool, 1));
        pool.commit("again", {1});
        const uint64_t undecodable_before = counter(ProfileEvents::CASGCUndecodableBlobBody);
        const SpareCounters counters;
        const auto events = recordEvents(pool);
        pool.backend->clearJournal();
        std::optional<RoundReport> report;
        String errors;
        {
            ScopedLogCapture capture("CasGc", "error");
            report = roundWithoutThrowing(pool);
            errors = capture.captured();
        }

        ASSERT_TRUE(report.has_value());
        EXPECT_EQ(blobRequests(pool.backend->journal()), (std::vector<Request>{{JournalVerb::Head, pool.bodyKey(1)}}));
        EXPECT_EQ(report->spared, 1u);
        EXPECT_FALSE(pool.row(1).has_value()) << "the spared row was carried";
        EXPECT_TRUE(pool.exists(intent));
        EXPECT_TRUE(pool.exists(pool.bodyKey(1)));
        EXPECT_EQ(counter(ProfileEvents::CASGCUndecodableBlobBody) - undecodable_before, 1u);
        EXPECT_EQ(counters.keptOnSpare(), 1u);
        EXPECT_EQ(counters.deletedOnSpare(), 0u);
        const auto decodes = eventsOf(*events, CasEventType::CorruptDecode, 1);
        ASSERT_EQ(decodes.size(), 1u);
        EXPECT_EQ(decodes.front().outcome, "spared");
        EXPECT_NE(errors.find("dropped their rows"), String::npos) << errors;
        EXPECT_NE(errors.find(pool.bodyKey(1)), String::npos) << errors;
        EXPECT_EQ(errors.find("carried their rows"), String::npos) << errors;
    }
}

TEST(CASGcIntents, SuppressedRoundSparesWithNoRequest)
{
    auto pool = makePool();
    String old_intent;
    ASSERT_NO_FATAL_FAILURE(prepareSpare(pool, 1, SparedRow::Pending, old_intent));
    pool.backend->clearJournal();

    const RoundReport report = pool.round(UniversePolicy::StageA_Suppressed);

    EXPECT_EQ(report.spared, 1u) << "positive control: the round spared the row";
    EXPECT_TRUE(blobRequests(pool.backend->journal()).empty());
    EXPECT_FALSE(pool.row(1).has_value());
    EXPECT_TRUE(pool.exists(old_intent));
}

TEST(CASGcIntents, SparedCandidateSendsNoHead)
{
    auto pool = makePool();
    pool.commitThenDrop({1});
    pool.backend->lose_intent_creates = true;
    pool.round();
    pool.backend->lose_intent_creates = false;
    ASSERT_TRUE(isCandidate(pool.row(1)));
    displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(1));
    pool.commit("again", {1});
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.spared, 1u) << "positive control: the round spared the row";
    EXPECT_TRUE(blobRequests(pool.backend->journal()).empty());
    EXPECT_FALSE(pool.row(1).has_value());
}

/// Every spared row gets one `HEAD`, after the cut. Above concurrency 1 they are read ahead: the first spared row's
/// `HEAD` is scheduled before the rows behind it and waits until one of them reaches the store; their workers wait
/// until that `HEAD` is sent.
TEST(CASGcIntents, SpareHeadsAreReadAheadAfterTheCut)
{
    constexpr uint64_t concurrency = 4;
    constexpr int64_t window = 4 * concurrency;
    const std::vector<uint64_t> cohort{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint64_t> ordered = inRunOrder(cohort);
    auto pool = makePool({.io_concurrency = concurrency});
    ASSERT_NO_FATAL_FAILURE(pool.makePending(cohort));
    std::vector<String> old_intents;
    for (uint64_t b : cohort)
    {
        old_intents.push_back(pool.intentKey(b, pool.row(b)->token));
        displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(b));
    }
    pool.commit("again", cohort);

    std::set<String> read_ahead;
    for (size_t i = 1; i < ordered.size(); ++i)
        read_ahead.insert(pool.bodyKey(ordered[i]));
    auto workers = std::make_shared<Gate>();
    auto first_read_ahead = std::make_shared<Gate>();
    pool.backend->on_blob_head = [workers, first_read_ahead, read_ahead](const String & key)
    {
        if (!read_ahead.contains(key))
            return;
        first_read_ahead->open();
        workers->wait();
    };
    SCOPE_EXIT({ first_read_ahead->open(); workers->open(); });
    int64_t scheduled_at_cut = 0;
    std::optional<int64_t> outstanding;
    pool.gc->setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_ref_intake")
        {
            scheduled_at_cut = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled);
            pool.backend->mark("cut_complete");
        }
    });
    pool.backend->armOnHead(pool.bodyKey(ordered.front()), [&]
    {
        first_read_ahead->wait();
        outstanding = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled) - scheduled_at_cut;
        workers->open();
    });
    const uint64_t hits_before = counter(ProfileEvents::CASGCReadAheadHit);
    const SpareCounters counters;
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.spared, cohort.size());
    ASSERT_TRUE(outstanding.has_value()) << "the first spared row never sent its HEAD";
    EXPECT_GE(*outstanding, 2) << "no spare HEAD was read ahead";
    EXPECT_LE(*outstanding, window) << "the read-ahead ran past its window";
    const auto journal = pool.backend->journal();
    const size_t cut = indexOfMark(journal, "cut_complete");
    ASSERT_LT(cut, journal.size()) << "positive control: the cut was marked";
    const auto requests = blobRequests(journal);
    for (uint64_t b : cohort)
    {
        EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(b)), 1u) << b;
        EXPECT_GT(indexOfRequest(journal, JournalVerb::Head, pool.bodyKey(b)), cut) << "blob " << b << ": HEAD before the cut";
    }
    for (const String & intent : old_intents)
        EXPECT_FALSE(pool.exists(intent)) << intent;
    EXPECT_GE(counter(ProfileEvents::CASGCReadAheadHit) - hits_before, cohort.size() - 1);
    EXPECT_EQ(counters.deletedOnSpare(), cohort.size());
}

/// Parks blob `HEAD`s at the store and records the peak number parked at once. They are released when the pool is
/// full with `front` on a worker, or when one more than `concurrency` arrive: a `HEAD` sent beside a full pool.
class InFlightHeadLatch
{
public:
    InFlightHeadLatch(String front_, size_t concurrency_) : front(std::move(front_)), concurrency(concurrency_) {}

    void enter(const String & key)
    {
        std::unique_lock lock(mutex);
        ++in_flight;
        peak = std::max(peak, in_flight);
        if (key == front && std::this_thread::get_id() != round_thread)
            front_on_worker = true;
        if (in_flight > concurrency || (in_flight == concurrency && front_on_worker))
            released = true;
        cv.notify_all();
        if (!cv.wait_for(lock, std::chrono::seconds(20), [this] { return released; }))
        {
            timed_out = true;
            released = true;
            cv.notify_all();
        }
        --in_flight;
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    size_t peakInFlight() const
    {
        std::lock_guard lock(mutex);
        return peak;
    }

    bool timedOut() const
    {
        std::lock_guard lock(mutex);
        return timed_out;
    }

private:
    const String front;
    const size_t concurrency;
    const std::thread::id round_thread = std::this_thread::get_id();
    mutable std::mutex mutex;
    std::condition_variable cv;
    size_t in_flight = 0;
    size_t peak = 0;
    bool front_on_worker = false;
    bool released = false;
    bool timed_out = false;
};

/// The first row of a stretch counts against `gc_io_concurrency` like the rows read ahead behind it, for spared rows
/// and for pending rows at zero.
TEST(CASGcIntents, SpareAndPendingHeadsStayWithinTheIoConcurrency)
{
    constexpr uint64_t concurrency = 4;
    const std::vector<uint64_t> cohort{1, 2, 3, 4, 5, 6, 7, 8};
    const std::vector<uint64_t> ordered = inRunOrder(cohort);
    for (const bool spare : {true, false})
    {
        SCOPED_TRACE(spare ? "spared rows" : "pending rows at zero");
        auto pool = makePool({.io_concurrency = concurrency});
        ASSERT_NO_FATAL_FAILURE(pool.makePending(cohort));
        if (spare)
            pool.commit("again", cohort);
        auto latch = std::make_shared<InFlightHeadLatch>(pool.bodyKey(ordered.front()), concurrency);
        pool.backend->on_blob_head = [latch](const String & key) { latch->enter(key); };
        SCOPE_EXIT({ latch->release(); });
        pool.backend->clearJournal();

        const RoundReport report = pool.round();

        if (spare)
            EXPECT_EQ(report.spared, cohort.size()) << "positive control: every row was spared";
        else
            EXPECT_EQ(report.bodies_deleted, cohort.size()) << "positive control: every pending row was decided";
        const auto requests = blobRequests(pool.backend->journal());
        for (uint64_t b : cohort)
            EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(b)), 1u) << b;
        EXPECT_FALSE(latch->timedOut());
        EXPECT_LE(latch->peakInFlight(), concurrency) << "more HEADs were in flight than gc_io_concurrency";
        EXPECT_EQ(latch->peakInFlight(), concurrency) << "positive control: the HEADs filled the pool";
    }
}

/// The fold keeps blob sets for the condemned-row read-ahead only when a parent row can use them, and keeps each blob
/// once: a fold of additions over a parent with no `pending` or `standing` row keeps none.
TEST(CASGcIntents, ReadAheadBlobSetsAreKeptOnlyForPendingParentRows)
{
    constexpr uint64_t concurrency = 4;
    const std::vector<uint64_t> cohort{1, 2, 3, 4, 5, 6};
    for (const uint64_t shards : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(shards);
        {
            SCOPED_TRACE("additions over no condemned row");
            auto pool = makePool({.gc_shards = shards, .io_concurrency = concurrency});
            pool.writeBodies(cohort);
            pool.commit("tbl", cohort);
            std::vector<FoldMetrics> folds;
            recordFolds(pool, folds);

            pool.round();

            ASSERT_EQ(folds.size(), 1u);
            EXPECT_GE(metricOr(folds[0].reduce, "deltas_in"), cohort.size()) << "positive control: the additions were folded";
            EXPECT_EQ(metricOr(folds[0].reduce, "read_ahead_blobs"), 0u);
        }
        {
            SCOPED_TRACE("additions over pending rows");
            auto pool = makePool({.gc_shards = shards, .io_concurrency = concurrency});
            ASSERT_NO_FATAL_FAILURE(pool.makePending(cohort));
            pool.commit("again", cohort);
            std::vector<FoldMetrics> folds;
            recordFolds(pool, folds);
            const uint64_t hits_before = counter(ProfileEvents::CASGCReadAheadHit);

            const RoundReport report = pool.round();

            EXPECT_EQ(report.spared, cohort.size());
            ASSERT_EQ(folds.size(), 1u);
            EXPECT_EQ(metricOr(folds[0].reduce, "read_ahead_blobs"), cohort.size()) << "each referenced blob once";
            EXPECT_GE(counter(ProfileEvents::CASGCReadAheadHit) - hits_before, cohort.size()) << "the spare HEADs were not read ahead";
        }
    }
}

/// A spare `HEAD` without an answer fails the round before its commit: the adopted state keeps `pending(t)` and
/// its intent, and the next round decides again.
TEST(CASGcIntents, FailedSpareHeadFailsTheRoundAndKeepsPending)
{
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.io_concurrency = concurrency});
        String old_intent;
        ASSERT_NO_FATAL_FAILURE(prepareSpare(pool, 1, SparedRow::Pending, old_intent));
        const RetiredEntry pending = *pool.row(1);
        const uint64_t round_before = committedRound(*pool.backend, pool.layout());
        const String body = pool.bodyKey(1);
        auto attempts = std::make_shared<std::atomic<size_t>>(0);
        pool.backend->on_blob_head = [body, attempts](const String & key)
        {
            if (key != body)
                return;
            ++*attempts;
            throw Poco::TimeoutException("the spare HEAD was never answered");
        };
        pool.backend->clearJournal();

        EXPECT_ANY_THROW(pool.round());

        EXPECT_GE(attempts->load(), 1u) << "positive control: the spare HEAD was sent";
        EXPECT_EQ(intentDeletes(blobRequests(pool.backend->journal())), 0u);
        EXPECT_EQ(committedRound(*pool.backend, pool.layout()), round_before);
        ASSERT_TRUE(isPending(pool.row(1)));
        EXPECT_EQ(pool.row(1)->token.value, pending.token.value);
        EXPECT_TRUE(pool.exists(old_intent));

        pool.backend->on_blob_head = {};
        pool.round();
        EXPECT_FALSE(pool.exists(old_intent)) << "positive control: the next round deleted the intent";
    }
}

/// The intent delete lands and the round fails before its commit: the adopted state holds `pending(t)` with no
/// intent over body `u`. The next round spares again, where the absent intent is a success, or, at zero, moves the
/// row to `u`. Neither sends a body delete.
TEST(CASGcIntents, SpareIntentDeleteLandsAndTheRoundFails)
{
    for (const bool zero_next : {false, true})
    {
        SCOPED_TRACE(zero_next ? "next round at zero" : "next round referenced");
        auto pool = makePool();
        String old_intent;
        ASSERT_NO_FATAL_FAILURE(prepareSpare(pool, 1, SparedRow::Pending, old_intent));
        const RetiredEntry pending = *pool.row(1);
        const String body = pool.bodyKey(1);
        const String state_key = pool.layout().gcStateKey();
        pool.backend->before_write = [&pool, state_key, old_intent](const String & key)
        {
            if (key != state_key)
                return;
            const auto journal = pool.backend->journal();
            if (indexOfRequest(journal, JournalVerb::RemoveMany, old_intent) != journal.size())
                throw DB::Exception(DB::ErrorCodes::READONLY, "the store refused the commit");
        };

        EXPECT_ANY_THROW(pool.round());
        pool.backend->before_write = {};

        ASSERT_FALSE(pool.exists(old_intent)) << "positive control: the intent delete landed";
        ASSERT_TRUE(isPending(pool.row(1))) << "the adopted state no longer holds the row";
        EXPECT_EQ(pool.row(1)->token.value, pending.token.value);

        /// The missing intent is a finding only over the row's own token; over a referenced body it is not listed.
        const FsckReport referenced = runFsck(*pool.store, /*detail=*/true);
        EXPECT_EQ(referenced.pending_without_intent, 0u) << formatFsckSummary(referenced);
        EXPECT_EQ(referenced.pending_intent_absent, 0u) << formatFsckSummary(referenced);
        EXPECT_TRUE(referenced.clean()) << formatFsckSummary(referenced);

        if (zero_next)
        {
            pool.drop("again");
            /// The row names `t`, not the body `u`, so the body is awaiting GC.
            const FsckReport unreferenced = runFsck(*pool.store, /*detail=*/true);
            EXPECT_EQ(unreferenced.pending_without_intent, 0u) << formatFsckSummary(unreferenced);
            EXPECT_EQ(unreferenced.awaiting_gc, 1u) << formatFsckSummary(unreferenced);
            EXPECT_TRUE(unreferenced.clean()) << formatFsckSummary(unreferenced);
        }
        const String new_intent = pool.currentIntentKey(1);
        pool.backend->clearJournal();

        pool.round();

        const auto requests = blobRequests(pool.backend->journal());
        EXPECT_EQ(countOf(requests, JournalVerb::Remove, body), 0u) << "a body delete after the intent was gone";
        EXPECT_TRUE(pool.bodyMeta(1).has_value());
        if (zero_next)
        {
            EXPECT_EQ(requests, (std::vector<Request>{
                {JournalVerb::Head, body}, {JournalVerb::Write, new_intent}, {JournalVerb::RemoveMany, old_intent}}));
            ASSERT_TRUE(isPending(pool.row(1)));
            EXPECT_TRUE(sameIncarnation(pool.row(1)->token, pool.bodyMeta(1)->etag));
        }
        else
        {
            EXPECT_EQ(requests, (std::vector<Request>{{JournalVerb::Head, body}, {JournalVerb::RemoveMany, old_intent}}));
            EXPECT_FALSE(pool.row(1).has_value());
        }
    }
}

TEST(CASGcIntents, SpareIntentDeleteReachesTheEventLog)
{
    auto pool = makePool();
    const auto events = recordEvents(pool);
    String old_intent;
    ASSERT_NO_FATAL_FAILURE(prepareSpare(pool, 1, SparedRow::Pending, old_intent));

    pool.round();

    ASSERT_FALSE(pool.exists(old_intent)) << "positive control: the spare deleted the intent";
    const auto deletes = eventsOf(*events, CasEventType::BlobIntentDelete, 1);
    ASSERT_EQ(deletes.size(), 1u);
    EXPECT_EQ(deletes.front().outcome, "deleted");
    EXPECT_EQ(deletes.front().detail.at("key"), old_intent);
    EXPECT_NE(deletes.front().reason.find("referenced again"), String::npos) << deletes.front().reason;
}

/// A writer whose `HEAD` saw `t` reads `I(B,t)` after a spare deleted it over `u`, and adopts `u`. GC never deletes
/// that body while the writer's reference lives, and reclaims it once the reference is dropped.
TEST(CASGcIntents, WriterAdoptingAfterASpareDeleteKeepsItsBody)
{
    auto pool = makePool();
    const uint64_t b = 1;
    ASSERT_NO_FATAL_FAILURE(pool.makePending({b}));
    const String old_intent = pool.intentKey(b, pool.row(b)->token);
    const String body = pool.bodyKey(b);
    const SpareCounters counters;

    const String part = "writer";
    PartWriteInfo info;
    info.intended_ref = pool.ns.string() + "/" + part;
    auto build = pool.store->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobEntryFor("f1", numberedBlobHash(b))});
    build->precommitAdd(pool.ns, part, id);

    /// Between the writer's `HEAD` and its intent read: another writer replaces the body, and a round that sees this
    /// writer's precommit edge spares the row.
    std::optional<RoundReport> spare_round;
    pool.backend->before_intent_read = [&](const String &)
    {
        displaceBlobToken(*pool.backend, pool.layout(), blobRefOf(b));
        spare_round = pool.round();
    };
    pool.backend->clearJournal();
    try
    {
        build->putBlob(blobRefOf(b), BlobSource::fromString("x"));
    }
    catch (const DB::Exception & e)
    {
        /// The payload does not hash to the key, so only an adoption succeeds.
        FAIL() << "the writer did not adopt the body: " << e.message();
    }
    build->promote(pool.ns, part, build->buildId(), id);
    pool.store->renewWatermarkOnce();

    ASSERT_TRUE(spare_round.has_value()) << "the writer never read the intent";
    EXPECT_EQ(spare_round->spared, 1u);
    ASSERT_EQ(counters.deletedOnSpare(), 1u) << "the spare did not delete I(B,t)";
    const auto journal = pool.backend->journal();
    const size_t spare_delete = indexOfRequest(journal, JournalVerb::RemoveMany, old_intent);
    const size_t writer_read = indexOfLastRequest(journal, JournalVerb::Read, old_intent);
    ASSERT_LT(writer_read, journal.size());
    EXPECT_LT(spare_delete, writer_read) << "the writer read the intent before the spare deleted it";
    EXPECT_EQ(indexOfRequest(journal, JournalVerb::Publish, body), journal.size()) << "the writer published instead of adopting";
    const std::optional<CanonicalIdentity> adopted = pool.bodyMeta(b)->etag.canonical();

    for (int i = 0; i < 3; ++i)
    {
        pool.round();
        ASSERT_TRUE(pool.bodyMeta(b).has_value()) << "round " << i << ": the adopted body was deleted under a live reference";
        EXPECT_EQ(pool.bodyMeta(b)->etag.canonical(), adopted) << "round " << i;
    }
    const FsckReport live = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(live.dangling, 0u) << formatFsckSummary(live);
    EXPECT_TRUE(live.clean()) << formatFsckSummary(live);

    pool.store->dropRef(pool.ns, part);
    pool.store->renewWatermarkOnce();
    for (int i = 0; i < 6 && pool.bodyMeta(b).has_value(); ++i)
        pool.round();
    EXPECT_FALSE(pool.bodyMeta(b).has_value()) << "positive control: the body was reclaimed after the reference was dropped";
    const FsckReport dropped = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(dropped.dangling, 0u) << formatFsckSummary(dropped);
    EXPECT_TRUE(dropped.clean()) << formatFsckSummary(dropped);
}

/// ---- No-bytes copies that a round's cut misses ----

namespace
{

/// Runs a hook once, after the store answered a ref-log read under `prefix` with absence: the round has
/// just proved that namespace's frontier. No backend lock is held while the hook runs.
class FrontierHookBackend : public HeadHookBackend
{
public:
    void armOnAbsentLogRead(const String & prefix, std::function<void()> hook)
    {
        std::lock_guard lock(frontier_mutex);
        frontier_prefix = prefix;
        on_frontier = std::move(hook);
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto got = HeadHookBackend::read(key, access);
        std::function<void()> fire;
        {
            std::lock_guard lock(frontier_mutex);
            if (!got && on_frontier && key.starts_with(frontier_prefix) && key.find("/_log/") != String::npos)
                fire = std::exchange(on_frontier, {});
        }
        if (fire)
            fire();
        return got;
    }

private:
    std::mutex frontier_mutex;
    String frontier_prefix;
    std::function<void()> on_frontier;
};

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

/// The `createHardLink` / relink path: a part in `ns` that names `payload`'s blob, with no request on the body.
void copyWithoutBytes(const PoolPtr & store, const String & ns, const String & ref_name, const String & payload)
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
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(root, ref_name, id);
    build->promote(root, ref_name, build->buildId(), id);
}

/// The round that ran after journal entry `from` decided the blob and destroyed nothing a committed part names:
/// it sent its decision `HEAD` and no body `DELETE`, the body is stored and fsck finds no dangling reference.
void expectBodyKept(CountingBackend & backend, Pool & store, const String & body_key, size_t from,
                    const RoundReport & round)
{
    const auto journal = backend.journal();
    const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
    EXPECT_GT(countOf(requests, JournalVerb::Head, body_key), 0u) << "positive control: the round sent its decision HEAD";
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, body_key) + countOf(requests, JournalVerb::RemoveMany, body_key), 0u)
        << "the round sent a body DELETE; deleted=" << round.deleted;
    OperationForTest op(backend);
    EXPECT_TRUE((*op).read(body_key, Retry::standard()).has_value())
        << "the round deleted the body that a committed part names; deleted=" << round.deleted;
    const FsckReport report = runFsck(store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

/// A carried row is the `pending` row of `token`, unchanged; a positive cut drops the row.
void expectRowOutcome(const std::optional<RetiredEntry> & row, bool carried, const PersistedEtag & token)
{
    if (!carried)
    {
        EXPECT_FALSE(row.has_value()) << "the cut was positive, so the spare drops the row";
        return;
    }
    ASSERT_TRUE(row.has_value()) << "the round dropped the pending row";
    EXPECT_TRUE(row->delete_pending && !row->standing) << "the row is not pending; standing=" << row->standing;
    EXPECT_EQ(row->token.canonical(), token.canonical()) << "the carried row changed its token";
}

struct CrossNamespaceOrder
{
    /// The destination's namespace sorts first, so pass 1 proves its frontier before it reads the source.
    /// Otherwise the source sorts first and the source's records past one read window are read in pass 2.
    bool destination_first;
    /// The control: the copy and the drop land before round 2 starts, so its cut holds both.
    bool before_the_round = false;
    /// The control: the copy and the drop land at the probe that proves the source's frontier, so the cut holds the
    /// source's edge.
    bool source_read_first = false;
};

String crossNamespaceOrderName(const ::testing::TestParamInfo<CrossNamespaceOrder> & param_info)
{
    if (param_info.param.before_the_round)
        return "BeforeTheRound";
    if (param_info.param.source_read_first)
        return "SourceReadBeforeTheDrop";
    return param_info.param.destination_first ? "DestinationFirst" : "SourceFirst";
}

const auto kCrossNamespaceOrders = ::testing::Values(
    CrossNamespaceOrder{.destination_first = true}, CrossNamespaceOrder{.destination_first = false},
    CrossNamespaceOrder{.destination_first = true, .before_the_round = true},
    CrossNamespaceOrder{.destination_first = true, .source_read_first = true});

class CASGcIntentsCrossNamespace : public ::testing::TestWithParam<CrossNamespaceOrder>
{
};

class CASGcIntentsTransitionSource : public ::testing::TestWithParam<CrossNamespaceOrder>
{
};

}

/// Round 1 misses the edge of writer `W`, which adopts body `t`, and commits `pending(t)`. In round 2, after the
/// round proved the frontier of namespace `L` and before it reads the rest of `W`'s namespace, `L` copies `W`'s
/// entry with no request on the body while `W`'s edge is live, and then `W`'s part is dropped. The body must
/// survive: `L` names it.
TEST_P(CASGcIntentsCrossNamespace, NoBytesCopyBetweenTwoFrontiersKeepsTheBody)
{
    auto backend = std::make_shared<FrontierHookBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    auto store = Pool::open(backend, std::move(config));
    const Layout & layout = store->layout();
    const bool destination_first = GetParam().destination_first;
    const bool before_the_round = GetParam().before_the_round;
    const bool source_read_first = GetParam().source_read_first;
    const String ns_w = destination_first ? "test/b_source" : "test/a_source";
    const String ns_l = destination_first ? "test/a_copy" : "test/b_copy";
    const String payload = "cross-namespace-no-bytes-copy";
    const BlobRef x = idOf(payload);
    const String body_key = layout.blobKey(x);
    const auto row = [&] { return retiredEntryFor(*backend, layout, x); };
    Gc gc(store, kGateLeaderA);

    backend->clearJournal();
    ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    const String ns_l_stream = firstWrittenStreamPrefix(*backend);
    ASSERT_FALSE(ns_l_stream.empty());

    backend->clearJournal();
    ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
    const String ns_w_stream = firstWrittenStreamPrefix(*backend);
    ASSERT_FALSE(ns_w_stream.empty());
    runGateRound(store, gc);
    store->dropRef(RootNamespace{ns_w}, "p0");
    store->renewWatermarkOnce();

    /// Round 1. `W` writes at the decision `HEAD`, after the cut: its edge is missed, it adopts `t` before the
    /// intent exists, and the round commits `pending(t)`.
    std::optional<BlobMaterializationAction> w;
    backend->armOnHead(body_key, [&] { w = writeOnePart(store, ns_w, "w", payload); });
    runGateRound(store, gc);
    ASSERT_EQ(w, BlobMaterializationAction::Observed) << "W did not adopt the stored body";
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 1 did not commit pending(t)";
    const PersistedEtag t = row()->token;

    /// Round 2. At the absent probe that proves `L`'s frontier: the copy, then the drop of `W`'s part.
    bool copied = false;
    size_t copy_begin = 0;
    size_t copy_end = 0;
    const auto copy_then_drop = [&]
    {
        copy_begin = backend->journal().size();
        copyWithoutBytes(store, ns_l, "copy", payload);
        copy_end = backend->journal().size();
        store->dropRef(RootNamespace{ns_w}, "w");
        copied = true;
    };
    if (before_the_round)
        copy_then_drop();
    else
        backend->armOnAbsentLogRead(source_read_first ? ns_w_stream : ns_l_stream, copy_then_drop);
    const size_t round_begin = backend->journal().size();
    const RoundReport round2 = runGateRound(store, gc);
    ASSERT_TRUE(copied) << "the round never proved the hooked frontier";

    const auto journal = backend->journal();
    for (size_t i = copy_begin; i < copy_end; ++i)
        for (const String & key : journal[i].keys)
            EXPECT_TRUE(key.find(body_key) == String::npos && key.find("/blob-intents/") == String::npos)
                << "the copy sent a request on the body or an intent: " << key;

    expectBodyKept(*backend, *store, body_key, round_begin, round2);
    /// Between the frontiers the cut shows zero with `W`'s records folded; the controls show `L`'s or `W`'s edge.
    expectRowOutcome(row(), /*carried=*/!before_the_round && !source_read_first, t);
}

INSTANTIATE_TEST_SUITE_P(CASOrder, CASGcIntentsCrossNamespace, kCrossNamespaceOrders, crossNamespaceOrderName);

namespace
{

struct NewbornCase
{
    /// The destination namespace does not exist at the round's catalog read; otherwise it exists and sorts after
    /// the source, so the walk reads it after the copy.
    bool newborn;
};

class CASGcIntentsNewbornCopy : public ::testing::TestWithParam<NewbornCase>
{
};

}

/// Round 1 commits `pending(t)` while writer `W` adopts `t` after the cut. Round 2, right after its catalog read:
/// a no-bytes copy of `W`'s part into namespace `L`, then the drop of `W`'s part. When `L` is born by that copy,
/// round 2 does not walk it; the copy's source is walked.
TEST_P(CASGcIntentsNewbornCopy, NoBytesCopyIntoANamespaceBornAfterTheCatalogReadKeepsTheBody)
{
    auto backend = std::make_shared<HeadHookBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    auto store = Pool::open(backend, std::move(config));
    const Layout & layout = store->layout();
    const bool newborn = GetParam().newborn;
    const String ns_w = "test/b_source";
    const String ns_l = newborn ? "test/z_newborn" : "test/z_copy";
    const String payload = "newborn-namespace-no-bytes-copy";
    const BlobRef x = idOf(payload);
    const String body_key = layout.blobKey(x);
    const auto row = [&] { return retiredEntryFor(*backend, layout, x); };
    Gc gc(store, kGateLeaderA);

    if (!newborn)
        ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
    runGateRound(store, gc);
    store->dropRef(RootNamespace{ns_w}, "p0");
    store->renewWatermarkOnce();

    std::optional<BlobMaterializationAction> w;
    backend->armOnHead(body_key, [&] { w = writeOnePart(store, ns_w, "w", payload); });
    runGateRound(store, gc);
    ASSERT_EQ(w, BlobMaterializationAction::Observed) << "W did not adopt the stored body";
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 1 did not commit pending(t)";
    const PersistedEtag t = row()->token;

    bool fired = false;
    gc.setPostHotScanCatalogReadHookForTest([&]
    {
        copyWithoutBytes(store, ns_l, "copy", payload);
        store->dropRef(RootNamespace{ns_w}, "w");
        fired = true;
    });
    const size_t round_begin = backend->journal().size();
    const RoundReport round2 = runGateRound(store, gc);
    ASSERT_TRUE(fired) << "the catalog-read hook never fired";

    expectBodyKept(*backend, *store, body_key, round_begin, round2);
    /// The newborn namespace is not walked, so the cut shows zero; the existing one is walked after the copy.
    expectRowOutcome(row(), /*carried=*/newborn, t);
}

INSTANTIATE_TEST_SUITE_P(CASCatalog, CASGcIntentsNewbornCopy,
    ::testing::Values(NewbornCase{.newborn = true}, NewbornCase{.newborn = false}),
    [](const ::testing::TestParamInfo<NewbornCase> & param_info)
    {
        return param_info.param.newborn ? std::string("Newborn") : std::string("ExistingReadAfterTheCopy");
    });

/// The `pending` row with a live reference comes from the ordinary reuse after condemnation: in round 2 writer `Y`
/// meets `I(B,t)` after the cut and publishes `u`; the decision `HEAD` shows `u`, so the transition commits
/// `pending(u)` although `Y`'s edge is not in the cut. In round 3, after the round proved `L`'s frontier, a no-bytes
/// copy of `Y`'s part into `L` and the drop of `Y`'s part.
TEST_P(CASGcIntentsTransitionSource, NoBytesCopyOfARepublishedBodyBetweenTwoFrontiersKeepsTheBody)
{
    auto backend = std::make_shared<FrontierHookBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    auto store = Pool::open(backend, std::move(config));
    const Layout & layout = store->layout();
    const bool destination_first = GetParam().destination_first;
    const bool before_the_round = GetParam().before_the_round;
    const bool source_read_first = GetParam().source_read_first;
    const String ns_w = destination_first ? "test/b_source" : "test/a_source";
    const String ns_l = destination_first ? "test/a_copy" : "test/b_copy";
    const String payload = "transition-source-no-bytes-copy";
    const BlobRef x = idOf(payload);
    const String body_key = layout.blobKey(x);
    const auto row = [&] { return retiredEntryFor(*backend, layout, x); };
    Gc gc(store, kGateLeaderA);

    backend->clearJournal();
    ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    const String ns_l_stream = firstWrittenStreamPrefix(*backend);
    ASSERT_FALSE(ns_l_stream.empty());

    backend->clearJournal();
    ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
    const String ns_w_stream = firstWrittenStreamPrefix(*backend);
    ASSERT_FALSE(ns_w_stream.empty());
    runGateRound(store, gc);
    store->dropRef(RootNamespace{ns_w}, "p0");
    store->renewWatermarkOnce();

    runGateRound(store, gc);
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 1 did not commit pending(t)";
    const PersistedEtag t = row()->token;

    std::optional<BlobMaterializationAction> y;
    backend->armOnHead(body_key, [&] { y = writeOnePart(store, ns_w, "y", payload); });
    runGateRound(store, gc);
    ASSERT_EQ(y, BlobMaterializationAction::Published) << "Y did not republish after meeting the intent";
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 2 did not commit pending(u)";
    const PersistedEtag u = row()->token;
    ASSERT_NE(u.canonical(), t.canonical()) << "round 2 did not move the row to Y's incarnation";

    bool copied = false;
    const auto copy_then_drop = [&]
    {
        copyWithoutBytes(store, ns_l, "copy", payload);
        store->dropRef(RootNamespace{ns_w}, "y");
        copied = true;
    };
    if (before_the_round)
        copy_then_drop();
    else
        backend->armOnAbsentLogRead(source_read_first ? ns_w_stream : ns_l_stream, copy_then_drop);
    const size_t round_begin = backend->journal().size();
    const RoundReport round3 = runGateRound(store, gc);
    ASSERT_TRUE(copied) << "the round never proved the hooked frontier";

    expectBodyKept(*backend, *store, body_key, round_begin, round3);
    expectRowOutcome(row(), /*carried=*/!before_the_round && !source_read_first, u);
}

INSTANTIATE_TEST_SUITE_P(CASOrder, CASGcIntentsTransitionSource, kCrossNamespaceOrders, crossNamespaceOrderName);

/// ---- A pending row whose blob the round touched is carried ----

namespace
{

/// The pool of the no-bytes copy tests: writer `w` in `ns_w` holds body `t` under `pending(t)`, because round 1
/// missed its edge. Every namespace in `copy_namespaces` has an unrelated seed part, so each round walks it.
class MissedWriterPool
{
public:
    MissedWriterPool(const String & ns_w_, const std::vector<String> & copy_namespaces, const String & payload_,
                     uint64_t gc_shards = 1)
        : ns_w(ns_w_), payload(payload_), x(idOf(payload_))
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
        config.gc_shards = gc_shards;
        store = Pool::open(backend, std::move(config));
        body_key = store->layout().blobKey(x);
        gc = std::make_unique<Gc>(store, kGateLeaderA);
        for (const String & ns : copy_namespaces)
        {
            backend->clearJournal();
            EXPECT_EQ(writeOnePart(store, ns, "seed", "an unrelated part of " + ns), BlobMaterializationAction::Published);
            streams[ns] = firstWrittenStreamPrefix(*backend);
            EXPECT_FALSE(streams[ns].empty()) << ns;
        }
    }

    /// Ends with `pending(t)` while `w` names `t`.
    void commitPendingUnderAMissedWriter()
    {
        backend->clearJournal();
        ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
        streams[ns_w] = firstWrittenStreamPrefix(*backend);
        ASSERT_FALSE(streams[ns_w].empty());
        runGateRound(store, *gc);
        store->dropRef(RootNamespace{ns_w}, "p0");
        store->renewWatermarkOnce();

        std::optional<BlobMaterializationAction> w;
        backend->armOnHead(body_key, [&] { w = writeOnePart(store, ns_w, "w", payload); });
        runGateRound(store, *gc);
        ASSERT_EQ(w, BlobMaterializationAction::Observed) << "W did not adopt the stored body";
        ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 1 did not commit pending(t)";
        t = row()->token;
    }

    /// One round; at the absent probe that proves `frontier_ns`'s frontier, `copy_ns` gets a no-bytes copy named
    /// `copy_ref` and `drop_ns`'s part `drop_ref` is dropped. Expects the body kept by that round.
    RoundReport relayRound(const String & frontier_ns, const String & copy_ns, const String & copy_ref,
                           const String & drop_ns, const String & drop_ref)
    {
        bool copied = false;
        backend->armOnAbsentLogRead(streams.at(frontier_ns), [&]
        {
            copyWithoutBytes(store, copy_ns, copy_ref, payload);
            store->dropRef(RootNamespace{drop_ns}, drop_ref);
            copied = true;
        });
        const size_t from = backend->journal().size();
        const RoundReport report = runGateRound(store, *gc);
        EXPECT_TRUE(copied) << "the round never proved the frontier of " << frontier_ns;
        expectBodyKept(*backend, *store, body_key, from, report);
        return report;
    }

    std::optional<RetiredEntry> row() const
    {
        const uint64_t shards = store->poolMeta().gc_shards;
        for (const RetiredEntry & entry : currentRetiredSet(*backend, store->layout(), blobShard(x, shards)))
            if (entry.ref == x)
                return entry;
        return std::nullopt;
    }

    bool bodyStored() const
    {
        OperationForTest op(*backend);
        return (*op).read(body_key, Retry::standard()).has_value();
    }

    std::shared_ptr<FrontierHookBackend> backend = std::make_shared<FrontierHookBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    const String ns_w;
    const String payload;
    const BlobRef x;
    String body_key;
    std::map<String, String> streams;
    PersistedEtag t;
};


}

/// A relay over three namespaces: in each round the copy lands behind the frontier of the next namespace and its
/// source is dropped. The body survives every round, and the last copy keeps it in a quiet round.
TEST(CASGcIntentsRelay, ThreeNamespaceRelayKeepsTheBody)
{
    MissedWriterPool pool("test/c_source", {"test/a_last", "test/b_middle"}, "three-namespace-relay");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());

    pool.relayRound("test/b_middle", "test/b_middle", "copy", "test/c_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);
    pool.relayRound("test/a_last", "test/a_last", "copy", "test/b_middle", "copy");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    const size_t from = pool.backend->journal().size();
    const RoundReport quiet = runGateRound(pool.store, *pool.gc);
    expectBodyKept(*pool.backend, *pool.store, pool.body_key, from, quiet);
    expectRowOutcome(pool.row(), /*carried=*/false, pool.t);
}

/// A relay over two namespaces whose second copy goes back into the first: the source sorts first, so the first
/// copy lands behind the destination's frontier while the source's records past one read window wait for pass 2.
TEST(CASGcIntentsRelay, TwoNamespaceRelayThatCopiesBackKeepsTheBody)
{
    MissedWriterPool pool("test/a_source", {"test/b_copy"}, "two-namespace-relay-back");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());

    pool.relayRound("test/b_copy", "test/b_copy", "copy", "test/a_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    /// The source namespace now sorts first as the destination of the back copy.
    pool.relayRound("test/a_source", "test/a_source", "back", "test/b_copy", "copy");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    const size_t from = pool.backend->journal().size();
    const RoundReport quiet = runGateRound(pool.store, *pool.gc);
    expectBodyKept(*pool.backend, *pool.store, pool.body_key, from, quiet);
    expectRowOutcome(pool.row(), /*carried=*/false, pool.t);
}

/// After the copy of the cross-namespace case, the destination part keeps the body: a quiet round sees its edge,
/// spares the blob and drops the row. Once the destination is dropped the body is reclaimed.
TEST(CASGcIntentsRelay, DestinationPartKeepsTheBodyUntilItIsDropped)
{
    MissedWriterPool pool("test/b_source", {"test/a_copy"}, "destination-keeps-the-body");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());
    pool.relayRound("test/a_copy", "test/a_copy", "copy", "test/b_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    const size_t from = pool.backend->journal().size();
    const RoundReport quiet = runGateRound(pool.store, *pool.gc);
    expectBodyKept(*pool.backend, *pool.store, pool.body_key, from, quiet);
    expectRowOutcome(pool.row(), /*carried=*/false, pool.t);

    pool.store->dropRef(RootNamespace{"test/a_copy"}, "copy");
    pool.store->renewWatermarkOnce();
    for (int i = 0; i < 6 && pool.bodyStored(); ++i)
        runGateRound(pool.store, *pool.gc);
    EXPECT_FALSE(pool.bodyStored()) << "the body was not reclaimed after the destination part was dropped";
    const FsckReport report = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

/// The cross-namespace case for a blob of a non-zero shard of four.
TEST(CASGcIntentsRelay, NoBytesCopyInANonZeroShardKeepsTheBody)
{
    constexpr uint64_t shards = 4;
    String payload;
    for (size_t i = 0; payload.empty(); ++i)
        if (const String candidate = "sharded-cross-namespace-" + std::to_string(i); blobShard(idOf(candidate), shards) != 0)
            payload = candidate;
    MissedWriterPool pool("test/b_source", {"test/a_copy"}, payload, shards);
    ASSERT_EQ(pool.store->poolMeta().gc_shards, shards);
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());
    pool.relayRound("test/a_copy", "test/a_copy", "copy", "test/b_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);
}

/// A reference added and removed in one intake while the row is `pending`: the round carries the row, and the next
/// round, which folds no record of the blob, deletes the body.
TEST_P(CASGcIntentsSharded, AddAndDropInOneIntakeCarriesThePendingRow)
{
    auto pool = makePool({.gc_shards = shards()});
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob()}));
    const PersistedEtag t = pool.row(blob())->token;

    pool.touch({blob()});
    pool.backend->clearJournal();
    pool.round();
    auto requests = blobRequests(pool.backend->journal());
    EXPECT_GT(countOf(requests, JournalVerb::Head, pool.bodyKey(blob())), 0u) << "positive control: the decision HEAD";
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(blob())), 0u) << "the touched round deleted the body";
    expectRowOutcome(pool.row(blob()), /*carried=*/true, t);

    pool.backend->clearJournal();
    pool.round();
    requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(blob())), 1u) << "the quiet round did not delete the body";
    EXPECT_FALSE(pool.bodyMeta(blob()).has_value());
}

/// The carry is per blob: an untouched pending blob of the same shard is deleted in the round that carries the
/// touched one.
TEST_P(CASGcIntentsSharded, CarriedRowDoesNotHoldBackAnotherBlobOfItsShard)
{
    auto pool = makePool({.gc_shards = shards()});
    const uint64_t other = shards() == 1 ? 2 : blobsInShard(3, 4, 2).back();
    ASSERT_NE(other, blob());
    ASSERT_EQ(blobShard(blobRefOf(other), shards()), blobShard(blobRefOf(blob()), shards()));
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob(), other}));
    const PersistedEtag t = pool.row(blob())->token;

    pool.touch({blob()});
    pool.backend->clearJournal();
    const RoundReport report = pool.round();
    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(blob())), 0u) << "the touched blob's body was deleted";
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(other)), 1u) << "the untouched blob's body was kept";
    EXPECT_EQ(report.bodies_deleted, 1u);
    expectRowOutcome(pool.row(blob()), /*carried=*/true, t);
    ASSERT_TRUE(pool.row(other).has_value());
    EXPECT_TRUE(pool.row(other)->standing);
}

/// The same content added and dropped between every two rounds: no body delete while it lasts. After the last
/// drop, the round that folds it carries the row and the next one deletes the body.
TEST_P(CASGcIntentsSharded, ChurnOfOneContentDefersTheBodyDelete)
{
    auto pool = makePool({.gc_shards = shards()});
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob()}));
    const PersistedEtag t = pool.row(blob())->token;

    pool.backend->clearJournal();
    for (int i = 0; i < 3; ++i)
    {
        pool.touch({blob()});
        const RoundReport report = pool.round();
        EXPECT_EQ(report.bodies_deleted, 0u) << "churn round " << i;
        expectRowOutcome(pool.row(blob()), /*carried=*/true, t);
    }
    auto requests = blobRequests(pool.backend->journal());
    EXPECT_GE(countOf(requests, JournalVerb::Head, pool.bodyKey(blob())), 3u) << "positive control: a decision HEAD per round";
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(blob())), 0u);
    ASSERT_TRUE(pool.bodyMeta(blob()).has_value());

    /// An unsuppressed round with the default (unbounded) redelete budget, and no record of the blob.
    pool.backend->clearJournal();
    const RoundReport quiet = pool.round();
    requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(blob())), 1u);
    EXPECT_EQ(quiet.bodies_deleted, 1u);
    EXPECT_FALSE(pool.bodyMeta(blob()).has_value());
}
