#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <IO/ReadBufferFromMemory.h>

#include "cas_publication_test_support.h"
#include "cas_test_helpers.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

const DB::UInt128 kGcId = hexToU128("00000000000000000000000000000001");

size_t firstIndexOf(const std::vector<JournalEntry> & journal, auto && predicate)
{
    const auto it = std::find_if(journal.begin(), journal.end(), predicate);
    return static_cast<size_t>(it - journal.begin());
}

size_t lastIndexOf(const std::vector<JournalEntry> & journal, auto && predicate)
{
    const auto it = std::find_if(journal.rbegin(), journal.rend(), predicate);
    return it == journal.rend() ? journal.size() : static_cast<size_t>(journal.rend() - it) - 1;
}

/// Two threads take turns in a fixed order; each turn is one request, so the expected journal is the turn order.
class TurnLatch
{
public:
    void waitForTurn(size_t turn)
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return current == turn; });
    }

    void finishTurn()
    {
        {
            std::lock_guard lock(mutex);
            ++current;
        }
        cv.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    size_t current = 0;
};

}

TEST(CASTestSupport, JournalKeepsIssueOrderAcrossThreads)
{
    CountingBackend backend;
    TurnLatch latch;
    const std::vector<String> order = {"k0", "k1", "k2", "k3", "k4", "k5"};

    auto worker = [&](size_t parity)
    {
        OperationForTest op(backend);
        for (size_t turn = parity; turn < order.size(); turn += 2)
        {
            latch.waitForTurn(turn);
            (*op).head(order[turn], Retry::once());
            latch.finishTurn();
        }
    };
    std::thread a(worker, 0);
    std::thread b(worker, 1);
    a.join();
    b.join();

    const auto journal = backend.journal();
    ASSERT_EQ(journal.size(), order.size());
    for (size_t i = 0; i < order.size(); ++i)
    {
        EXPECT_EQ(journal[i].verb, JournalVerb::Head) << i;
        EXPECT_EQ(journal[i].keys, std::vector<String>{order[i]}) << i;
    }
}

TEST(CASTestSupport, BatchDeleteIsOneEntryWithEveryKey)
{
    CountingBackend backend;
    const Layout layout("p");
    const NamespaceLifeId life = fixture::fixtureLife(RootNamespace{"00/aa@cas@"});
    std::vector<String> keys;
    std::vector<WriteOnceKey> batch;
    {
        OperationForTest op(backend);
        for (uint64_t sequence = 1; sequence <= 3; ++sequence)
        {
            const RefTxnId id{1, sequence};
            keys.push_back(layout.refLogKey(life, id));
            batch.push_back(layout.writeOnceRefLogKey(life, id));
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(keys.back(), "x", Retry::once())));
        }
        backend.clearJournal();
        (*op).removeManyWriteOnce(batch, Retry::once());
    }

    const auto journal = backend.journal();
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].verb, JournalVerb::RemoveMany);
    EXPECT_EQ(journal[0].keys, keys);
    EXPECT_EQ(backend.deleteTotal(), keys.size());
}

TEST(CASTestSupport, RefusedRequestIsJournaled)
{
    CountingBackend backend;
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create("p/k", "v1", Retry::once())));
    backend.clearJournal();

    /// A create over an existing key is refused with a conflict.
    EXPECT_TRUE(std::holds_alternative<Conflict>((*op).create("p/k", "v2", Retry::once())));
    /// A read of an absent key finds nothing.
    EXPECT_FALSE((*op).read("p/absent", Retry::once()).has_value());

    const auto journal = backend.journal();
    ASSERT_EQ(journal.size(), 3u);
    EXPECT_EQ(journal[0].verb, JournalVerb::Write);
    EXPECT_EQ(journal[0].keys, std::vector<String>{"p/k"});
    EXPECT_EQ(journal[0].detail, "create");
    /// The engine reads the winner's object to build the conflict answer.
    EXPECT_EQ(journal[1].verb, JournalVerb::Read);
    EXPECT_EQ(journal[1].keys, std::vector<String>{"p/k"});
    EXPECT_EQ(journal[2].verb, JournalVerb::Read);
    EXPECT_EQ(journal[2].keys, std::vector<String>{"p/absent"});
}

TEST(CASTestSupport, CutCompleteFollowsIntakeAndPrecedesTheFold)
{
    constexpr uint64_t kBlobCount = 2;
    auto backend = std::make_shared<CountingBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    auto store = Pool::open(backend, std::move(config));
    Gc gc(store, kGcId);

    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef manifest{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    std::vector<ManifestEntry> entries;
    for (uint64_t blob = 1; blob <= kBlobCount; ++blob)
    {
        writeBlobBody(*backend, store->layout(), numberedBlobHash(blob));
        entries.push_back(blobEntryFor("f" + std::to_string(blob), numberedBlobHash(blob)));
    }
    writeManifestRaw(*backend, store->layout(), ns, manifest, entries);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, manifest);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(*backend, store->layout(), ns, "tbl", manifest);

    backend->clearJournal();
    markCutComplete(gc, *backend);
    runRegularRoundReclaiming(gc);

    const Layout & layout = store->layout();
    const auto journal = backend->journal();
    const auto is_mark = [](const JournalEntry & e) { return e.verb == JournalVerb::Mark && e.detail == "cut_complete"; };
    const auto is_blob_head = [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::Head && e.keys.front().starts_with(layout.blobsPrefix());
    };
    const auto is_ref_read = [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::Read && e.keys.front().starts_with(layout.casRefsPrefix());
    };
    const auto is_blob_write = [&](const JournalEntry & e)
    {
        return e.verb == JournalVerb::Write && e.keys.front().starts_with(layout.blobIntentsPrefix());
    };

    /// Positive controls: the mark, a ref-log read, the blob HEADs and the intent creates all exist. Only the FIRST ref-log read is bounded: a read-ahead read nobody took can
    /// follow the mark, but intake cannot end before it has read anything.
    ASSERT_EQ(std::count_if(journal.begin(), journal.end(), is_mark), 1);
    const size_t mark = firstIndexOf(journal, is_mark);
    const size_t first_ref_read = firstIndexOf(journal, is_ref_read);
    const size_t first_blob_head = firstIndexOf(journal, is_blob_head);
    const size_t first_blob_write = firstIndexOf(journal, is_blob_write);
    ASSERT_LT(first_ref_read, journal.size()) << "the round read no ref-log object";
    ASSERT_LT(first_blob_head, journal.size()) << "the round sent no blob HEAD";
    ASSERT_LT(first_blob_write, journal.size()) << "the round sent no intent create";

    EXPECT_LT(first_ref_read, mark);
    EXPECT_LT(mark, first_blob_head);
    EXPECT_LT(mark, first_blob_write);
}

TEST(CASTestSupport, PublicationFakeJournalsOneEntryPerBlobRemove)
{
    auto backend = std::make_shared<EtagFaithfulPublicationBackend>(EtagFaithfulPublicationBackend::FaultScript::None);
    const Layout layout("p");
    writeBlobBody(*backend, layout, numberedBlobHash(1));
    const String key = layout.blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(numberedBlobHash(1))});

    OperationForTest op(*backend);
    const auto seen = (*op).head(key, Retry::once());
    ASSERT_TRUE(seen.has_value());
    backend->clearJournal();

    ASSERT_EQ((*op).remove(key, seen->etag, Retry::once()), Removal::Removed);

    const auto journal = backend->journal();
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].verb, JournalVerb::Remove);
    EXPECT_EQ(journal[0].keys, std::vector<String>{key});
}

TEST(CASTestSupport, PublicationFakeWithoutAScriptPublishesOnce)
{
    auto backend = std::make_shared<EtagFaithfulPublicationBackend>(EtagFaithfulPublicationBackend::FaultScript::None);
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const String payload = "publication-fake-without-a-script";
    const BlobRef ref = idOf(payload);
    const RootNamespace ns{"srv1/honest"};
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/part";
    PartWriteTxnPtr build = store->beginPartWrite(info);
    const ManifestId id = build->stageManifest({blobEntryFor("col.bin", u128Of(payload), payload.size())});
    build->precommitAdd(ns, "part", id);

    BlobSource source;
    source.size = payload.size();
    source.open = [&payload]() -> std::unique_ptr<DB::ReadBuffer>
    {
        return std::make_unique<DB::ReadBufferFromMemory>(payload.data(), payload.size());
    };
    build->putBlob(ref, source);

    const String key = store->layout().blobKey(ref);
    EXPECT_EQ(backend->publishCount(key), 1u);
    EXPECT_EQ(backend->copy_publications + backend->streaming_publications, 1u);
    EXPECT_FALSE(backend->fault_fired);
    OperationForTest op(*backend);
    const auto stored = (*op).read(key, Retry::once());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->bytes.substr(store->poolMeta().blob_header_len), payload);
}

TEST(CASTestSupport, SeedOlderPoolWritesTheFloor)
{
    for (const uint64_t floor : {1, 3})
    {
        InMemoryBackend backend;
        seedOlderPoolForTest(backend, floor, "q", 2);
        const Layout layout("q");
        OperationForTest op(backend);
        const auto meta = (*op).read(layout.poolMetaKey(), Retry::once());
        ASSERT_TRUE(meta.has_value()) << floor;
        const PoolMeta decoded = decodePoolMeta(meta->bytes);
        EXPECT_EQ(decoded.min_reader_generation, floor);
        EXPECT_EQ(decoded.gc_shards, 2u);
        EXPECT_TRUE((*op).read(layout.refCatalogKey(), Retry::once()).has_value()) << floor;
    }
}
