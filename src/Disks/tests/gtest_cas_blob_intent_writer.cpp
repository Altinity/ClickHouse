#include <gtest/gtest.h>

#include "cas_publication_test_support.h"
#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <IO/ReadBufferFromString.h>
#include <Common/ProfileEvents.h>

#include <Poco/Exception.h>

#include <atomic>
#include <memory>
#include <vector>

namespace ProfileEvents
{
extern const Event CASBlobReuseTokenRejected;
extern const Event CASBlobReuseIntentPresent;
}

/// The writer's two reads: `HEAD B`, then `GET I(B,t)` for the token that `HEAD` showed. The journals below keep
/// only requests on blob and intent keys, so manifest and ref traffic of the build does not enter them.

using namespace DB::Cas;
using DB::Cas::tests::EtagFaithfulPublicationBackend;
using DB::Cas::tests::JournalEntry;
using DB::Cas::tests::JournalVerb;
using DB::Cas::tests::OperationForTest;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

/// The etag-faithful store, with three seams: a publication that lands and loses its answer, a read of an
/// intent key that fails, and a `HEAD` of a blob body that answers a chosen token.
class WriterBackend : public EtagFaithfulPublicationBackend
{
public:
    enum class IntentReadFault : uint8_t { None, Refused, Unanswered };

    WriterBackend() : EtagFaithfulPublicationBackend(FaultScript::None) {}

    std::atomic<bool> land_then_lose_first_publication{false};
    std::atomic<IntentReadFault> intent_read_fault{IntentReadFault::None};
    std::optional<String> blob_head_value;

    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        EtagFaithfulPublicationBackend::publish(request, access);
        if (land_then_lose_first_publication.exchange(false))
            throw Poco::TimeoutException("the publication landed and its answer was lost");
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key.find("/blob-intents/") != String::npos)
        {
            switch (intent_read_fault.load())
            {
                case IntentReadFault::Refused:
                    CountingBackend::read(key, access);
                    throw std::runtime_error("the store refused the intent read: Access Denied");
                case IntentReadFault::Unanswered:
                    CountingBackend::read(key, access);
                    throw Poco::TimeoutException("the intent read was never answered");
                case IntentReadFault::None:
                    break;
            }
        }
        return EtagFaithfulPublicationBackend::read(key, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = EtagFaithfulPublicationBackend::head(key, access);
        if (meta && blob_head_value && key.find("/blobs/") != String::npos)
            meta->value = *blob_head_value;
        return meta;
    }
};

PoolPtr openPool(const BackendPtr & backend)
{
    auto pool = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto now = std::make_shared<std::atomic<uint64_t>>(0);
    pool->mountRequests().setNowFnForTest([now] { return now->load(); });
    pool->mountRequests().setSleepFnForTest([now](uint64_t ms) { now->fetch_add(ms + 1); });
    return pool;
}

PartWriteTxnPtr precommittedBuild(const PoolPtr & pool, const String & payload, const String & part = "part")
{
    const RootNamespace ns{"srv/intent-writer"};
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + part;
    auto build = pool->beginPartWrite(info);
    const ManifestId id = build->stageManifest({DB::Cas::tests::blobEntryFor("col.bin", u128Of(payload), payload.size())});
    build->precommitAdd(ns, part, id);
    return build;
}

BlobSource sourceOf(const String & bytes)
{
    BlobSource source;
    source.size = bytes.size();
    source.open = [bytes]() -> std::unique_ptr<DB::ReadBuffer> { return std::make_unique<DB::ReadBufferFromOwnString>(bytes); };
    return source;
}

/// Requests on blob and intent keys, in order, as `(verb, key)`.
std::vector<std::pair<JournalVerb, String>> blobRequests(const std::vector<JournalEntry> & journal)
{
    std::vector<std::pair<JournalVerb, String>> out;
    for (const JournalEntry & entry : journal)
        for (const String & key : entry.keys)
            if (key.find("/blobs/") != String::npos || key.find("/blob-intents/") != String::npos)
                out.emplace_back(entry.verb, key);
    return out;
}

String intentKeyOfBody(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    OperationForTest op(backend);
    const auto meta = (*op).head(layout.blobKey(ref), Retry::standard());
    chassert(meta.has_value());
    return layout.blobIntentKey(ref, *meta->etag.canonical());
}

void plantBody(Backend & backend, const PoolPtr & pool, const String & payload)
{
    DB::Cas::tests::writeBlobRaw(backend, pool->layout(), payload, pool->poolMeta().blob_header_len, DB::UInt128{});
}

void plantIntent(Backend & backend, const String & key, const String & bytes = blobIntentRecord())
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::standard())));
}

uint64_t counter(ProfileEvents::Event event)
{
    return static_cast<uint64_t>(ProfileEvents::global_counters[event]);
}

using Requests = std::vector<std::pair<JournalVerb, String>>;

BlobUploadResult upload(const PartWriteTxn & build, const BlobRef & ref, BlobSource source)
{
    const uint64_t size = source.size;
    return build.uploadBlobDetached(BlobUploadRequest{.ref = ref, .source = std::move(source), .declared_size = size});
}

}

TEST(CASBlobIntentWriter, FirstCreationIsHeadAndOnePut)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "first-creation-payload";
    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();

    const BlobUploadResult result = upload(*build, idOf(payload), sourceOf(payload));

    const String key = pool->layout().blobKey(idOf(payload));
    EXPECT_EQ(blobRequests(backend->journal()), (Requests{{JournalVerb::Head, key}, {JournalVerb::Publish, key}}));
    EXPECT_EQ(result.diagnostics.action, BlobMaterializationAction::Published);
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::Absent);
}

TEST(CASBlobIntentWriter, CleanReuseIsHeadAndGet)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "clean-reuse-payload";
    plantBody(*backend, pool, payload);
    const String intent = intentKeyOfBody(*backend, pool->layout(), idOf(payload));
    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();

    const BlobUploadResult result = upload(*build, idOf(payload), sourceOf(payload));

    const String key = pool->layout().blobKey(idOf(payload));
    EXPECT_EQ(blobRequests(backend->journal()), (Requests{{JournalVerb::Head, key}, {JournalVerb::Read, intent}}));
    EXPECT_EQ(result.diagnostics.action, BlobMaterializationAction::Observed);
}

TEST(CASBlobIntentWriter, IntentPresentPublishesFreshOnce)
{
    for (const String & intent_bytes : {blobIntentRecord(), String("other bytes at an intent key")})
    {
        SCOPED_TRACE(intent_bytes);
        auto backend = std::make_shared<WriterBackend>();
        auto pool = openPool(backend);
        const String payload = "intent-present-payload";
        plantBody(*backend, pool, payload);
        const String intent = intentKeyOfBody(*backend, pool->layout(), idOf(payload));
        plantIntent(*backend, intent, intent_bytes);
        auto build = precommittedBuild(pool, payload);
        backend->clearJournal();
        const uint64_t before = counter(ProfileEvents::CASBlobReuseIntentPresent);

        const BlobUploadResult result = upload(*build, idOf(payload), sourceOf(payload));

        const String key = pool->layout().blobKey(idOf(payload));
        EXPECT_EQ(blobRequests(backend->journal()),
                  (Requests{{JournalVerb::Head, key}, {JournalVerb::Read, intent}, {JournalVerb::Publish, key}}));
        EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::IntentPresent);
        EXPECT_EQ(counter(ProfileEvents::CASBlobReuseIntentPresent) - before, 1u);
        EXPECT_NE(intentKeyOfBody(*backend, pool->layout(), idOf(payload)), intent) << "the fresh body repeats the condemned token";
    }
}

TEST(CASBlobIntentWriter, NoSecondHeadOnAnyPath)
{
    for (const int path : {0, 1, 2})
    {
        SCOPED_TRACE(path);
        auto backend = std::make_shared<WriterBackend>();
        auto pool = openPool(backend);
        const String payload = "no-second-head-payload";
        if (path >= 1)
            plantBody(*backend, pool, payload);
        if (path == 2)
            plantIntent(*backend, intentKeyOfBody(*backend, pool->layout(), idOf(payload)));
        auto build = precommittedBuild(pool, payload);
        backend->clearJournal();
        static_cast<void>(build->putBlob(idOf(payload), sourceOf(payload)));
        const auto journal = backend->journal();
        size_t heads = 0;
        for (const auto & [verb, key] : blobRequests(journal))
            heads += verb == JournalVerb::Head;
        EXPECT_EQ(heads, 1u);
        for (const JournalEntry & entry : journal)
            for (const String & key : entry.keys)
                EXPECT_FALSE(key.ends_with(".meta")) << key;
    }
}

TEST(CASBlobIntentWriter, LandedAmbiguousPublicationIsAdoptedInThreeReads)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "landed-ambiguous-payload";
    auto build = precommittedBuild(pool, payload);
    backend->land_then_lose_first_publication = true;
    backend->clearJournal();

    const BlobUploadResult result = upload(*build, idOf(payload), sourceOf(payload));
    const auto journal = backend->journal();

    const String key = pool->layout().blobKey(idOf(payload));
    const String intent = intentKeyOfBody(*backend, pool->layout(), idOf(payload));
    EXPECT_EQ(blobRequests(journal),
              (Requests{{JournalVerb::Head, key}, {JournalVerb::Publish, key}, {JournalVerb::Head, key}, {JournalVerb::Read, intent}}));
    EXPECT_EQ(result.diagnostics.action, BlobMaterializationAction::Observed);
}

TEST(CASBlobIntentWriter, FailedIntentReadIsNotAbsence)
{
    for (const auto fault : {WriterBackend::IntentReadFault::Refused, WriterBackend::IntentReadFault::Unanswered})
    {
        SCOPED_TRACE(static_cast<int>(fault));
        auto backend = std::make_shared<WriterBackend>();
        auto pool = openPool(backend);
        const String payload = "failed-intent-read-payload";
        plantBody(*backend, pool, payload);
        auto build = precommittedBuild(pool, payload);
        backend->intent_read_fault = fault;
        backend->clearJournal();

        EXPECT_ANY_THROW(static_cast<void>(build->putBlob(idOf(payload), sourceOf(payload))));

        const Requests requests = blobRequests(backend->journal());
        ASSERT_GE(requests.size(), 2u);
        EXPECT_EQ(requests[0].first, JournalVerb::Head);
        EXPECT_EQ(requests[1].first, JournalVerb::Read) << "the intent was never read";
        for (const auto & [verb, key] : requests)
            EXPECT_NE(verb, JournalVerb::Publish);
        EXPECT_FALSE(build->dependencyProof(idOf(payload)).has_value()) << "a failed intent read was taken as absence";
    }
}

TEST(CASBlobIntentWriter, WriterNeverMutatesIntents)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "never-mutates-payload";
    plantBody(*backend, pool, payload);
    const String intent = intentKeyOfBody(*backend, pool->layout(), idOf(payload));
    plantIntent(*backend, intent);
    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();

    static_cast<void>(build->putBlob(idOf(payload), sourceOf(payload)));

    bool intent_read = false;
    for (const JournalEntry & entry : backend->journal())
        for (const String & key : entry.keys)
            if (key.find("/blob-intents/") != String::npos)
            {
                intent_read |= entry.verb == JournalVerb::Read && key == intent;
                EXPECT_TRUE(entry.verb == JournalVerb::Read) << "the writer sent a mutation to " << key;
            }
    EXPECT_TRUE(intent_read) << "positive control: the existing intent was read";
    OperationForTest op(*backend);
    EXPECT_TRUE((*op).read(intent, Retry::standard()).has_value()) << "the intent is gone";
}

TEST(CASBlobIntentWriter, VerbatimCopyIsSelectedOnlyAfterAnAbsentFirstHead)
{
    for (const bool present : {false, true})
    {
        SCOPED_TRACE(present);
        auto backend = std::make_shared<WriterBackend>();
        auto pool = openPool(backend);
        const String payload = "verbatim-copy-payload";
        const uint64_t header_len = pool->poolMeta().blob_header_len;
        const String staging_key = "p/staging/mount1/copy.tmp";
        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;
        header.incarnation_tag = DB::UInt128{77};
        {
            OperationForTest op(*backend);
            (*op).create(staging_key, encodeEnvelopeHeader(header, static_cast<uint32_t>(header_len)) + payload, Retry::standard());
        }
        if (present)
        {
            plantBody(*backend, pool, payload);
            plantIntent(*backend, intentKeyOfBody(*backend, pool->layout(), idOf(payload)));
        }
        auto build = precommittedBuild(pool, payload);
        BlobSource staged = sourceOf(payload);
        staged.server_side_copy_from = staging_key;

        static_cast<void>(build->putBlob(idOf(payload), staged));
        EXPECT_EQ(backend->copy_publications, present ? 0u : 1u);
        EXPECT_EQ(backend->streaming_publications, present ? 1u : 0u);
    }
}

TEST(CASBlobIntentWriter, WellFormedUncanonicalTokenPublishesFresh)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "weak-token-payload";
    plantBody(*backend, pool, payload);
    backend->blob_head_value = "W/\"x\"";
    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();
    const uint64_t before = counter(ProfileEvents::CASBlobReuseTokenRejected);

    const BlobUploadResult result = upload(*build, idOf(payload), sourceOf(payload));

    const String key = pool->layout().blobKey(idOf(payload));
    EXPECT_EQ(blobRequests(backend->journal()), (Requests{{JournalVerb::Head, key}, {JournalVerb::Publish, key}}));
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::TokenRejected);
    EXPECT_EQ(counter(ProfileEvents::CASBlobReuseTokenRejected) - before, 1u);
}

TEST(CASBlobIntentWriter, MalformedTokenFailsTheRead)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "malformed-token-payload";
    plantBody(*backend, pool, payload);
    backend->blob_head_value = String();
    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();

    EXPECT_ANY_THROW(static_cast<void>(build->putBlob(idOf(payload), sourceOf(payload))));

    for (const auto & [verb, key] : blobRequests(backend->journal()))
    {
        EXPECT_EQ(verb, JournalVerb::Head) << key;
        EXPECT_EQ(key.find("/blob-intents/"), String::npos) << key;
    }
    EXPECT_FALSE(build->dependencyProof(idOf(payload)).has_value());
}

TEST(CASBlobIntentWriter, AdoptionReadsReachTheStoreAfterAPrimedAbsence)
{
    auto backend = std::make_shared<WriterBackend>();
    auto pool = openPool(backend);
    const String payload = "primed-absence-payload";
    const BlobRef ref = idOf(payload);

    /// A committed part holds the blob; dropping it lets GC condemn the body.
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef committed{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    plantBody(*backend, pool, payload);
    DB::Cas::tests::writeManifestRaw(*backend, pool->layout(), ns, committed, {DB::Cas::tests::blobEntryFor("f", u128Of(payload))});
    DB::Cas::tests::publishCommittedTransition(*backend, pool->layout(), ns, "tbl", std::nullopt, committed);
    Gc gc(pool, hexToU128("000000000000000000000000000000cc"));
    DB::Cas::tests::runRegularRoundReclaiming(gc);
    pool->renewWatermarkOnce();

    /// The pool reads the intent while none exists.
    const String intent = intentKeyOfBody(*backend, pool->layout(), ref);
    EXPECT_FALSE(pool->mountRequests().admit().read(intent, Retry::standard()).has_value());

    DB::Cas::tests::dropRefTransition(*backend, pool->layout(), ns, "tbl", committed);
    const RoundReport condemning = DB::Cas::tests::runRegularRoundReclaiming(gc);
    pool->renewWatermarkOnce();
    ASSERT_EQ(condemning.intent_confirmed, 1u) << "GC did not confirm the intent";

    auto build = precommittedBuild(pool, payload);
    backend->clearJournal();
    const BlobUploadResult result = upload(*build, ref, sourceOf(payload));

    const String key = pool->layout().blobKey(ref);
    EXPECT_EQ(blobRequests(backend->journal()),
              (Requests{{JournalVerb::Head, key}, {JournalVerb::Read, intent}, {JournalVerb::Publish, key}}));
    EXPECT_EQ(result.diagnostics.reason, BlobPublicationReason::IntentPresent);
}
