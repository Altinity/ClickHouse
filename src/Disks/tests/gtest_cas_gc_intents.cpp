#include <gtest/gtest.h>

#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
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
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NO_REPLICA_HAS_PART;
extern const int NETWORK_ERROR;
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

        expectThrowsCodeWithMessage(DB::ErrorCodes::READONLY, "the store is read-only", [&] { pool.round(); });

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

namespace
{

/// Counts the jobs the round has scheduled since the cut when the first row of `ordered` sends its `HEAD`. That
/// `HEAD` waits until one of the rows behind it reaches the store, and their workers wait until it is sent, so every
/// job the scan scheduled is still counted there. `on_cut` runs at the cut.
class ReadAheadWindowProbe
{
public:
    ReadAheadWindowProbe(IntentPool<> & pool, const std::vector<uint64_t> & ordered, std::function<void()> on_cut = {})
    {
        std::set<String> read_ahead;
        for (size_t i = 1; i < ordered.size(); ++i)
            read_ahead.insert(pool.bodyKey(ordered[i]));
        pool.backend->on_blob_head = [workers_ = workers, first_read_ahead_ = first_read_ahead, read_ahead](const String & key)
        {
            if (!read_ahead.contains(key))
                return;
            first_read_ahead_->open();
            workers_->wait();
        };
        pool.gc->setPhaseSink([this, at_cut = std::move(on_cut)](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_ref_intake")
            {
                scheduled_at_cut = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled);
                if (at_cut)
                    at_cut();
            }
        });
        pool.backend->armOnHead(pool.bodyKey(ordered.front()), [this]
        {
            first_read_ahead->wait();
            outstanding = CurrentMetrics::get(CurrentMetrics::LocalThreadScheduled) - scheduled_at_cut;
            workers->open();
        });
    }

    ~ReadAheadWindowProbe()
    {
        first_read_ahead->open();
        workers->open();
    }

    std::optional<int64_t> outstanding;

private:
    std::shared_ptr<Gate> workers = std::make_shared<Gate>();
    std::shared_ptr<Gate> first_read_ahead = std::make_shared<Gate>();
    int64_t scheduled_at_cut = 0;
};

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
    ReadAheadWindowProbe probe(pool, ordered);

    const RoundReport report = pool.round();

    EXPECT_EQ(report.bodies_deleted, cohort.size()) << "positive control: every pending row was decided";
    ASSERT_TRUE(probe.outstanding.has_value()) << "the first pending row never sent its HEAD";
    EXPECT_GE(*probe.outstanding, 2) << "no pending HEAD was read ahead";
    EXPECT_LE(*probe.outstanding, window) << "the read-ahead ran past its window";
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

namespace
{

/// Runs a round that fails while `held` parks one of its jobs: once `wait_until_in_flight` returns, the round must
/// still be running, and it returns its failure only after `held` is released and the parked job is `held_done`.
void expectThrowingRoundToDrain(IntentPool<> & pool, ManualBarrier & held, const std::atomic<bool> & held_done,
                                const std::function<void()> & wait_until_in_flight, std::string_view held_job,
                                std::string_view failure)
{
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
    wait_until_in_flight();
    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a " << held_job << " was still held";
    EXPECT_FALSE(held_done.load());

    held.release();
    EXPECT_TRUE(round.get()) << "the " << failure << " did not fail the round";
    EXPECT_TRUE(held_done.load());
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

    expectThrowingRoundToDrain(pool, held, held_create_done, [&]
    {
        try
        {
            held.waitUntilArrived();
        }
        catch (...) // Ok: reported as a test failure
        {
            ADD_FAILURE() << "the last create was never in flight beside the first one";
        }
    }, "create", "refused create");
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

    expectThrowingRoundToDrain(pool, held, held_job_done, [&]
    {
        held.waitUntilArrived();
        failed.waitUntilArrived();
    }, "delete job", "failed delete");
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

/// A part write of `payload` that has precommitted and, with `upload`, materialized its blob; `promote` ends it.
/// Without `upload` the entry names the blob with no request on the body.
struct SlowWrite
{
    PartWriteTxnPtr build;
    RootNamespace root;
    String ref_name;
    ManifestId id;
    std::optional<BlobMaterializationAction> action;

    void promote() { build->promote(root, ref_name, build->buildId(), id); }
};

SlowWrite beginSlowWrite(const PoolPtr & store, const String & ns, const String & ref_name, const String & payload,
                         bool upload)
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
    if (!upload)
        build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(root, ref_name, id);
    std::optional<BlobMaterializationAction> action;
    if (upload)
    {
        const BlobUploadResult uploaded = build->uploadBlobDetached(BlobUploadRequest{
            .ref = entry.ref, .source = BlobSource::fromString(payload), .declared_size = payload.size()});
        build->mergeBlobUploadResults(std::span<const BlobUploadResult>(&uploaded, 1));
        action = uploaded.diagnostics.action;
    }
    return SlowWrite{build, root, ref_name, id, action};
}

/// Writes one part holding `payload` through the writer. `Observed` means the writer adopted the stored
/// incarnation; `Published` means it uploaded a fresh one.
BlobMaterializationAction writeOnePart(const PoolPtr & store, const String & ns, const String & ref_name,
                                       const String & payload)
{
    SlowWrite write = beginSlowWrite(store, ns, ref_name, payload, /*upload=*/true);
    write.promote();
    return *write.action;
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
template <typename B>
void truncateBody(IntentPool<B> & pool, uint64_t b)
{
    OperationForTest op(*pool.backend);
    if (const auto meta = (*op).head(pool.bodyKey(b), Retry::standard()))
        ASSERT_EQ((*op).remove(pool.bodyKey(b), meta->etag, Retry::standard()), Removal::Removed);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(pool.bodyKey(b), "short", Retry::standard())));
}

/// Runs a round and fails the test, not the binary, when it throws.
template <typename B>
std::optional<RoundReport> roundWithoutThrowing(IntentPool<B> & pool)
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

/// The rows a scheduled round leaves on file are in its `fold_reduce` row, so a backlog carried round after round is
/// visible in the GC log.
TEST(CASGcIntents, FoldReduceRowCountsTheRowsOnFile)
{
    auto pool = makePool({.create_budget = 1});
    pool.commitThenDrop({1, 2, 3});
    std::vector<FoldMetrics> folds;
    recordFolds(pool, folds);
    const RoundReport report = pool.round();
    ASSERT_EQ(folds.size(), 1u);
    ASSERT_EQ(report.intent_confirmed, 1u) << "positive control: the budget of one create made one row pending";
    EXPECT_EQ(metricOr(folds.front().reduce, "condemned_rows"), 3u);
    EXPECT_EQ(metricOr(folds.front().reduce, "delete_pending_rows"), 1u);
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

namespace
{

size_t bodyHeads(IntentPool<> & pool, const std::vector<uint64_t> & blobs)
{
    const auto requests = blobRequests(pool.backend->journal());
    size_t heads = 0;
    for (uint64_t b : blobs)
        heads += countOf(requests, JournalVerb::Head, pool.bodyKey(b));
    return heads;
}

}

/// A `pending` row over an undecodable body is carried after its `HEAD` and keeps its redelete slot. The next round
/// starts after it, so the row behind it is reclaimed and the budget still bounds the `HEAD`s.
TEST(CASGcIntents, ACarriedPendingRowDoesNotStarveLaterDeletesUnderABudget)
{
    const std::vector<uint64_t> ordered = inRunOrder({1, 2});
    auto pool = makePool({.redelete_budget = 1});
    ASSERT_NO_FATAL_FAILURE(pool.makePending(ordered));
    ASSERT_NO_FATAL_FAILURE(truncateBody(pool, ordered[0]));

    size_t carried_heads = 0;
    size_t rounds = 0;
    for (; rounds < 2 && pool.bodyMeta(ordered[1]).has_value(); ++rounds)
    {
        SCOPED_TRACE(rounds);
        pool.backend->clearJournal();
        ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
        EXPECT_LE(bodyHeads(pool, ordered), 1u) << "the budget of one redelete was exceeded";
        const auto requests = blobRequests(pool.backend->journal());
        carried_heads += countOf(requests, JournalVerb::Head, pool.bodyKey(ordered[0]));
        EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(ordered[0])), 0u) << "the short body was deleted";
        EXPECT_TRUE(isPending(pool.row(ordered[0])));
    }
    EXPECT_FALSE(pool.bodyMeta(ordered[1]).has_value()) << "the row behind the carried row starved";
    EXPECT_GE(carried_heads, 1u) << "positive control: the carried row took the slot";
}

/// With a budget of B, a round sends at most B decision `HEAD`s for `pending` rows, read-ahead included. The rows behind
/// a carried row are reclaimed in three rounds, one fewer than a budget that restarts at the carried row every round.
TEST(CASGcIntents, RedeleteBudgetBoundsThePendingHeadsOfARound)
{
    const std::vector<uint64_t> ordered = inRunOrder({1, 2, 3, 4, 5});
    const std::vector<uint64_t> healthy(ordered.begin() + 1, ordered.end());
    for (const uint64_t concurrency : {uint64_t{1}, uint64_t{4}})
    {
        SCOPED_TRACE(concurrency);
        auto pool = makePool({.redelete_budget = 2, .io_concurrency = concurrency});
        ASSERT_NO_FATAL_FAILURE(pool.makePending(ordered));
        ASSERT_NO_FATAL_FAILURE(truncateBody(pool, ordered[0]));

        const auto anyHealthyBody = [&]
        {
            return std::any_of(healthy.begin(), healthy.end(), [&](uint64_t b) { return pool.bodyMeta(b).has_value(); });
        };
        size_t rounds = 0;
        uint64_t deleted = 0;
        for (; rounds < 3 && anyHealthyBody(); ++rounds)
        {
            SCOPED_TRACE(rounds);
            pool.backend->clearJournal();
            const auto report = roundWithoutThrowing(pool);
            ASSERT_TRUE(report.has_value());
            deleted += report->bodies_deleted;
            EXPECT_LE(bodyHeads(pool, ordered), 2u) << "the budget of two redeletes was exceeded";
        }
        EXPECT_FALSE(anyHealthyBody()) << "a row behind the carried row starved";
        EXPECT_EQ(deleted, healthy.size()) << "positive control: every healthy body was deleted by the rounds";
        EXPECT_TRUE(isPending(pool.row(ordered[0])));
    }
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

/// Every spared row gets one `HEAD`, after the cut. Above concurrency 1 they are read ahead.
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

    ReadAheadWindowProbe probe(pool, ordered, [&] { pool.backend->mark("cut_complete"); });
    const uint64_t hits_before = counter(ProfileEvents::CASGCReadAheadHit);
    const SpareCounters counters;
    pool.backend->clearJournal();

    const RoundReport report = pool.round();

    EXPECT_EQ(report.spared, cohort.size());
    ASSERT_TRUE(probe.outstanding.has_value()) << "the first spared row never sent its HEAD";
    EXPECT_GE(*probe.outstanding, 2) << "no spare HEAD was read ahead";
    EXPECT_LE(*probe.outstanding, window) << "the read-ahead ran past its window";
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
    beginSlowWrite(store, ns, ref_name, payload, /*upload=*/false).promote();
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
            EXPECT_EQ(writeOnePart(store, ns, "seed", "an unrelated part of " + ns), BlobMaterializationAction::Published);
    }

    /// Publishes the blob in part `p0` of `ns_w`, folds it, and drops the part.
    void publishAndDrop()
    {
        ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
        runGateRound(store, *gc);
        store->dropRef(RootNamespace{ns_w}, "p0");
        store->renewWatermarkOnce();
    }

    /// Ends with `pending(t)` while `w` names `t`.
    void commitPendingUnderAMissedWriter()
    {
        ASSERT_NO_FATAL_FAILURE(publishAndDrop());

        std::optional<BlobMaterializationAction> w;
        backend->armOnHead(body_key, [&] { w = writeOnePart(store, ns_w, "w", payload); });
        runGateRound(store, *gc);
        ASSERT_EQ(w, BlobMaterializationAction::Observed) << "W did not adopt the stored body";
        ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "round 1 did not commit pending(t)";
        t = row()->token;
    }

    /// One round; after the walk of `copy_ns`, when the round walks `next_ns` next, `copy_ns` gets a no-bytes copy
    /// named `copy_ref` and `drop_ns`'s part `drop_ref` is dropped. Expects the body kept by that round.
    RoundReport relayRound(const String & copy_ns, const String & next_ns, const String & copy_ref,
                           const String & drop_ns, const String & drop_ref)
    {
        const size_t from = backend->journal().size();
        const RoundWithCallback round = runRoundWithCallback(
            *gc, RoundInterleavePoint::AfterNamespaceWalk, {.first = copy_ns, .second = next_ns}, [&]
            {
                copyWithoutBytes(store, copy_ns, copy_ref, payload);
                store->dropRef(RootNamespace{drop_ns}, drop_ref);
            });
        store->renewWatermarkOnce();
        EXPECT_EQ(round.fired, 1u) << "the round never walked " << next_ns << " after " << copy_ns;
        expectBodyKept(*backend, *store, body_key, from, round.report);
        return round.report;
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

    std::shared_ptr<HeadHookBackend> backend = std::make_shared<HeadHookBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    const String ns_w;
    const String payload;
    const BlobRef x;
    String body_key;
    PersistedEtag t;
};

struct CrossNamespaceOrder
{
    /// The destination's namespace sorts first, so pass 1 proves its frontier before it reads the source.
    /// Otherwise the source sorts first and the source's records past one read window are read in pass 2.
    bool destination_first;
    /// The control: the copy and the drop land before round 2 starts, so its cut holds both.
    bool before_the_round = false;
    /// The control: the copy and the drop land after the last intake read, so the cut holds the source's edge.
    bool source_read_first = false;
};

/// Arms `event` in the next round of `gc` where `order` puts it: right after the walk of `ns_l` (the walk of the
/// source follows it, in pass 1 or in pass 2), or after the intake for the control. Runs it at once for the
/// control that lands before the round.
std::unique_ptr<ScopedRoundCallback> armCrossNamespaceEvent(Gc & gc, const CrossNamespaceOrder & order, const String & ns_l,
                                                            const String & ns_w, const std::function<void()> & event)
{
    if (order.before_the_round)
    {
        event();
        return nullptr;
    }
    if (order.source_read_first)
        return std::make_unique<ScopedRoundCallback>(gc, RoundInterleavePoint::AfterIntake, RoundInterleaveSelector{}, event);
    return std::make_unique<ScopedRoundCallback>(
        gc, RoundInterleavePoint::AfterNamespaceWalk, RoundInterleaveSelector{.first = ns_l, .second = ns_w}, event);
}

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
/// round walked namespace `L` and before it reads the rest of `W`'s namespace, `L` copies `W`'s entry with no
/// request on the body while `W`'s edge is live, and then `W`'s part is dropped. The body must survive: `L` names it.
TEST_P(CASGcIntentsCrossNamespace, NoBytesCopyBetweenTwoFrontiersKeepsTheBody)
{
    const bool destination_first = GetParam().destination_first;
    const bool before_the_round = GetParam().before_the_round;
    const bool source_read_first = GetParam().source_read_first;
    const String ns_w = destination_first ? "test/b_source" : "test/a_source";
    const String ns_l = destination_first ? "test/a_copy" : "test/b_copy";
    MissedWriterPool pool(ns_w, {}, "cross-namespace-no-bytes-copy");
    const auto & backend = pool.backend;
    const PoolPtr & store = pool.store;
    const String & body_key = pool.body_key;
    const String & payload = pool.payload;
    Gc & gc = *pool.gc;
    const auto row = [&] { return retiredEntryFor(*backend, store->layout(), pool.x); };

    ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    /// Round 1. `W` writes at the decision `HEAD`, after the cut: its edge is missed, it adopts `t` before the
    /// intent exists, and the round commits `pending(t)`.
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());
    const PersistedEtag t = pool.t;

    /// Round 2. After the walk of `L`: the copy, then the drop of `W`'s part.
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
    const auto armed = armCrossNamespaceEvent(gc, GetParam(), ns_l, ns_w, copy_then_drop);
    const size_t round_begin = backend->journal().size();
    const RoundReport round2 = runGateRound(store, gc);
    ASSERT_TRUE(copied) << "the round never reached the hooked point";

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
    const bool newborn = GetParam().newborn;
    const String ns_w = "test/b_source";
    const String ns_l = newborn ? "test/z_newborn" : "test/z_copy";
    MissedWriterPool pool(ns_w, {}, "newborn-namespace-no-bytes-copy");
    const auto & backend = pool.backend;
    const PoolPtr & store = pool.store;
    const String & body_key = pool.body_key;
    const String & payload = pool.payload;
    Gc & gc = *pool.gc;
    const auto row = [&] { return retiredEntryFor(*backend, store->layout(), pool.x); };

    if (!newborn)
        ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());
    const PersistedEtag t = pool.t;

    const size_t round_begin = backend->journal().size();
    const RoundWithCallback round2 = runRoundWithCallback(gc, RoundInterleavePoint::AfterCatalogRead, {}, [&]
    {
        copyWithoutBytes(store, ns_l, "copy", payload);
        store->dropRef(RootNamespace{ns_w}, "w");
    });
    store->renewWatermarkOnce();
    ASSERT_EQ(round2.fired, 1u) << "the catalog-read hook never fired";

    expectBodyKept(*backend, *store, body_key, round_begin, round2.report);
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
/// `pending(u)` although `Y`'s edge is not in the cut. In round 3, after the round walked `L`, a no-bytes
/// copy of `Y`'s part into `L` and the drop of `Y`'s part.
TEST_P(CASGcIntentsTransitionSource, NoBytesCopyOfARepublishedBodyBetweenTwoFrontiersKeepsTheBody)
{
    const bool destination_first = GetParam().destination_first;
    const bool before_the_round = GetParam().before_the_round;
    const bool source_read_first = GetParam().source_read_first;
    const String ns_w = destination_first ? "test/b_source" : "test/a_source";
    const String ns_l = destination_first ? "test/a_copy" : "test/b_copy";
    MissedWriterPool pool(ns_w, {}, "transition-source-no-bytes-copy");
    const auto & backend = pool.backend;
    const PoolPtr & store = pool.store;
    const String & body_key = pool.body_key;
    const String & payload = pool.payload;
    Gc & gc = *pool.gc;
    const auto row = [&] { return retiredEntryFor(*backend, store->layout(), pool.x); };

    ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    ASSERT_NO_FATAL_FAILURE(pool.publishAndDrop());

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
    const auto armed = armCrossNamespaceEvent(gc, GetParam(), ns_l, ns_w, copy_then_drop);
    const size_t round_begin = backend->journal().size();
    const RoundReport round3 = runGateRound(store, gc);
    ASSERT_TRUE(copied) << "the round never reached the hooked point";

    expectBodyKept(*backend, *store, body_key, round_begin, round3);
    expectRowOutcome(row(), /*carried=*/!before_the_round && !source_read_first, u);
}

INSTANTIATE_TEST_SUITE_P(CASOrder, CASGcIntentsTransitionSource, kCrossNamespaceOrders, crossNamespaceOrderName);

/// ---- A pending row whose blob the round touched is carried ----

/// A relay over three namespaces: in each round the copy lands after the walk of its destination and before the
/// walk of its source, and its source is dropped. The body survives every round, and the last copy keeps it in a quiet round.
TEST(CASGcIntentsRelay, ThreeNamespaceRelayKeepsTheBody)
{
    MissedWriterPool pool("test/c_source", {"test/a_last", "test/b_middle"}, "three-namespace-relay");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());

    pool.relayRound("test/b_middle", "test/c_source", "copy", "test/c_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);
    pool.relayRound("test/a_last", "test/b_middle", "copy", "test/b_middle", "copy");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    const size_t from = pool.backend->journal().size();
    const RoundReport quiet = runGateRound(pool.store, *pool.gc);
    expectBodyKept(*pool.backend, *pool.store, pool.body_key, from, quiet);
    expectRowOutcome(pool.row(), /*carried=*/false, pool.t);
}

/// A relay over two namespaces whose second copy goes back into the first: the source sorts first, so the first
/// copy lands after the destination's walk while the source's records past one read window wait for pass 2.
TEST(CASGcIntentsRelay, TwoNamespaceRelayThatCopiesBackKeepsTheBody)
{
    MissedWriterPool pool("test/a_source", {"test/b_copy"}, "two-namespace-relay-back");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());

    pool.relayRound("test/b_copy", "test/a_source", "copy", "test/a_source", "w");
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    /// The source namespace now sorts first as the destination of the back copy.
    pool.relayRound("test/a_source", "test/b_copy", "back", "test/b_copy", "copy");
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
    pool.relayRound("test/a_copy", "test/b_source", "copy", "test/b_source", "w");
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
    pool.relayRound("test/a_copy", "test/b_source", "copy", "test/b_source", "w");
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

/// ---- A leader deposed while its round was paused ----

namespace
{

/// Answers a `HEAD` of `gc/state` as the test sets it.
class PausedLeaderBackend : public HeadHookBackend
{
public:
    enum class StateHead : uint8_t
    {
        Store,
        Absent,
        Unanswered,
        Refused,
    };

    std::atomic<StateHead> state_head{StateHead::Store};
    /// Every token of `gc/state` is a weak ETag: a valid precondition with no canonical identity.
    std::atomic<bool> weak_state_tokens{false};

    /// A `GET` of this key answers absent, as after another leader's retention removed it.
    void hideReads(const String & key)
    {
        std::lock_guard lock(hidden_mutex);
        hidden_key = key;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        {
            std::lock_guard lock(hidden_mutex);
            if (key == hidden_key)
                return std::nullopt;
        }
        auto raw = HeadHookBackend::read(key, access);
        if (raw && weakened(key))
            raw->value = weak(raw->value);
        return raw;
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        std::optional<String> stored_form = expected_value;
        if (stored_form && weakened(key) && stored_form->starts_with("W/\"") && stored_form->ends_with("\""))
            stored_form = stored_form->substr(3, stored_form->size() - 4);
        auto written = HeadHookBackend::write(key, bytes, stored_form, access);
        if (written && weakened(key))
            return weak(*written);
        return written;
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = HeadHookBackend::head(key, access);
        if (!key.ends_with("/gc/state"))
            return meta;
        if (meta && weakened(key))
            meta->value = weak(meta->value);
        switch (state_head.load())
        {
            case StateHead::Store:
                return meta;
            case StateHead::Absent:
                return std::nullopt;
            case StateHead::Unanswered:
                throw Poco::TimeoutException("the gc/state HEAD was never answered");
            case StateHead::Refused:
                throw std::runtime_error("the store refused the gc/state HEAD");
        }
        return meta;
    }

private:
    std::mutex hidden_mutex;
    String hidden_key;

    bool weakened(const String & key) const { return weak_state_tokens && key.ends_with("/gc/state"); }
    static String weak(const String & value) { return "W/\"" + value + "\""; }
};

using StateHead = PausedLeaderBackend::StateHead;

std::vector<size_t> stateHeadIndices(const std::vector<JournalEntry> & journal)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Head && journal[i].keys.front().ends_with("/gc/state"))
            out.push_back(i);
    return out;
}

/// Leader A holds a row of the blob of `payload` while writer W's precommit is the only record of W's namespace
/// above A's cursor. A's next round parks after the walk of a quiet namespace and before the walk of W's. Meanwhile
/// W finishes and publishes a snapshot past that record, and leader B takes the lease, folds W's records, commits
/// and removes the logs below the snapshot, that record included.
struct PausedLeaderPool
{
    /// B commits up to five rounds while A is parked. With the default retention B removes the parent runs A's
    /// reduce reads; 16 keeps them.
    explicit PausedLeaderPool(const String & payload_, uint64_t generations_to_keep = 16)
        : payload(payload_), x(idOf(payload_))
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
        /// No snapshot covers W's precommit before A parks.
        config.snapshot_log_count_threshold = 5;
        config.gc_snapshot_generations_to_keep = generations_to_keep;
        store = Pool::open(backend, std::move(config));
        body_key = store->layout().blobKey(x);
        gc_a = std::make_unique<Gc>(store, kGateLeaderA);
        gc_b = std::make_unique<Gc>(store, kGateLeaderB);
    }

    /// A part of the blob, committed in a round of A and dropped: A's next round condemns it.
    void seed()
    {
        ASSERT_EQ(writeOnePart(store, ns_quiet, "seed", "a part of the namespace A walks first"),
                  BlobMaterializationAction::Published);
        backend->clearJournal();
        ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
        stream = firstWrittenStreamPrefix(*backend);
        ASSERT_FALSE(stream.empty());
        runGateRound(store, *gc_a);
        store->dropRef(RootNamespace{ns_w}, "p0");
        store->renewWatermarkOnce();
    }

    std::optional<RetiredEntry> row() const { return retiredEntryFor(*backend, store->layout(), x); }

    bool snapshotAbove(const RefTxnId & id) const
    {
        const std::optional<RefTxnId> snapshot = store->newestPublishedSnapshotIdForTest(RootNamespace{ns_w});
        return snapshot && id < *snapshot;
    }

    /// The last record written to W's namespace.
    String lastLogWritten() const
    {
        String last;
        for (const JournalEntry & entry : backend->journal())
            if (entry.verb == JournalVerb::Write && entry.keys.front().starts_with(stream)
                && entry.keys.front().find("/_log/") != String::npos)
                last = entry.keys.front();
        return last;
    }

    /// Arms A's next round to park before the walk of W's namespace; `finish_writer` runs there. `precommit_key` is
    /// W's precommit record, the one A expects next.
    void parkLeaderA(std::function<void()> finish_writer, const String & precommit_key)
    {
        parked = std::make_unique<ScopedRoundCallback>(
            *gc_a, RoundInterleavePoint::AfterNamespaceWalk, RoundInterleaveSelector{.first = ns_quiet, .second = ns_w},
            [this, finish = std::move(finish_writer), key = precommit_key]
        {
            parked_key = key;
            parked_key_present = objectPresent(*backend, key);
            const RefTxnId parked_id = store->layout().parseRefObjectKey(key)->txn_id;
            finish();
            /// A background publish that meets the lane mid-append backs off on the boot clock, so writes alone may
            /// leave no snapshot for a while. The snapshot is published here once the background attempts settled.
            store->waitForSnapshotPublishSettleForTest(RootNamespace{ns_w});
            (void)store->tryPublishSnapshotAndAdvanceCheckpointOnce(RootNamespace{ns_w});
            snapshot_above_parked = snapshotAbove(parked_id);
            successor_observed = !runGateRound(store, *gc_b).acquired_lease;
            successor_acquired = runGateRound(store, *gc_b).acquired_lease;
            for (size_t i = 0; i < 4 && objectPresent(*backend, key); ++i)
                runGateRound(store, *gc_b);
            parked_key_cleaned = !objectPresent(*backend, key);
            /// B folds new parts until its retention removes one of these keys.
            for (size_t i = 0; i < 8 && !prune_until_absent.empty() && !anyAbsent(prune_until_absent); ++i)
            {
                writeOnePart(store, ns_w, "successor" + std::to_string(i), "successor part " + std::to_string(i));
                runGateRound(store, *gc_b);
            }
            resumed_at = backend->journal().size();
        });
    }

    /// A's parked round. Its commit loses to B and throws `ABORTED`; the round must have reached the validation
    /// read first, and it saw B's version there.
    void runLeaderA()
    {
        std::map<String, UInt64> reduce;
        gc_a->setPhaseSink([&reduce](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_reduce")
                reduce = record.metrics;
        });
        try
        {
            runGateRound(store, *gc_a);
        }
        catch (const DB::Exception & e)
        {
            ASSERT_EQ(e.code(), DB::ErrorCodes::ABORTED) << e.message();
        }
        ASSERT_EQ(parked->fired(), 1u) << "A's round never walked W's namespace after the quiet one";
        parked.reset();
        ASSERT_EQ(metricOr(reduce, "parent_unconfirmed"), 1u) << "A's round never reached the validation read";
    }

    /// A's parked round when B removed what the round reads: it must end as a lost leadership that carries
    /// `original_failure`, the read that found the object gone.
    void runLeaderAExpectingLostLeadership(const String & original_failure)
    {
        std::optional<int> code;
        String message;
        try
        {
            runGateRound(store, *gc_a);
        }
        catch (const DB::Exception & e)
        {
            code = e.code();
            message = e.message();
        }
        ASSERT_EQ(parked->fired(), 1u) << "A's round never reached the point where it parks";
        parked.reset();
        ASSERT_TRUE(code.has_value()) << "A's round did not fail";
        EXPECT_EQ(*code, DB::ErrorCodes::ABORTED) << message;
        EXPECT_NE(message.find("gc/state moved during the round"), String::npos) << message;
        EXPECT_NE(message.find(original_failure), String::npos) << message;
    }

    /// The key of the fold seal `gc/state` adopts now.
    String adoptedSealKey() const
    {
        OperationForTest op(*backend);
        const GcState state = decodeGcState((*op).read(store->layout().gcStateKey(), Retry::standard())->bytes);
        return store->layout().foldSealKey(state.snap_generation, state.snap_attempt);
    }

    /// The keys of the runs the adopted fold seal names.
    std::vector<String> adoptedRunKeys() const
    {
        OperationForTest op(*backend);
        std::vector<String> keys;
        for (const RunRef & run : decodeFoldSeal((*op).read(adoptedSealKey(), Retry::standard())->bytes).blob_target_runs)
            keys.push_back(run.key);
        return keys;
    }

    bool anyAbsent(const std::vector<String> & keys) const
    {
        return std::any_of(keys.begin(), keys.end(), [this](const String & key) { return !objectPresent(*backend, key); });
    }

    void expectDeposedAndCleaned() const
    {
        ASSERT_FALSE(parked_key.empty()) << "A never read W's namespace";
        EXPECT_TRUE(parked_key_present) << "the record after A's cursor was not stored when A parked";
        const auto journal = backend->journal();
        const auto first_read = std::find_if(journal.begin() + resumed_at, journal.end(), [&](const JournalEntry & entry)
        {
            return entry.verb == JournalVerb::Read && entry.keys.front().starts_with(stream)
                && entry.keys.front().find("/_log/") != String::npos;
        });
        ASSERT_NE(first_read, journal.end()) << "A did not read W's namespace after it resumed";
        EXPECT_EQ(first_read->keys.front(), parked_key) << "A's first read after it resumed is not W's precommit";
        EXPECT_TRUE(snapshot_above_parked) << "W published no snapshot past the parked record";
        EXPECT_TRUE(successor_observed) << "B's first round must only observe A";
        EXPECT_TRUE(successor_acquired) << "B never took the lease";
        EXPECT_TRUE(parked_key_cleaned) << "B's cleanup did not remove the parked record";
        EXPECT_EQ(storedLeaseOwner(*backend, store->layout()), kGateLeaderB);
    }

    std::shared_ptr<PausedLeaderBackend> backend = std::make_shared<PausedLeaderBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc_a;
    std::unique_ptr<Gc> gc_b;
    const String ns_quiet = "test/a_quiet";
    const String ns_w = "test/w";
    const String payload;
    const BlobRef x;
    String body_key;
    String stream;

    std::unique_ptr<ScopedRoundCallback> parked;
    std::vector<String> prune_until_absent;
    size_t resumed_at = 0;
    String parked_key;
    bool parked_key_present = false;
    bool snapshot_above_parked = false;
    bool successor_observed = false;
    bool successor_acquired = false;
    bool parked_key_cleaned = false;
};

}

/// A commits `pending(t)` while W, which adopted `t` after the cut, has only precommitted. A's next round parks
/// before it reads W's precommit, which B then removes: A must not read the absence as the end of W's records.
TEST(CASGcIntents, PausedLeaderWhoseLogWasCleanedDeletesNothing)
{
    PausedLeaderPool pool("paused-leader-body");
    ASSERT_NO_FATAL_FAILURE(pool.seed());

    std::optional<SlowWrite> w;
    String precommit_key;
    pool.backend->armOnHead(pool.body_key, [&]
    {
        w = beginSlowWrite(pool.store, pool.ns_w, "w", pool.payload, /*upload=*/true);
        precommit_key = pool.lastLogWritten();
    });
    runGateRound(pool.store, *pool.gc_a);
    ASSERT_TRUE(w && w->action == BlobMaterializationAction::Observed) << "W did not adopt the stored body";
    ASSERT_TRUE(pool.row() && pool.row()->delete_pending && !pool.row()->standing) << "A did not commit pending(t)";

    pool.parkLeaderA([&] { w->promote(); }, precommit_key);
    const size_t from = pool.backend->journal().size();
    ASSERT_NO_FATAL_FAILURE(pool.runLeaderA());
    ASSERT_NO_FATAL_FAILURE(pool.expectDeposedAndCleaned());

    const auto journal = pool.backend->journal();
    const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.body_key) + countOf(requests, JournalVerb::RemoveMany, pool.body_key), 0u)
        << "the deposed leader deleted the body";
    EXPECT_TRUE(objectPresent(*pool.backend, pool.body_key)) << "W's part names a deleted body";
    const FsckReport report = runFsck(*pool.store, /*detail=*/true);
    EXPECT_TRUE(report.clean()) << formatFsckSummary(report);
}

/// The same pause over `standing(t)`: W's precommit names the blob, and A must not delete `I(B,t)`.
TEST(CASGcIntents, PausedLeaderWhoseLogWasCleanedDeletesNoIntent)
{
    PausedLeaderPool pool("paused-leader-intent");
    ASSERT_NO_FATAL_FAILURE(pool.seed());
    runGateRound(pool.store, *pool.gc_a);
    runGateRound(pool.store, *pool.gc_a);
    ASSERT_TRUE(pool.row() && pool.row()->standing) << "A did not commit standing(t)";
    const String intent_key = pool.store->layout().blobIntentKey(pool.x, *pool.row()->token.canonical());
    ASSERT_TRUE(objectPresent(*pool.backend, intent_key));
    ASSERT_FALSE(objectPresent(*pool.backend, pool.body_key)) << "the standing round deletes the body first";

    SlowWrite w = beginSlowWrite(pool.store, pool.ns_w, "w", pool.payload, /*upload=*/false);
    pool.parkLeaderA([&] { w.promote(); }, pool.lastLogWritten());
    const size_t from = pool.backend->journal().size();
    ASSERT_NO_FATAL_FAILURE(pool.runLeaderA());
    ASSERT_NO_FATAL_FAILURE(pool.expectDeposedAndCleaned());

    const auto journal = pool.backend->journal();
    const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
    EXPECT_EQ(intentDeletes(requests), 0u) << "the deposed leader deleted the intent";
    EXPECT_TRUE(objectPresent(*pool.backend, intent_key));
    const FsckReport report = runFsck(*pool.store, /*detail=*/true);
    /// A deleted the body in its `standing` round, before W adopted it without a request, so W's entry is
    /// the one dangling reference. The intent must not turn up as a second finding.
    EXPECT_EQ(report.dangling, 1u) << formatFsckSummary(report);
    for (const FsckObject & object : report.objects)
        if (object.cls == FsckClass::Dangling)
            EXPECT_EQ(object.key, pool.body_key);
    EXPECT_EQ(report.intent_malformed + report.intent_unread + report.pending_without_intent
                  + report.pending_intent_absent + report.pending_token_rejected + report.stale_edge,
              0u) << formatFsckSummary(report);
    EXPECT_EQ(report.intent_listed, 1u) << formatFsckSummary(report);
}

/// The same pause at the default retention: B's retention removes the parent runs A's reduce reads. A newer leader
/// removing what a deposed one reads is a lost leadership, not corruption, and A deletes nothing.
TEST(CASGcIntents, PausedLeaderAtTheDefaultRetentionEndsItsRoundAsLostLeadership)
{
    PausedLeaderPool pool("paused-leader-default-retention", PoolConfig{}.gc_snapshot_generations_to_keep);
    ASSERT_NO_FATAL_FAILURE(pool.seed());

    std::optional<SlowWrite> w;
    String precommit_key;
    pool.backend->armOnHead(pool.body_key, [&]
    {
        w = beginSlowWrite(pool.store, pool.ns_w, "w", pool.payload, /*upload=*/true);
        precommit_key = pool.lastLogWritten();
    });
    runGateRound(pool.store, *pool.gc_a);
    ASSERT_TRUE(w && w->action == BlobMaterializationAction::Observed) << "W did not adopt the stored body";
    ASSERT_TRUE(pool.row() && pool.row()->delete_pending && !pool.row()->standing) << "A did not commit pending(t)";

    pool.prune_until_absent = pool.adoptedRunKeys();
    ASSERT_FALSE(pool.prune_until_absent.empty());
    pool.parkLeaderA([&] { w->promote(); }, precommit_key);
    const size_t from = pool.backend->journal().size();
    ASSERT_NO_FATAL_FAILURE(pool.runLeaderAExpectingLostLeadership("is absent"));
    EXPECT_TRUE(pool.anyAbsent(pool.prune_until_absent)) << "B's retention kept the parent runs A reads";
    ASSERT_NO_FATAL_FAILURE(pool.expectDeposedAndCleaned());

    const auto journal = pool.backend->journal();
    const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.body_key) + countOf(requests, JournalVerb::RemoveMany, pool.body_key), 0u)
        << "the deposed leader deleted the body";
    EXPECT_TRUE(objectPresent(*pool.backend, pool.body_key)) << "W's part names a deleted body";
}

/// A leader parked after its catalog read, while B folds new parts and its retention removes the fold seal A adopted:
/// A's read of that seal finds nothing, which is a lost leadership, not corruption.
TEST(CASGcIntents, PausedLeaderWhoseAdoptedSealWasPrunedEndsItsRoundAsLostLeadership)
{
    PausedLeaderPool pool("paused-leader-pruned-seal", PoolConfig{}.gc_snapshot_generations_to_keep);
    ASSERT_NO_FATAL_FAILURE(pool.seed());
    runGateRound(pool.store, *pool.gc_a);

    const std::vector<String> adopted_keys{pool.adoptedSealKey()};
    bool b_took_over = false;
    pool.parked = std::make_unique<ScopedRoundCallback>(
        *pool.gc_a, RoundInterleavePoint::AfterCatalogRead, RoundInterleaveSelector{}, [&]
    {
        EXPECT_FALSE(runGateRound(pool.store, *pool.gc_b).acquired_lease) << "B's first round must only observe A";
        b_took_over = runGateRound(pool.store, *pool.gc_b).acquired_lease;
        for (size_t i = 0; i < 8 && objectPresent(*pool.backend, adopted_keys.front()); ++i)
        {
            writeOnePart(pool.store, pool.ns_w, "while-a-parks" + std::to_string(i), "while A parks " + std::to_string(i));
            runGateRound(pool.store, *pool.gc_b);
        }
    });
    ASSERT_NO_FATAL_FAILURE(pool.runLeaderAExpectingLostLeadership("adopted fold seal"));
    EXPECT_TRUE(b_took_over) << "B never took the lease";
    EXPECT_FALSE(objectPresent(*pool.backend, adopted_keys.front())) << "B's retention kept the seal A adopted";
    EXPECT_EQ(storedLeaseOwner(*pool.backend, pool.store->layout()), kGateLeaderB);
}

namespace
{

String adoptedSealKeyOf(const BackendPtr & backend, const Layout & layout)
{
    OperationForTest op(backend);
    const GcState state = decodeGcState((*op).read(layout.gcStateKey(), Retry::standard())->bytes);
    return layout.foldSealKey(state.snap_generation, state.snap_attempt);
}

/// Runs a round whose reads of the adopted fold seal answer absent from the phase `hide_after` on (from the start
/// when empty), with the `HEAD` of `gc/state` answered as `answer`. Returns the round's error code and message.
std::pair<int, String> roundWithAdoptedSealGone(StateHead answer, const String & hide_after)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    pool.makePending({1});
    const String seal_key = adoptedSealKeyOf(pool.backend, pool.layout());
    pool.backend->state_head = answer;
    if (hide_after.empty())
        pool.backend->hideReads(seal_key);
    else
        pool.gc->setPhaseSink([&](const GcPhaseRecord & record)
        {
            if (record.phase == hide_after)
                pool.backend->hideReads(seal_key);
        });
    try
    {
        pool.round();
    }
    catch (const DB::Exception & e)
    {
        return {e.code(), e.message()};
    }
    return {0, ""};
}

}

/// The pre-fold drain and the fold each read the adopted seal once more. A seal gone under a moved `gc/state` is a
/// lost leadership; gone under an unchanged one it is corruption. Neither message advises a command.
TEST(CASGcIntents, AdoptedSealGoneAtTheDrainOrTheFoldIsALostLeadershipOnlyWhenTheStateMoved)
{
    struct Site
    {
        const char * hide_after;
        const char * failure;
        bool states_the_consequence;
    };
    for (const Site site : {Site{"", "pre-fold drain: adopted parent seal", false},
                            Site{"parent_seal_read", "the adopted fold seal (generation", true}})
    {
        SCOPED_TRACE(site.failure);
        const auto [moved_code, moved_message] = roundWithAdoptedSealGone(StateHead::Absent, site.hide_after);
        EXPECT_EQ(moved_code, DB::ErrorCodes::ABORTED) << moved_message;
        EXPECT_NE(moved_message.find("gc/state moved during the round"), String::npos) << moved_message;
        EXPECT_NE(moved_message.find(site.failure), String::npos) << moved_message;
        EXPECT_EQ(moved_message.find("cas-gc-rebuild"), String::npos) << moved_message;

        const auto [same_code, same_message] = roundWithAdoptedSealGone(StateHead::Store, site.hide_after);
        EXPECT_EQ(same_code, DB::ErrorCodes::CORRUPTED_DATA) << same_message;
        EXPECT_NE(same_message.find(site.failure), String::npos) << same_message;
        EXPECT_EQ(same_message.find("cas-gc-rebuild"), String::npos) << same_message;
        EXPECT_EQ(same_message.find("GC stops reclaiming on this pool; stored data is not affected") != String::npos,
                  site.states_the_consequence) << same_message;
    }
}

/// The validation read of `gc/state` answers with absence, or never answers: the round deletes nothing, throws
/// nothing and carries the row. The next round, answered honestly, deletes the body.
TEST(CASGcIntents, UnconfirmedLeasedStateSuppressesTheRound)
{
    for (const StateHead answer : {StateHead::Absent, StateHead::Unanswered})
    {
        SCOPED_TRACE(answer == StateHead::Absent ? "absent" : "unanswered");
        IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
        ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
        std::map<String, UInt64> reduce;
        pool.gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
        {
            if (record.phase == "fold_reduce")
                reduce = record.metrics;
        });

        pool.backend->state_head = answer;
        pool.backend->clearJournal();
        RoundReport report;
        ASSERT_NO_THROW(report = pool.round());
        const auto journal = pool.backend->journal();
        EXPECT_GE(stateHeadIndices(journal).size(), 1u) << "positive control: the round sent its validation read";
        const auto requests = blobRequests(journal);
        EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(1)), 0u);
        EXPECT_EQ(intentDeletes(requests), 0u);
        EXPECT_EQ(report.bodies_deleted, 0u);
        EXPECT_TRUE(pool.bodyMeta(1).has_value());
        EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending && !pool.row(1)->standing);
        EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 1u);
        EXPECT_EQ(metricOr(reduce, "suppress_destructive"), 1u);

        pool.backend->state_head = StateHead::Store;
        reduce.clear();
        report = pool.round();
        EXPECT_EQ(report.bodies_deleted, 1u) << "the confirmed round did not delete the body";
        EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 0u);
    }
}

/// A round whose destructive work is suppressed takes no redelete slot, so it keeps the position the previous round
/// left: the round after it still starts behind the carried row.
TEST(CASGcIntents, ASuppressedRoundKeepsTheRedeletePosition)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>(), {.redelete_budget = 1});
    const std::vector<uint64_t> ordered = inRunOrder({1, 2});
    ASSERT_NO_FATAL_FAILURE(pool.makePending(ordered));
    ASSERT_NO_FATAL_FAILURE(truncateBody(pool, ordered[0]));
    std::map<String, UInt64> reduce;
    pool.gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_reduce")
            reduce = record.metrics;
    });

    ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
    ASSERT_TRUE(isPending(pool.row(ordered[0])));
    ASSERT_TRUE(pool.bodyMeta(ordered[1]).has_value()) << "positive control: the carried row took the only slot";

    pool.backend->state_head = StateHead::Absent;
    ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
    ASSERT_EQ(metricOr(reduce, "suppress_destructive"), 1u) << "positive control: the round was suppressed";
    ASSERT_TRUE(pool.bodyMeta(ordered[1]).has_value());

    pool.backend->state_head = StateHead::Store;
    pool.backend->clearJournal();
    ASSERT_TRUE(roundWithoutThrowing(pool).has_value());
    const auto requests = blobRequests(pool.backend->journal());
    EXPECT_EQ(countOf(requests, JournalVerb::Head, pool.bodyKey(ordered[0])), 0u) << "the round started at the carried row";
    EXPECT_FALSE(pool.bodyMeta(ordered[1]).has_value()) << "the row behind the carried row was not reclaimed";
}

/// The validation read is sent when the only `pending` row is in a shard other than the first.
TEST(CASGcIntents, ValidationReadIsSentForAPendingRowInALaterShard)
{
    constexpr uint64_t shards = 4;
    const uint64_t blob = blobsInShard(2, shards, 1).front();
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>(), {.gc_shards = shards});
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob}));
    for (uint64_t shard = 0; shard < shards; ++shard)
        EXPECT_EQ(currentRetiredSet(*pool.backend, pool.layout(), shard).size(), shard == 2 ? 1u : 0u) << "shard " << shard;

    std::map<String, UInt64> reduce;
    pool.gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_reduce")
            reduce = record.metrics;
    });
    pool.backend->state_head = StateHead::Absent;
    pool.backend->clearJournal();
    const RoundReport report = pool.round();
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 1u);
    EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 1u);
    EXPECT_EQ(report.bodies_deleted, 0u);
    EXPECT_TRUE(pool.bodyMeta(blob).has_value());
}

/// A store whose `gc/state` token has no canonical form: the validation read shows the token the lease write
/// returned, so it confirms the leased version and the body is deleted.
TEST(CASGcIntents, ValidationReadConfirmsAStateTokenWithoutACanonicalForm)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    pool.backend->weak_state_tokens = true;
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    std::map<String, UInt64> reduce;
    pool.gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_reduce")
            reduce = record.metrics;
    });

    pool.backend->clearJournal();
    const RoundReport report = pool.round();
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 1u) << "positive control: the round sent its validation read";
    EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 0u);
    EXPECT_EQ(report.bodies_deleted, 1u);
    EXPECT_FALSE(pool.bodyMeta(1).has_value());
}

/// A seal must carry a summary for every shard. One without the summary of the shard that holds a `pending` row
/// fails the round before any delete, so that row is never deleted without the validation read.
TEST(CASGcIntents, SealWithoutTheSummaryOfAPendingShardFailsTheRoundBeforeAnyDelete)
{
    constexpr uint64_t shards = 4;
    const uint64_t blob = blobsInShard(2, shards, 1).front();
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>(), {.gc_shards = shards});
    ASSERT_NO_FATAL_FAILURE(pool.makePending({blob}));
    {
        OperationForTest op(*pool.backend);
        const GcState state = decodeGcState((*op).read(pool.layout().gcStateKey(), Retry::standard())->bytes);
        const String key = pool.layout().foldSealKey(state.snap_generation, state.snap_attempt);
        const auto sealed = (*op).read(key, Retry::standard());
        ASSERT_TRUE(sealed.has_value());
        CasFoldSeal seal = decodeFoldSeal(sealed->bytes);
        ASSERT_EQ(seal.condemned_summary.at(2).pending_total, 1u);
        seal.condemned_summary.erase(2);
        ASSERT_TRUE(std::holds_alternative<Committed>((*op).replace(key, encodeFoldSeal(seal), sealed->etag, Retry::standard())));
    }

    pool.backend->clearJournal();
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)pool.round(); });
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(blob)), 0u);
    EXPECT_TRUE(pool.bodyMeta(blob).has_value());
}

namespace
{

/// Restates the adopted seal's summary of shard 0 with the one `pending` row of blob 1 as a candidate, and runs a round
/// whose `HEAD` of `gc/state` answers `answer`. The summary states no `pending` row, so the round skips its validation
/// read and merges the shard. Returns the round's error code and message.
std::pair<int, String> roundOverASealThatUnderstatesItsPendingRows(IntentPool<PausedLeaderBackend> & pool, StateHead answer)
{
    const std::optional<RetiredEntry> row = pool.row(1);
    if (!row || !row->delete_pending)
        throw std::runtime_error("the row of blob 1 is not pending");
    restateAdoptedSummary(*pool.backend, pool.layout(), 0,
        CondemnedSummary{.condemned_total = 1, .pending_total = 0, .oldest_nonpending_condemn_round = row->condemn_round});
    pool.backend->state_head = answer;
    pool.backend->clearJournal();
    try
    {
        pool.round();
    }
    catch (const DB::Exception & e)
    {
        return {e.code(), e.message()};
    }
    return {0, ""};
}

}

/// The adopted seal understates the `pending` rows of a shard the round merges: the round fails as corrupt bookkeeping
/// once it has read that shard's run, and deletes nothing.
TEST(CASGcIntents, SealThatUnderstatesThePendingRowsOfAMergedShardFailsTheRoundBeforeAnyDelete)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const auto [code, message] = roundOverASealThatUnderstatesItsPendingRows(pool, StateHead::Store);
    EXPECT_EQ(code, DB::ErrorCodes::CORRUPTED_DATA) << message;
    EXPECT_NE(message.find("hold 1 condemned, 1 pending"), String::npos) << message;
    EXPECT_NE(message.find("states 1 condemned, 0 pending"), String::npos) << message;
    EXPECT_NE(message.find("GC stops reclaiming on this pool"), String::npos) << message;
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(1)), 0u);
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
    EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending && !pool.row(1)->standing);
}

/// The same seal under a leader whose `gc/state` moved: without the validation read nothing else would stop its
/// delete. The round ends as a lost leadership and deletes nothing.
TEST(CASGcIntents, PausedLeaderOverASealThatUnderstatesItsPendingRowsDeletesNothing)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    const auto [code, message] = roundOverASealThatUnderstatesItsPendingRows(pool, StateHead::Absent);
    EXPECT_EQ(code, DB::ErrorCodes::ABORTED) << message;
    EXPECT_NE(message.find("gc/state moved during the round"), String::npos) << message;
    EXPECT_EQ(countOf(blobRequests(pool.backend->journal()), JournalVerb::Remove, pool.bodyKey(1)), 0u);
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
}

/// A refusal of the validation read fails the round, as a refusal of any other read of the fold does.
TEST(CASGcIntents, RefusedValidationReadFailsTheRound)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    pool.backend->state_head = StateHead::Refused;
    pool.backend->clearJournal();
    EXPECT_ANY_THROW(pool.round());
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 1u);
    EXPECT_TRUE(pool.bodyMeta(1).has_value());
    EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending && !pool.row(1)->standing);
}

/// The validation read follows every intake read and the cut, and precedes the first decision `HEAD`.
TEST(CASGcIntents, ValidationReadSitsBetweenTheCutAndTheFirstDecisionHead)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    markCutComplete(*pool.gc, *pool.backend);
    pool.backend->clearJournal();
    const RoundReport report = pool.round();
    ASSERT_EQ(report.bodies_deleted, 1u) << "positive control: the round deleted the body";

    const auto journal = pool.backend->journal();
    const size_t cut = indexOfMark(journal, "cut_complete");
    ASSERT_LT(cut, journal.size());
    size_t first_blob_head = journal.size();
    size_t last_log_read = 0;
    for (size_t i = 0; i < journal.size(); ++i)
    {
        const JournalEntry & entry = journal[i];
        if (entry.verb == JournalVerb::Head && first_blob_head == journal.size() && entry.keys.front().find("/blobs/") != String::npos)
            first_blob_head = i;
        if (entry.verb == JournalVerb::Read && entry.keys.front().find("/_log/") != String::npos)
            last_log_read = i;
    }
    ASSERT_LT(first_blob_head, journal.size());
    const std::vector<size_t> state_heads = stateHeadIndices(journal);
    ASSERT_EQ(state_heads.size(), 1u);
    EXPECT_GT(state_heads.front(), cut);
    EXPECT_GT(state_heads.front(), last_log_read);
    EXPECT_LT(state_heads.front(), first_blob_head);
}

/// A round whose parent holds no `pending` or `standing` row sends no validation read: an idle round and the
/// round that condemns. The round after the condemnation sends one.
TEST(CASGcIntents, RoundsWithoutADeletePendingParentRowSendNoValidationRead)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    pool.backend->clearJournal();
    pool.round();
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 0u) << "idle round";

    pool.commitThenDrop({1});
    pool.backend->clearJournal();
    const RoundReport condemning = pool.round();
    EXPECT_EQ(condemning.intent_confirmed, 1u) << "positive control: the round condemned";
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 0u) << "condemning round";

    pool.backend->clearJournal();
    pool.round();
    EXPECT_EQ(stateHeadIndices(pool.backend->journal()).size(), 1u) << "positive control: the round after it";
}

/// ---- A no-bytes republish whose source is not live ----

/// `republishRef` reads the source, then publishes the destination with no request on the body. A source dropped
/// and reclaimed before the destination precommit must fail the call, and the call must take back its precommit.
TEST(CASSourceLiveness, RepublishRefRefusesASourceDroppedBeforeTheDestinationPrecommit)
{
    auto backend = std::make_shared<IntentDeleteParkingBackend>();
    auto store = openGatePool(backend);
    const String payload = "source-liveness-republish";
    const String body_key = store->layout().blobKey(idOf(payload));
    const RootNamespace from{"test/from"};
    const RootNamespace to{"test/to"};

    ASSERT_EQ(writeOnePart(store, from.string(), "all_1_1_0", payload), BlobMaterializationAction::Published);
    CachedPartFolderAccess access(store);
    Gc gc(store, kGateLeaderA);
    bool reclaimed = false;
    access.setHookForTest(CachedPartFolderAccess::TestHook::BeforeDestinationPublish, [&]
    {
        store->dropRef(from, "all_1_1_0");
        store->renewWatermarkOnce();
        runRoundsUntil(store, gc, [&] { return !objectPresent(*backend, body_key); });
        reclaimed = !objectPresent(*backend, body_key);
    });
    expectThrowsCode(DB::ErrorCodes::NO_REPLICA_HAS_PART, [&] { access.republishRef({from, "all_1_1_0"}, {to, "all_1_1_0"}); });
    access.setHookForTest(CachedPartFolderAccess::TestHook::BeforeDestinationPublish, nullptr);
    ASSERT_TRUE(reclaimed) << "the source's body was not reclaimed inside the window";
    EXPECT_FALSE(access.existsRef({to, "all_1_1_0"}, Freshness::ForceFresh)) << "the copy of a dropped source was promoted";
    EXPECT_TRUE(store->livePrecommitsForTest(to).empty()) << "the refused republish left its precommit";

    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

/// ---- What a round must walk: the intake requirements of the body-delete rule ----

namespace
{

using IntakeBackend = HintHoleBackendOn<HeadHookBackend>;

/// A pool that holds `pending(t)` for one blob nobody references, plus an unrelated part in a stable namespace so
/// that every round has something to walk. A copy of the blob into a namespace the adopted state has never
/// seen is the event under test: the blob's row can only be carried if the round reads that namespace.
class IntakePool
{
public:
    explicit IntakePool(const String & ns_old_ = "test/a_old", uint64_t snapshot_log_count_threshold = 256) : ns_old(ns_old_)
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
        config.snapshot_log_count_threshold = snapshot_log_count_threshold;
        store = Pool::open(backend, std::move(config));
        body_key = store->layout().blobKey(x);
        gc = std::make_unique<Gc>(store, kGateLeaderA);
        EXPECT_EQ(writeOnePart(store, "test/m_stable", "seed", "an unrelated part of the stable namespace"),
                  BlobMaterializationAction::Published);
    }

    /// Ends with `pending(t)` and no reference to the blob.
    void commitPending()
    {
        ASSERT_EQ(writeOnePart(store, ns_old, "p0", payload), BlobMaterializationAction::Published);
        runGateRound(store, *gc);
        store->dropRef(RootNamespace{ns_old}, "p0");
        store->renewWatermarkOnce();
        runGateRound(store, *gc);
        ASSERT_TRUE(row() && row()->delete_pending && !row()->standing) << "the condemning round did not commit pending(t)";
        t = row()->token;
    }

    /// A no-bytes copy of the blob into `ns`, dropped at once: one `+1` and one `-1` of the blob in the first
    /// records of that namespace. Returns the stream prefix of the namespace's life.
    String copyAndDrop(const String & ns, const String & ref_name = "c")
    {
        backend->clearJournal();
        copyWithoutBytes(store, ns, ref_name, payload);
        const String stream = firstWrittenStreamPrefix(*backend);
        EXPECT_FALSE(stream.empty()) << ns;
        store->dropRef(RootNamespace{ns}, ref_name);
        store->renewWatermarkOnce();
        return stream;
    }

    /// Writes parts into `ns` until the writer publishes a snapshot past everything written so far.
    void publishSnapshotAbove(const String & ns)
    {
        const RootNamespace root{ns};
        const std::optional<RefTxnId> before = store->newestPublishedSnapshotIdForTest(root);
        for (size_t i = 0; i < 8 && store->newestPublishedSnapshotIdForTest(root) == before; ++i)
        {
            writeOnePart(store, ns, "filler" + std::to_string(i), "snapshot filler " + std::to_string(i));
            store->waitForSnapshotPublishSettleForTest(root);
        }
        ASSERT_NE(store->newestPublishedSnapshotIdForTest(root), before) << "positive control: a snapshot was published";
        store->renewWatermarkOnce();
    }

    /// The round after journal entry `from` carried `pending(t)` and destroyed nothing; later rounds, which fold no
    /// record of the blob, reclaim it. The last step shows that the carry, not something else, kept the body.
    void expectCarriedThenReclaimed(size_t from, const RoundReport & round)
    {
        expectBodyKept(*backend, *store, body_key, from, round);
        expectRowOutcome(row(), /*carried=*/true, t);
        for (int i = 0; i < 6 && bodyStored(); ++i)
            runGateRound(store, *gc);
        EXPECT_FALSE(bodyStored()) << "the body was not reclaimed once no round saw a record of it";
    }

    std::optional<RetiredEntry> row() const { return retiredEntryFor(*backend, store->layout(), x); }

    bool bodyStored() const
    {
        OperationForTest op(*backend);
        return (*op).read(body_key, Retry::standard()).has_value();
    }

    std::shared_ptr<IntakeBackend> backend = std::make_shared<IntakeBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    const String ns_old;
    const String payload = "intake-requirements-payload";
    const BlobRef x = idOf(payload);
    String body_key;
    PersistedEtag t;
};

}

/// A namespace with no parent row is read from its first record in the round that first sees it: the `+1` of the
/// blob in it carries the row. A round that started at the namespace's end would see a zero it had not earned.
TEST(CASGcIntakeRequirements, LifeWithNoParentRowIsWalkedFromItsFirstRecord)
{
    IntakePool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    pool.copyAndDrop("test/n_new");

    const size_t from = pool.backend->journal().size();
    const RoundReport round = runGateRound(pool.store, *pool.gc);
    pool.expectCarriedThenReclaimed(from, round);
}

/// The new namespace has a published snapshot above its `+1` and `-1` of the blob. The round that first sees it
/// still starts at the life's first record, not at the snapshot, so it sees the add and carries the row.
TEST(CASGcIntakeRequirements, LifeWithAPublishedSnapshotIsWalkedFromItsFirstRecord)
{
    IntakePool pool("test/a_old", /*snapshot_log_count_threshold*/ 2);
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    pool.copyAndDrop("test/n_snapshotted");
    ASSERT_NO_FATAL_FAILURE(pool.publishSnapshotAbove("test/n_snapshotted"));

    const size_t from = pool.backend->journal().size();
    const RoundReport round = runGateRound(pool.store, *pool.gc);
    pool.expectCarriedThenReclaimed(from, round);
}

/// The stream listing omits the new namespace's records, the catalog still lists its life: the round walks it by
/// exact key and carries the row.
TEST(CASGcIntakeRequirements, LifeTheStreamListingOmitsButTheCatalogListsIsWalked)
{
    IntakePool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    const String stream = pool.copyAndDrop("test/n_hidden");
    pool.backend->hidePrefix(stream);

    const size_t from = pool.backend->journal().size();
    const RoundReport round = runGateRound(pool.store, *pool.gc);
    EXPECT_GT(pool.backend->holesServed(), 0u) << "positive control: the listing omitted the life";
    pool.expectCarriedThenReclaimed(from, round);
}

/// A life that is still `Creating` holds no precommit: an append to it is refused and writes nothing, so a round
/// has no edge to miss in it.
TEST(CASGcIntakeRequirements, CreatingLifeRefusesTheAppendOfAPrecommit)
{
    IntakePool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    const Layout & layout = pool.store->layout();
    const RootNamespace ns{"test/n_creating"};

    CasRequests requests = openRequestsForTest(pool.backend);
    CasOperation op = requests.admit();
    /// A creator whose mount slot never existed is not provably dead, so the birth is refused, not completed.
    CasRefCatalog::casAdmitEntry(op, layout, /*gc_shards*/ 1, CatalogEntry{
        .ns = ns, .state = NsState::Creating, .incarnation = hexToU128("00000000000000000000000000000077"),
        .creator = CreatorFence{.server_root_id = "ghost-server", .writer_epoch = 9, .fence_generation = 1}});

    pool.backend->clearJournal();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { copyWithoutBytes(pool.store, ns.string(), "c", pool.payload); });
    for (const JournalEntry & entry : pool.backend->journal())
        if (entry.verb == JournalVerb::Write)
            for (const String & key : entry.keys)
                EXPECT_EQ(key.find("/_log/"), String::npos) << "the refused append left a record: " << key;
    EXPECT_TRUE(pool.row() && pool.row()->delete_pending) << "the row of the blob changed";
}

/// A namespace removed and re-created under the same name: the new life has no parent row and starts from its own
/// first record. The old life is erased from the catalog by the round that decides the blob.
TEST(CASGcIntakeRequirements, RecreatedNamespaceStartsFromItsOwnFirstRecord)
{
    IntakePool pool;
    const String ns = "test/a_reborn";
    const String holder = "test/h_holder";
    const RootNamespace ns_root{ns};

    ASSERT_EQ(writeOnePart(pool.store, ns, "p0", pool.payload), BlobMaterializationAction::Published);
    copyWithoutBytes(pool.store, holder, "keep", pool.payload);
    runGateRound(pool.store, *pool.gc);

    /// Round 1 folds the terminal record of the old life; round 2 erases its catalog row, and the drop of the
    /// holder's copy makes the blob zero in that same round, so round 2 commits `pending(t)`.
    pool.store->dropNamespace(ns_root);
    runGateRound(pool.store, *pool.gc);
    ASSERT_FALSE(pool.row().has_value()) << "the holder's edge should still keep the blob out of the retired set";
    pool.store->dropRef(RootNamespace{holder}, "keep");
    pool.store->renewWatermarkOnce();
    runGateRound(pool.store, *pool.gc);
    ASSERT_TRUE(pool.row() && pool.row()->delete_pending && !pool.row()->standing) << "round 2 did not commit pending(t)";
    pool.t = pool.row()->token;

    const std::optional<NamespaceLifeId> old_life = [&]
    {
        CasRequests requests = openRequestsForTest(pool.backend);
        CasOperation op = requests.admit();
        return CasRefCatalog::lifeIfCataloged(op, pool.store->layout(), ns_root);
    }();
    ASSERT_FALSE(old_life.has_value()) << "the old life is still in the catalog";

    pool.copyAndDrop(ns);
    const size_t from = pool.backend->journal().size();
    const RoundReport round = runGateRound(pool.store, *pool.gc);
    pool.expectCarriedThenReclaimed(from, round);
}

/// The catalog read of a round follows the acquisition of its lease: the round of a leader that steals the
/// lease from the previous one is checked, twice.
TEST(CASGcIntakeRequirements, CatalogReadFollowsTheLeaseAcquisition)
{
    IntakePool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    const Layout & layout = pool.store->layout();

    const auto expectOrder = [&](size_t from, const char * what)
    {
        const auto journal = pool.backend->journal();
        size_t first_lease_write = journal.size();
        size_t first_catalog_read = journal.size();
        for (size_t i = from; i < journal.size(); ++i)
        {
            const JournalEntry & entry = journal[i];
            const bool names_state = std::find(entry.keys.begin(), entry.keys.end(), layout.gcStateKey()) != entry.keys.end();
            const bool names_catalog = std::find(entry.keys.begin(), entry.keys.end(), layout.refCatalogKey()) != entry.keys.end();
            if (entry.verb == JournalVerb::Write && names_state && first_lease_write == journal.size())
                first_lease_write = i;
            if (entry.verb == JournalVerb::Read && names_catalog && first_catalog_read == journal.size())
                first_catalog_read = i;
        }
        ASSERT_LT(first_lease_write, journal.size()) << what << ": positive control: the round acquired its lease";
        ASSERT_LT(first_catalog_read, journal.size()) << what << ": positive control: the round read the catalog";
        EXPECT_LT(first_lease_write, first_catalog_read) << what << ": the catalog was read before the lease";
    };

    for (const UInt128 leader : {kGateLeaderB, hexToU128("000000000000000000000000000000c3")})
    {
        /// The first round of a successor only observes the held lease; the second steals it.
        Gc successor(pool.store, leader);
        ASSERT_FALSE(runGateRound(pool.store, successor).acquired_lease);
        const size_t from = pool.backend->journal().size();
        ASSERT_TRUE(runGateRound(pool.store, successor).acquired_lease);
        ASSERT_EQ(storedLeaseOwner(*pool.backend, layout), leader);
        expectOrder(from, "a round that steals the lease");
    }
}

/// ---- The round-point test hook ----

namespace
{

/// Two namespaces with a part each, folded. `refill` gives each namespace one more part, two records past one
/// read window, so both walks continue in pass 2.
class RoundPointPool
{
public:
    RoundPointPool()
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
        store = Pool::open(backend, std::move(config));
        gc = std::make_unique<Gc>(store, kGateLeaderA);
        for (const String & ns : {first, second})
        {
            backend->clearJournal();
            EXPECT_EQ(writeOnePart(store, ns, "seed", "the seed of " + ns), BlobMaterializationAction::Published);
            streams[ns] = firstWrittenStreamPrefix(*backend);
            EXPECT_FALSE(streams[ns].empty()) << ns;
        }
        runGateRound(store, *gc);
    }

    void refill(const String & tag)
    {
        for (const String & ns : {first, second})
            EXPECT_EQ(writeOnePart(store, ns, "fill_" + tag, "a filler of " + ns + " " + tag), BlobMaterializationAction::Published);
    }

    /// Ends with `pending(t)` of a blob no part names.
    void commitPending()
    {
        ASSERT_EQ(writeOnePart(store, first, "p0", payload), BlobMaterializationAction::Published);
        runGateRound(store, *gc);
        store->dropRef(RootNamespace{first}, "p0");
        store->renewWatermarkOnce();
        runGateRound(store, *gc);
        const auto row = retiredEntryFor(*backend, store->layout(), idOf(payload));
        ASSERT_TRUE(row && row->delete_pending && !row->standing) << "the condemning round did not commit pending(t)";
    }

    /// Runs one round with a `hook` mark at `point`; returns the journal of that round.
    std::vector<JournalEntry> roundWithMark(RoundInterleavePoint point, const RoundInterleaveSelector & selector = {})
    {
        backend->clearJournal();
        last = runRoundWithCallback(*gc, point, selector, [&] { backend->mark("hook"); });
        store->renewWatermarkOnce();
        return backend->journal();
    }

    bool isLogRead(const JournalEntry & entry, const String & ns) const
    {
        return entry.verb == JournalVerb::Read && entry.keys.front().starts_with(streams.at(ns))
            && entry.keys.front().find("/_log/") != String::npos;
    }

    bool isAnyLogRead(const JournalEntry & entry) const { return isLogRead(entry, first) || isLogRead(entry, second); }

    /// Expects the mark after a read of `walked`'s log with no other log read in between, and the first log read
    /// after the mark in `next`.
    void expectMarkBetween(const std::vector<JournalEntry> & journal, const String & walked, const String & next) const
    {
        const size_t mark = indexOfMark(journal, "hook");
        ASSERT_LT(mark, journal.size());
        size_t before = journal.size();
        for (size_t i = 0; i < mark; ++i)
            if (isAnyLogRead(journal[i]))
                before = i;
        ASSERT_LT(before, journal.size()) << "no log read before the hook";
        EXPECT_TRUE(isLogRead(journal[before], walked)) << "the last log read before the hook is " << journal[before].keys.front();
        size_t after = journal.size();
        for (size_t i = mark + 1; i < journal.size() && after == journal.size(); ++i)
            if (isAnyLogRead(journal[i]))
                after = i;
        ASSERT_LT(after, journal.size()) << "no log read after the hook";
        EXPECT_TRUE(isLogRead(journal[after], next)) << "the first log read after the hook is " << journal[after].keys.front();
    }

    std::shared_ptr<HeadHookBackend> backend = std::make_shared<HeadHookBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc;
    const String first = "test/a_first";
    const String second = "test/b_second";
    const String payload = "round-point-pending";
    std::map<String, String> streams;
    RoundWithCallback last;
};

}

/// The catalog read precedes the hook, and every log read of the round follows it.
TEST(CASGcRoundPoint, AfterCatalogReadPrecedesEveryLogRead)
{
    RoundPointPool pool;
    pool.refill("1");
    const auto journal = pool.roundWithMark(RoundInterleavePoint::AfterCatalogRead);
    ASSERT_EQ(pool.last.fired, 1u);
    const size_t mark = indexOfMark(journal, "hook");
    const String catalog_key = pool.store->layout().refCatalogKey();
    bool catalog_read_before = false;
    for (size_t i = 0; i < mark; ++i)
    {
        catalog_read_before |= journal[i].verb == JournalVerb::Read && journal[i].keys.front() == catalog_key;
        EXPECT_FALSE(pool.isAnyLogRead(journal[i])) << "a log read before the hook: " << journal[i].keys.front();
    }
    EXPECT_TRUE(catalog_read_before);
    EXPECT_TRUE(std::any_of(journal.begin() + mark, journal.end(), [&](const JournalEntry & e) { return pool.isAnyLogRead(e); }))
        << "positive control: the round read the logs";
}

/// Pass 1 walks in name order: the hook between the two walks fires once, and the reverse pair is no site of pass 1
/// unless pass 2 starts with the first namespace.
TEST(CASGcRoundPoint, BetweenNamespacesInPassOneFiresBetweenTheTwoWalks)
{
    RoundPointPool pool;
    pool.refill("1");
    const auto journal = pool.roundWithMark(RoundInterleavePoint::AfterNamespaceWalk, {.first = pool.first, .second = pool.second});
    ASSERT_EQ(pool.last.fired, 1u);
    pool.expectMarkBetween(journal, pool.first, pool.second);

    pool.refill("2");
    const uint64_t round = committedRound(*pool.backend, pool.store->layout()) + 1;
    pool.roundWithMark(RoundInterleavePoint::AfterNamespaceWalk, {.first = pool.second, .second = pool.first});
    EXPECT_EQ(pool.last.fired, round % 2 == 0 ? 1u : 0u)
        << "after the last walk of pass 1 the next walk is the first of pass 2, which round " << round << " picks";
}

/// Pass 2 starts at index `round mod 2`: two rounds walk the two namespaces in both orders, and the hook fires
/// once between them in each.
TEST(CASGcRoundPoint, BetweenNamespacesInPassTwoFiresInBothOrders)
{
    RoundPointPool pool;
    for (const char * tag : {"1", "2"})
    {
        SCOPED_TRACE(tag);
        pool.refill(tag);
        const uint64_t round = committedRound(*pool.backend, pool.store->layout()) + 1;
        const String & walked = round % 2 == 0 ? pool.first : pool.second;
        const String & next = round % 2 == 0 ? pool.second : pool.first;
        const auto journal = pool.roundWithMark(RoundInterleavePoint::AfterNamespaceWalk, {.first = walked, .second = next, .pass = 2});
        ASSERT_EQ(pool.last.fired, 1u);
        pool.expectMarkBetween(journal, walked, next);
    }
}

/// After the intake: the end of the intake and the `gc/state` validation precede the hook, every decision `HEAD`
/// follows. Log reads after the hook belong to the orphan-manifest sweep, which is not part of the intake.
TEST(CASGcRoundPoint, AfterIntakeSitsBetweenTheValidationAndTheFirstDecisionHead)
{
    RoundPointPool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    pool.refill("1");
    markCutComplete(*pool.gc, *pool.backend);
    const auto journal = pool.roundWithMark(RoundInterleavePoint::AfterIntake);
    ASSERT_EQ(pool.last.fired, 1u);
    ASSERT_EQ(pool.last.report.bodies_deleted, 1u) << "positive control: the round decided and deleted the body";
    const size_t mark = indexOfMark(journal, "hook");
    const size_t cut = indexOfMark(journal, "cut_complete");
    ASSERT_LT(cut, journal.size());
    EXPECT_LT(cut, mark);
    const std::vector<size_t> state_heads = stateHeadIndices(journal);
    ASSERT_EQ(state_heads.size(), 1u);
    EXPECT_LT(state_heads.front(), mark);
    const String body_key = pool.store->layout().blobKey(idOf(pool.payload));
    size_t decision_heads = 0;
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Head && journal[i].keys.front() == body_key)
        {
            ++decision_heads;
            EXPECT_GT(i, mark) << "a decision HEAD before the hook";
        }
    EXPECT_GT(decision_heads, 0u);
}

/// Before the commit: the body delete precedes the hook, the `gc/state` commit follows it.
TEST(CASGcRoundPoint, AfterRequestsBeforeCommitSitsBetweenTheDeleteAndTheCommit)
{
    RoundPointPool pool;
    ASSERT_NO_FATAL_FAILURE(pool.commitPending());
    const auto journal = pool.roundWithMark(RoundInterleavePoint::AfterRequestsBeforeCommit);
    ASSERT_EQ(pool.last.fired, 1u);
    const size_t mark = indexOfMark(journal, "hook");
    const String body_key = pool.store->layout().blobKey(idOf(pool.payload));
    const String state_key = pool.store->layout().gcStateKey();
    size_t body_delete = journal.size();
    size_t last_state_write = journal.size();
    for (size_t i = 0; i < journal.size(); ++i)
    {
        if (journal[i].verb == JournalVerb::Remove && journal[i].keys.front() == body_key)
            body_delete = i;
        if (journal[i].verb == JournalVerb::Write && journal[i].keys.front() == state_key)
            last_state_write = i;
    }
    ASSERT_LT(body_delete, journal.size()) << "positive control: the round deleted the body";
    ASSERT_LT(last_state_write, journal.size());
    EXPECT_LT(body_delete, mark);
    EXPECT_GT(last_state_write, mark) << "the round committed before the hook";
}

/// ---- Other events inside a round ----

namespace
{

/// Holds the first single-key delete of `parked_key` before it reaches the store, and again after it landed, so
/// the test can act between the two. Runs a hook once before the first read of a key reaches the store.
class LateDeleteBackend : public HeadHookBackend
{
public:
    ManualBarrier parked;
    ManualBarrier landed;

    void park(const String & key)
    {
        std::lock_guard lock(late_mutex);
        parked_key = key;
    }

    void armBeforeRead(const String & key, std::function<void()> hook)
    {
        std::lock_guard lock(late_mutex);
        read_key = key;
        before_read = std::move(hook);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        bool hold = false;
        {
            std::lock_guard lock(late_mutex);
            if (parked_key && key == *parked_key)
            {
                parked_key.reset();
                hold = true;
            }
        }
        if (hold)
            parked.arriveAndWait();
        const RawRemoval removal = HeadHookBackend::remove(key, expected_value, access);
        if (hold)
        {
            {
                std::lock_guard lock(late_mutex);
                parked_removal = removal;
            }
            landed.arriveAndWait();
        }
        return removal;
    }

    /// What the store answered the parked delete; empty until it landed.
    std::optional<RawRemoval> parkedRemoval()
    {
        std::lock_guard lock(late_mutex);
        return parked_removal;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::function<void()> fire;
        {
            std::lock_guard lock(late_mutex);
            if (before_read && key == read_key)
                fire = std::exchange(before_read, {});
        }
        if (fire)
            fire();
        return HeadHookBackend::read(key, access);
    }

private:
    std::mutex late_mutex;
    std::optional<String> parked_key;
    std::optional<RawRemoval> parked_removal;
    String read_key;
    std::function<void()> before_read;
};

/// One namespace written through the writer path, and two leaders.
struct TwoLeaderPool
{
    TwoLeaderPool()
    {
        PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
        store = Pool::open(backend, std::move(config));
        gc_a = std::make_unique<Gc>(store, kGateLeaderA);
        gc_b = std::make_unique<Gc>(store, kGateLeaderB);
    }

    /// Ends with `pending(t)` for each payload; no part names any of them.
    void commitPending(const std::vector<String> & payloads)
    {
        for (size_t i = 0; i < payloads.size(); ++i)
            ASSERT_EQ(writeOnePart(store, ns, "p" + std::to_string(i), payloads[i]), BlobMaterializationAction::Published);
        runGateRound(store, *gc_a);
        for (size_t i = 0; i < payloads.size(); ++i)
            store->dropRef(RootNamespace{ns}, "p" + std::to_string(i));
        store->renewWatermarkOnce();
        runGateRound(store, *gc_a);
        for (const String & payload : payloads)
        {
            const auto row = retiredEntryFor(*backend, store->layout(), idOf(payload));
            ASSERT_TRUE(row && row->delete_pending && !row->standing) << payload;
        }
    }

    String bodyKey(const String & payload) const { return store->layout().blobKey(idOf(payload)); }

    std::shared_ptr<LateDeleteBackend> backend = std::make_shared<LateDeleteBackend>();
    PoolPtr store;
    std::unique_ptr<Gc> gc_a;
    std::unique_ptr<Gc> gc_b;
    const String ns = "test/w";
};

}

/// Leader A passes its `gc/state` validation; right after it leader B takes the lease. A still sends its body deletes:
/// the one for a blob nobody names lands, and the one for a blob whose writer uploaded a new incarnation after A's
/// decision `HEAD` is refused by its `If-Match` on the row's token. A's commit then fails.
TEST(CASGcRoundPoint, LeaderDeposedAfterTheValidationDeletesOnlyBodiesNobodyNames)
{
    TwoLeaderPool pool;
    const String unowned = "deposed-leader-unowned";
    const String rewritten = "deposed-leader-rewritten";
    ASSERT_NO_FATAL_FAILURE(pool.commitPending({unowned, rewritten}));

    bool b_observed = false;
    bool b_acquired = false;
    ScopedRoundCallback deposed(*pool.gc_a, RoundInterleavePoint::AfterIntake, {}, [&]
    {
        /// B's rounds delete nothing, so A's deletes are the only ones.
        b_observed = !pool.gc_b->runRegularRound({}, /*allow_steal*/ true, UniversePolicy::StageA_Suppressed).acquired_lease;
        b_acquired = pool.gc_b->runRegularRound({}, /*allow_steal*/ true, UniversePolicy::StageA_Suppressed).acquired_lease;
    });
    std::map<String, UInt64> reduce;
    std::optional<BlobMaterializationAction> writer;
    pool.gc_a->setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase != "fold_reduce")
            return;
        reduce = record.metrics;
        /// After A's decision `HEAD`s and before its deletes.
        writer = writeOnePart(pool.store, "test/x_writer", "w", rewritten);
    });

    const size_t from = pool.backend->journal().size();
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { runGateRound(pool.store, *pool.gc_a); });
    pool.gc_a->setPhaseSink({});
    ASSERT_EQ(deposed.fired(), 1u);
    ASSERT_TRUE(b_observed && b_acquired) << "B did not take the lease inside A's round";
    EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 0u) << "B took the lease before A's validation";
    EXPECT_EQ(storedLeaseOwner(*pool.backend, pool.store->layout()), kGateLeaderB) << "A's commit landed after it lost the lease";
    ASSERT_EQ(writer, BlobMaterializationAction::Published) << "the writer met the intent and must upload";

    const auto journal = pool.backend->journal();
    const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(unowned)), 1u) << "positive control: A deleted a body";
    EXPECT_FALSE(objectPresent(*pool.backend, pool.bodyKey(unowned)));
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(rewritten)), 1u) << "A sent its delete for the rewritten blob";
    EXPECT_TRUE(objectPresent(*pool.backend, pool.bodyKey(rewritten))) << "A's delete removed the writer's new incarnation";
    const FsckReport report = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

/// A's body delete is issued and held in the store. A writer of the same content meets the intent and uploads a new
/// incarnation, before the delete lands or between its own `HEAD` and its intent read; the delete then lands. The
/// writer's part names a stored body either way.
TEST(CASGcRoundPoint, LateBodyDeleteLeavesTheIncarnationAWriterUploaded)
{
    for (const bool lands_between_head_and_intent_read : {false, true})
    {
        SCOPED_TRACE(lands_between_head_and_intent_read ? "lands between the writer's HEAD and intent read"
                                                        : "lands after the writer");
        TwoLeaderPool pool;
        const String payload = "late-body-delete";
        ASSERT_NO_FATAL_FAILURE(pool.commitPending({payload}));
        const auto row = retiredEntryFor(*pool.backend, pool.store->layout(), idOf(payload));
        const String intent_key = pool.store->layout().blobIntentKey(idOf(payload), *row->token.canonical());

        pool.backend->park(pool.bodyKey(payload));
        std::thread leader_a([&]
        {
            try
            {
                runGateRound(pool.store, *pool.gc_a);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
                /// Only the store's state is asserted.
            }
        });
        SCOPE_EXIT({
            pool.backend->parked.release();
            pool.backend->landed.release();
            if (leader_a.joinable())
                leader_a.join();
        });
        pool.backend->parked.waitUntilArrived();
        bool delete_landed = false;
        const auto landDelete = [&]
        {
            pool.backend->parked.release();
            pool.backend->landed.waitUntilArrived();
            delete_landed = true;
        };

        if (lands_between_head_and_intent_read)
            pool.backend->armBeforeRead(intent_key, landDelete);
        const BlobMaterializationAction writer = writeOnePart(pool.store, "test/x_writer", "w", payload);
        if (!lands_between_head_and_intent_read)
            landDelete();
        /// Released here too, so a hook that never fired fails the assertion below instead of holding the leader.
        pool.backend->parked.release();
        pool.backend->landed.release();
        leader_a.join();

        ASSERT_TRUE(delete_landed) << "the writer never read the intent, so the delete was not placed inside the writer";
        /// Between the writer's `HEAD` and its intent read the delete still matches the incarnation it was sent for;
        /// after the writer's upload it does not.
        EXPECT_EQ(pool.backend->parkedRemoval(),
                  lands_between_head_and_intent_read ? Backend::RawRemoval::Removed : Backend::RawRemoval::Mismatch);
        EXPECT_EQ(writer, BlobMaterializationAction::Published) << "the writer adopted an incarnation under deletion";
        EXPECT_TRUE(objectPresent(*pool.backend, pool.bodyKey(payload))) << "the late delete removed the writer's body";
        const FsckReport report = runFsck(*pool.store, /*detail=*/true);
        EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
    }
}

/// The validation read of `gc/state` gets no answer; the store answered every earlier read of the round. The round
/// throws nothing, takes no destructive step and reports `parent_unconfirmed`.
TEST(CASGcRoundPoint, LostAnswerOfTheValidationReadSuppressesTheRound)
{
    IntentPool<PausedLeaderBackend> pool(std::make_shared<PausedLeaderBackend>());
    ASSERT_NO_FATAL_FAILURE(pool.makePending({1}));
    std::map<String, UInt64> reduce;
    pool.gc->setPhaseSink([&reduce](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_reduce")
            reduce = record.metrics;
    });

    pool.backend->clearJournal();
    RoundWithCallback round;
    ASSERT_NO_THROW(round = runRoundWithCallback(*pool.gc, RoundInterleavePoint::AfterCatalogRead, {}, [&]
    {
        pool.backend->state_head = StateHead::Unanswered;
    }));
    pool.store->renewWatermarkOnce();
    pool.backend->state_head = StateHead::Store;
    ASSERT_EQ(round.fired, 1u);
    const auto journal = pool.backend->journal();
    EXPECT_GE(stateHeadIndices(journal).size(), 1u) << "positive control: the round sent its validation read";
    const auto requests = blobRequests(journal);
    EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.bodyKey(1)), 0u);
    EXPECT_EQ(intentDeletes(requests), 0u);
    EXPECT_EQ(metricOr(reduce, "parent_unconfirmed"), 1u);
    EXPECT_EQ(metricOr(reduce, "suppress_destructive"), 1u);
    EXPECT_TRUE(pool.row(1) && pool.row(1)->delete_pending && !pool.row(1)->standing);

    EXPECT_EQ(pool.round().bodies_deleted, 1u) << "the answered round did not delete the body";
}

/// The writer's namespace is removed between the reads of a round and created again by a writer of the same content
/// after a later round's catalog read. The removal's records carry the row; the new part names a stored body.
TEST(CASGcRoundPoint, NamespaceRemovedAndRecreatedBetweenTheReads)
{
    MissedWriterPool pool("test/r_table", {"test/a_seed"}, "removed-and-recreated");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());

    const size_t from = pool.backend->journal().size();
    const RoundWithCallback removed = runRoundWithCallback(
        *pool.gc, RoundInterleavePoint::AfterNamespaceWalk, {.first = "test/a_seed", .second = "test/r_table"},
        [&] { pool.store->dropNamespace(RootNamespace{"test/r_table"}); });
    pool.store->renewWatermarkOnce();
    ASSERT_EQ(removed.fired, 1u);
    const auto journal = pool.backend->journal();
    EXPECT_EQ(countOf(blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end())), JournalVerb::Remove,
                      pool.body_key), 0u);
    expectRowOutcome(pool.row(), /*carried=*/true, pool.t);

    /// The next round erases the removed life from the catalog; the round after it sees the re-creation only after
    /// its catalog read.
    runGateRound(pool.store, *pool.gc);
    std::optional<BlobMaterializationAction> again;
    const RoundWithCallback recreated = runRoundWithCallback(*pool.gc, RoundInterleavePoint::AfterCatalogRead, {}, [&]
    {
        again = writeOnePart(pool.store, "test/r_table", "again", pool.payload);
    });
    pool.store->renewWatermarkOnce();
    ASSERT_EQ(recreated.fired, 1u);
    ASSERT_TRUE(again.has_value());
    for (int i = 0; i < 3; ++i)
        runGateRound(pool.store, *pool.gc);
    EXPECT_TRUE(pool.bodyStored()) << "the re-created part names a deleted body";
    FsckReport report = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);

    pool.store->dropRef(RootNamespace{"test/r_table"}, "again");
    pool.store->renewWatermarkOnce();
    for (int i = 0; i < 6 && pool.bodyStored(); ++i)
        runGateRound(pool.store, *pool.gc);
    EXPECT_FALSE(pool.bodyStored()) << "the body was not reclaimed once nothing named it";
}

/// The writer's namespace publishes a snapshot past an add and a drop of the blob that no round has read. The round
/// reads the records, not the snapshot, sees the add and takes no destructive step for the blob.
TEST(CASGcRoundPoint, SnapshotOverAnUnreadAddAndDropTakesNoDestructiveStep)
{
    auto backend = std::make_shared<HeadHookBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    config.snapshot_log_count_threshold = 2;
    auto store = Pool::open(backend, std::move(config));
    const RootNamespace ns{"test/r_table"};
    const String payload = "snapshot-over-add-and-drop";
    const String body_key = store->layout().blobKey(idOf(payload));
    const auto row = [&] { return retiredEntryFor(*backend, store->layout(), idOf(payload)); };
    Gc gc(store, kGateLeaderA);

    ASSERT_EQ(writeOnePart(store, ns.string(), "p0", payload), BlobMaterializationAction::Published);
    runGateRound(store, gc);
    store->dropRef(ns, "p0");
    store->renewWatermarkOnce();
    runGateRound(store, gc);
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing);
    const PersistedEtag t = row()->token;

    /// A copy of the blob and its drop, then parts until a snapshot is published past both.
    const std::optional<RefTxnId> before = store->newestPublishedSnapshotIdForTest(ns);
    copyWithoutBytes(store, ns.string(), "c", payload);
    store->dropRef(ns, "c");
    for (size_t i = 0; i < 8 && store->newestPublishedSnapshotIdForTest(ns) == before; ++i)
    {
        writeOnePart(store, ns.string(), "filler" + std::to_string(i), "snapshot filler " + std::to_string(i));
        store->waitForSnapshotPublishSettleForTest(ns);
    }
    ASSERT_NE(store->newestPublishedSnapshotIdForTest(ns), before) << "positive control: a snapshot past the drop";
    store->renewWatermarkOnce();

    const size_t from = backend->journal().size();
    const RoundReport round = runGateRound(store, gc);
    expectBodyKept(*backend, *store, body_key, from, round);
    expectRowOutcome(row(), /*carried=*/true, t);

    for (int i = 0; i < 6 && objectPresent(*backend, body_key); ++i)
        runGateRound(store, gc);
    EXPECT_FALSE(objectPresent(*backend, body_key)) << "the body was not reclaimed once no round saw a record of it";
}

/// The time budget cuts pass 2, so the writer's namespace is read over two rounds; the copy and the drop land at the
/// cut. The cut round is suppressed; the next one reads the rest and the copy, and keeps the body.
TEST(CASGcRoundPoint, IntakeSliceCutByTheTimeBudgetReadsANamespaceOverTwoRounds)
{
    auto backend = std::make_shared<HeadHookBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0, .gc_io_concurrency = 1};
    auto store = Pool::open(backend, std::move(config));
    const String ns_l = "test/a_copy";
    const String ns_w = "test/b_source";
    const String payload = "intake-slice-cut";
    const String body_key = store->layout().blobKey(idOf(payload));
    auto mono = std::make_shared<std::atomic<uint64_t>>(1'000'000);
    Gc gc(store, kGateLeaderA, {}, [mono] { return mono->load(); });

    ASSERT_EQ(writeOnePart(store, ns_l, "seed", "an unrelated part of the copy's table"), BlobMaterializationAction::Published);
    backend->clearJournal();
    ASSERT_EQ(writeOnePart(store, ns_w, "p0", payload), BlobMaterializationAction::Published);
    const String ns_w_stream = firstWrittenStreamPrefix(*backend);
    runGateRound(store, gc);
    store->dropRef(RootNamespace{ns_w}, "p0");
    store->renewWatermarkOnce();
    std::optional<BlobMaterializationAction> w;
    backend->armOnHead(body_key, [&] { w = writeOnePart(store, ns_w, "w", payload); });
    runGateRound(store, gc);
    ASSERT_EQ(w, BlobMaterializationAction::Observed);
    const auto row = [&] { return retiredEntryFor(*backend, store->layout(), idOf(payload)); };
    ASSERT_TRUE(row() && row()->delete_pending && !row()->standing);

    std::map<String, UInt64> reduce;
    gc.setPhaseSink([&reduce](const GcPhaseRecord & record)
    {
        if (record.phase == "fold_reduce")
            reduce = record.metrics;
    });
    const auto logReadsOfW = [&](size_t begin)
    {
        const auto journal = backend->journal();
        size_t n = 0;
        for (size_t i = begin; i < journal.size(); ++i)
            n += journal[i].verb == JournalVerb::Read && journal[i].keys.front().starts_with(ns_w_stream)
                && journal[i].keys.front().find("/_log/") != String::npos;
        return n;
    };

    /// After the last walk of pass 1, before pass 2 continues the writer's namespace.
    size_t from = backend->journal().size();
    const RoundWithCallback cut = runRoundWithCallback(gc, RoundInterleavePoint::AfterNamespaceWalk, {.first = ns_w, .second = ns_w}, [&]
    {
        mono->fetch_add(Gc::kRefIntakeBudgetMs + 1);
        copyWithoutBytes(store, ns_l, "copy", payload);
        store->dropRef(RootNamespace{ns_w}, "w");
    });
    store->renewWatermarkOnce();
    ASSERT_EQ(cut.fired, 1u);
    EXPECT_EQ(metricOr(reduce, "suppress_destructive"), 1u) << "the cut round was not suppressed";
    EXPECT_GT(logReadsOfW(from), 0u) << "positive control: the cut round read part of the namespace";
    EXPECT_EQ(cut.report.bodies_deleted, 0u);

    from = backend->journal().size();
    runGateRound(store, gc);
    EXPECT_EQ(metricOr(reduce, "suppress_destructive"), 0u);
    EXPECT_GT(logReadsOfW(from), 0u) << "the next round did not read the rest of the namespace";
    const auto journal = backend->journal();
    EXPECT_EQ(countOf(blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end())), JournalVerb::Remove, body_key), 0u);
    EXPECT_FALSE(row().has_value()) << "the copy's edge spares the blob";
    EXPECT_TRUE(objectPresent(*backend, body_key));
    const FsckReport report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

/// A state rebuild between the two rounds of a relay: the rebuilt baseline counts the middle copy, and the second hop
/// keeps the body.
TEST(CASGcRoundPoint, RebuildBetweenTheTwoRoundsOfARelayKeepsTheBody)
{
    MissedWriterPool pool("test/c_source", {"test/a_last", "test/b_middle"}, "rebuild-inside-a-relay");
    ASSERT_NO_FATAL_FAILURE(pool.commitPendingUnderAMissedWriter());
    pool.relayRound("test/b_middle", "test/c_source", "copy", "test/c_source", "w");

    const RebuildReport rebuilt = pool.gc->rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rebuilt.refusal.empty()) << rebuilt.refusal;
    pool.store->renewWatermarkOnce();

    pool.relayRound("test/a_last", "test/b_middle", "copy", "test/b_middle", "copy");
    for (int i = 0; i < 3; ++i)
    {
        const size_t from = pool.backend->journal().size();
        const RoundReport quiet = runGateRound(pool.store, *pool.gc);
        const auto journal = pool.backend->journal();
        const auto requests = blobRequests(std::vector<JournalEntry>(journal.begin() + from, journal.end()));
        EXPECT_EQ(countOf(requests, JournalVerb::Remove, pool.body_key), 0u) << "quiet round " << i << "; deleted=" << quiet.deleted;
    }
    EXPECT_TRUE(pool.bodyStored());
    const FsckReport report = runFsck(*pool.store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u) << formatFsckSummary(report);
}

namespace
{

SourceEdgeRecord inheritedEdge(uint64_t b)
{
    return SourceEdgeRecord{.ref = blobRefOf(b), .source_id = DB::UInt128{7}, .marker = RunMarker::Edge};
}

/// A row for the body now at blob `b`, stamped `condemn_round`.
SourceEdgeRecord inheritedRow(IntentPool<> & pool, uint64_t b, uint64_t condemn_round, bool pending = false)
{
    pool.writeBodies({b});
    return SourceEdgeRecord{.ref = blobRefOf(b), .source_id = DB::UInt128{0}, .marker = RunMarker::Condemned,
                            .delete_pending = pending, .token = PersistedEtag::capture(pool.bodyMeta(b)->etag), .size = 1,
                            .condemn_round = condemn_round};
}

std::vector<SourceEdgeRecord> sortedRecords(std::vector<SourceEdgeRecord> records)
{
    std::sort(records.begin(), records.end(), [](const SourceEdgeRecord & l, const SourceEdgeRecord & r)
    {
        return std::pair{l.ref, l.source_id} < std::pair{r.ref, r.source_id};
    });
    return records;
}

struct InheritedRunRound
{
    int code = 0;
    String message;
    std::vector<Request> requests;
    uint64_t parent_round = 0;
    uint64_t round_after = 0;
    String run_key;
};

/// One round over a seal whose run of `shard` holds the records `make_records` returns for the committed round.
InheritedRunRound roundOverInheritedRun(IntentPool<> & pool, uint64_t shard,
                                        const std::function<std::vector<SourceEdgeRecord>(uint64_t)> & make_records)
{
    pool.round();
    InheritedRunRound result;
    result.parent_round = committedRound(*pool.backend, pool.layout());
    {
        OperationForTest op(*pool.backend);
        const GcState state = decodeGcState((*op).read(pool.layout().gcStateKey(), Retry::standard())->bytes);
        result.run_key = pool.layout().blobTargetRunKey(state.snap_generation + 1, /*attempt*/ 1, shard, /*seq*/ 0);
    }
    injectSourceEdgeRun(*pool.backend, pool.layout(), shard, make_records(result.parent_round));
    pool.backend->clearJournal();
    try
    {
        pool.round();
    }
    catch (const DB::Exception & e)
    {
        result.code = e.code();
        result.message = e.message();
    }
    result.requests = blobRequests(pool.backend->journal());
    result.round_after = committedRound(*pool.backend, pool.layout());
    return result;
}

void expectRefusedAsCorrupt(const InheritedRunRound & round, std::string_view what)
{
    EXPECT_EQ(round.code, DB::ErrorCodes::CORRUPTED_DATA) << round.message;
    EXPECT_NE(round.message.find(what), String::npos) << round.message;
    EXPECT_EQ(round.round_after, round.parent_round) << "the refused round committed";
    for (const auto & [verb, key] : round.requests)
        EXPECT_TRUE(verb != JournalVerb::Remove && verb != JournalVerb::RemoveMany) << "the refused round deleted " << key;
}

/// The refusal is permanent for the pool, so it must name what an operator inspects: the run, the shard and the row.
void expectRefusalNamesTheRow(const InheritedRunRound & round, uint64_t shard, uint64_t blob, uint64_t source_id)
{
    EXPECT_NE(round.message.find("`" + round.run_key + "`"), String::npos) << round.message;
    EXPECT_NE(round.message.find("of GC shard " + std::to_string(shard) + ":"), String::npos) << round.message;
    EXPECT_NE(round.message.find("blob " + blobIdOf(blobRefOf(blob)) + " source " + u128ToHex(DB::UInt128{source_id})),
              String::npos) << round.message;
    EXPECT_TRUE(round.message.ends_with(kCorruptBookkeeping)) << round.message;
}

}

/// A live edge in the run of a shard that does not own its blob would leave the owning shard at zero.
TEST(CASGcIntents, InheritedRunWithAMisplacedLiveEdgeIsRefusedAsCorrupt)
{
    auto pool = makePool({.gc_shards = 2});
    const uint64_t own = blobsInShard(1, 2, 1).front();
    const uint64_t foreign = blobsInShard(0, 2, 1).front();
    const InheritedRunRound round = roundOverInheritedRun(pool, 1, [&](uint64_t parent_round)
    {
        return sortedRecords({inheritedRow(pool, own, parent_round), inheritedEdge(foreign)});
    });
    expectRefusedAsCorrupt(round, "belongs to GC shard 0 of 2, not to shard 1");
    expectRefusalNamesTheRow(round, /*shard*/ 1, foreign, /*source_id*/ 7);
}

/// A `pending` row makes a row decision due, so the round merges the shard rather than defer it.
TEST(CASGcIntents, InheritedRowStampedAfterTheParentRoundIsRefusedAsCorrupt)
{
    auto pool = makePool();
    const InheritedRunRound round = roundOverInheritedRun(pool, 0, [&](uint64_t parent_round)
    {
        return std::vector<SourceEdgeRecord>{inheritedRow(pool, 1, parent_round + 1, /*pending*/ true)};
    });
    expectRefusedAsCorrupt(round, "after the parent round");
    expectRefusalNamesTheRow(round, /*shard*/ 0, /*blob*/ 1, /*source_id*/ 0);
}

TEST(CASGcIntents, InheritedRowStampedWithTheParentRoundIsDecided)
{
    auto pool = makePool();
    const InheritedRunRound round = roundOverInheritedRun(pool, 0, [&](uint64_t parent_round)
    {
        return std::vector<SourceEdgeRecord>{inheritedRow(pool, 1, parent_round)};
    });
    EXPECT_EQ(round.code, 0) << round.message;
    EXPECT_EQ(round.round_after, round.parent_round + 1);
    EXPECT_TRUE(isPending(pool.row(1))) << "the candidate's intent was created";
}

TEST(CASGcIntents, InheritedRunInDescendingOrderIsRefusedAsCorrupt)
{
    auto pool = makePool();
    const InheritedRunRound round = roundOverInheritedRun(pool, 0, [&](uint64_t parent_round)
    {
        std::vector<SourceEdgeRecord> records = sortedRecords({inheritedRow(pool, 1, parent_round), inheritedEdge(2), inheritedEdge(3)});
        std::swap(records[1], records[2]);
        return records;
    });
    expectRefusedAsCorrupt(round, "sorts before the row above it");
    expectRefusalNamesTheRow(round, /*shard*/ 0, /*blob*/ 2, /*source_id*/ 7);
}
