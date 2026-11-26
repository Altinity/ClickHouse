#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Poco/AutoPtr.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Logger.h>
#include <base/defines.h>

/// The engine sends one chunk of at most 1000 write-once keys as one request under the ordinary
/// attempt loop; a failed attempt reissues the whole chunk, which is sound because a key the failed
/// attempt already deleted is absent on the reissue, and absence is success.

namespace ProfileEvents
{
    extern const Event CASRequestReissue;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::openRequestsForTest;

namespace DB::ErrorCodes
{
    extern const int CAS_DELETE_MARKER;
    extern const int LOGICAL_ERROR;
}

namespace
{

const Layout kLayout{"p"};
const RootNamespace kNs{"test/aa@cas@"};

std::vector<WriteOnceKey> manifestKeys(uint32_t count)
{
    std::vector<WriteOnceKey> keys;
    for (uint32_t ordinal = 1; ordinal <= count; ++ordinal)
        keys.push_back(kLayout.writeOnceManifestKey(
            ManifestId{kNs, ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = ordinal}}));
    return keys;
}

class UnlimitedConditionalBatchBackend : public InMemoryBackend
{
public:
    size_t conditionalBatchDeleteKeyLimit() const override { return kBulkDeleteMaxKeys + 1; }
};

class RecordingConditionalBackend : public InMemoryBackend
{
public:
    std::vector<std::vector<RawConditionalRemove>> attempts;
    bool lose_next_pair_response = false;

    std::vector<RawBatchRemoveResult> removeManyByConditional(
        const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
    {
        attempts.push_back(removals);
        if (lose_next_pair_response && removals.size() == 2)
        {
            lose_next_pair_response = false;
            const auto & first = removals[0];
            const auto & second = removals[1];
            EXPECT_EQ(remove(first.key, first.expected_token, access), RawRemoval::Removed);
            EXPECT_TRUE(write(second.key, "replacement", second.expected_token, access).has_value());
            throw Poco::TimeoutException("lost conditional delete response");
        }
        return InMemoryBackend::removeManyByConditional(removals, access);
    }
};

}

TEST(CASBulkDeleteEngine, AFailedAttemptReissuesTheWholeChunkAndAlreadyDeletedKeysAreSuccess)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(5);
    for (const WriteOnceKey & key : keys)
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key.str(), "b", Retry::once())));
    /// Half the chunk is gone before the failed attempt reports: the reissue must still succeed.
    {
        const auto h = op.head(keys[0].str(), Retry::once());
        ASSERT_TRUE(h.has_value());
        ASSERT_EQ(op.remove(keys[0].str(), h->etag, Retry::once()), Removal::Removed);
    }
    backend->failNextBulkRemoveWith(std::make_exception_ptr(Poco::TimeoutException("injected")));
    const auto reissues_before = ProfileEvents::global_counters[ProfileEvents::CASRequestReissue];

    op.removeManyWriteOnce(keys, Retry::standard());

    EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestReissue] - reissues_before, 1u);
    for (const WriteOnceKey & key : keys)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASBulkDeleteEngine, AnEmptyChunkIsNoRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    op.removeManyWriteOnce({}, Retry::once());
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
}

TEST(CASConditionalBatchDeleteEngine, ReturnsMixedOutcomesInInputOrder)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    const String removed_key = "p/conditional/removed";
    const String replaced_key = "p/conditional/replaced";
    const String gone_key = "p/conditional/gone";

    const Etag removed_token = std::get<Committed>(op.create(removed_key, "v1", Retry::once())).etag;
    const Etag stale_token = std::get<Committed>(op.create(replaced_key, "v1", Retry::once())).etag;
    const Etag gone_token = std::get<Committed>(op.create(gone_key, "v1", Retry::once())).etag;
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace(replaced_key, "v2", stale_token, Retry::once())));
    ASSERT_EQ(op.remove(gone_key, gone_token, Retry::once()), Removal::Removed);

    const std::vector<Removal> results = op.removeManyByConditional(
        {{removed_key, removed_token}, {replaced_key, stale_token}, {gone_key, gone_token}}, Retry::once());

    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0], Removal::Removed);
    EXPECT_EQ(results[1], Removal::Mismatch);
    EXPECT_EQ(results[2], Removal::Gone);
    EXPECT_FALSE(op.head(removed_key, Retry::once()).has_value());
    const auto replaced = op.read(replaced_key, Retry::once());
    ASSERT_TRUE(replaced.has_value());
    EXPECT_EQ(replaced->bytes, "v2");
}

TEST(CASConditionalBatchDeleteEngine, HeldDeletesRecheckTheSubmittedTokensWhenTheyLand)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    const auto stale = std::get<Committed>(op.create("already_replaced", "old", Retry::once())).etag;
    const auto live = std::get<Committed>(op.create("replaced_later", "old", Retry::once())).etag;
    const auto stable = std::get<Committed>(op.create("stable", "original", Retry::once())).etag;
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace("already_replaced", "new", stale, Retry::once())));
    backend->setHoldDeletes(true);

    /// Accept a stale token at submission so the token in the request differs from the one in the store.
    backend->setEnforceTokens(false);
    const auto results = op.removeManyByConditional({{"already_replaced", stale}, {"replaced_later", live}}, Retry::once());
    ASSERT_EQ(results, (std::vector{Removal::Removed, Removal::Removed}));
    backend->setEnforceTokens(true);
    ASSERT_EQ(op.removeManyByConditional({{"stable", stable}}, Retry::once()), (std::vector{Removal::Removed}));
    ASSERT_EQ(backend->pendingDeletes(), 3u);
    ASSERT_TRUE(op.head("stable", Retry::once()));
    ASSERT_TRUE(std::holds_alternative<Committed>(op.replace("replaced_later", "new", live, Retry::once())));

    EXPECT_EQ(backend->landPendingDelete(0), Backend::RawRemoval::Mismatch);
    EXPECT_EQ(backend->landPendingDelete(0), Backend::RawRemoval::Mismatch);
    EXPECT_EQ(backend->landPendingDelete(0), Backend::RawRemoval::Removed);
    EXPECT_EQ(backend->pendingDeletes(), 0u);
    EXPECT_TRUE(op.head("already_replaced", Retry::once()));
    EXPECT_TRUE(op.head("replaced_later", Retry::once()));
    EXPECT_FALSE(op.head("stable", Retry::once()));
}

TEST(CASConditionalBatchDeleteEngine, PartialSuccessRetryKeepsOriginalTokens)
{
    auto backend = std::make_shared<RecordingConditionalBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    backend->lose_next_pair_response = true;
    DB::Cas::tests::FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    auto op = requests.admit();
    const auto first = std::get<Committed>(op.create("a", "first", Retry::once())).etag;
    const auto second = std::get<Committed>(op.create("b", "second", Retry::once())).etag;

    const auto results = op.removeManyByConditional({{"a", first}, {"b", second}}, Retry::standard());

    EXPECT_EQ(results, (std::vector{Removal::Gone, Removal::Mismatch}));
    ASSERT_EQ(backend->attempts.size(), 2u);
    for (const auto & attempt : backend->attempts)
    {
        ASSERT_EQ(attempt.size(), 2u);
        EXPECT_EQ(attempt[0].key, "a");
        EXPECT_EQ(attempt[0].expected_token, PersistedEtag::capture(first).value);
        EXPECT_EQ(attempt[1].key, "b");
        EXPECT_EQ(attempt[1].expected_token, PersistedEtag::capture(second).value);
    }
    EXPECT_FALSE(op.head("a", Retry::once()));
    const auto replacement = op.read("b", Retry::once());
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->bytes, "replacement");
}

TEST(CASConditionalBatchDeleteEngine, LostResponseRetriesOnlyTheOutstandingPair)
{
    class PartialReplyBackend final : public RecordingConditionalBackend
    {
    public:
        std::vector<RawBatchRemoveResult> removeManyByConditional(
            const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
        {
            if (!attempts.empty() || removals.size() != 3)
                return RecordingConditionalBackend::removeManyByConditional(removals, access);

            attempts.push_back(removals);
            const auto error = std::unexpected(std::make_exception_ptr(Poco::TimeoutException("retry key")));
            return {error, remove(removals[1].key, removals[1].expected_token, access), error};
        }
    };

    auto backend = std::make_shared<PartialReplyBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    backend->lose_next_pair_response = true;
    DB::Cas::tests::FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    auto op = requests.admit();
    std::vector<ConditionalRemove> input;
    for (const String key : {"a", "settled", "b"})
        input.push_back({key, std::get<Committed>(op.create(key, "old", Retry::once())).etag});

    EXPECT_EQ(op.removeManyByConditional(input, Retry::standard()), (std::vector{Removal::Gone, Removal::Removed, Removal::Mismatch}));
    ASSERT_EQ(backend->attempts.size(), 3u);
    for (const size_t attempt : {1u, 2u})
    {
        const auto & pair = backend->attempts[attempt];
        ASSERT_EQ(pair.size(), 2u);
        EXPECT_EQ(pair[0].key, "a");
        EXPECT_EQ(pair[0].expected_token, PersistedEtag::capture(input[0].expected_token).value);
        EXPECT_EQ(pair[1].key, "b");
        EXPECT_EQ(pair[1].expected_token, PersistedEtag::capture(input[2].expected_token).value);
    }
    const auto replacement = op.read("b", Retry::once());
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->bytes, "replacement");
}

TEST(CASConditionalBatchDeleteEngine, DeleteMarkerFailsTheChunk)
{
    auto backend = std::make_shared<InMemoryBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    const auto first = std::get<Committed>(op.create("a", "body", Retry::once())).etag;
    const auto second = std::get<Committed>(op.create("b", "body", Retry::once())).etag;
    ASSERT_EQ(op.remove("a", first, Retry::once()), Removal::Removed);
    backend->setSimulateDeleteMarkers(true);

    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CAS_DELETE_MARKER, [&]
    {
        op.removeManyByConditional({{"a", first}, {"b", second}}, Retry::once());
    });
}

TEST(CASConditionalBatchDeleteEngine, EmptyInputMakesNoRequestAndAFullChunkMakesOne)
{
    auto backend = std::make_shared<RecordingConditionalBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    EXPECT_TRUE(op.removeManyByConditional({}, Retry::once()).empty());
    EXPECT_TRUE(backend->attempts.empty());

    std::vector<ConditionalRemove> chunk;
    for (size_t i = 0; i < kBulkDeleteMaxKeys; ++i)
    {
        const String key = std::to_string(i);
        chunk.push_back({key, std::get<Committed>(op.create(key, "body", Retry::once())).etag});
    }
    EXPECT_EQ(op.removeManyByConditional(chunk, Retry::once()), std::vector<Removal>(chunk.size(), Removal::Removed));
    ASSERT_EQ(backend->attempts.size(), 1u);
    EXPECT_EQ(backend->attempts.front().size(), chunk.size());
}

TEST(CASConditionalBatchDeleteEngineDeathTest, RejectsDuplicateKeysAndForeignTokensBeforeSending)
{
    auto backend = std::make_shared<RecordingConditionalBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    auto requests = openRequestsForTest(backend);
    auto op = requests.admit();
    auto other_requests = openRequestsForTest(std::make_shared<InMemoryBackend>());
    auto other_op = other_requests.admit();
    const auto token = std::get<Committed>(op.create("a", "body", Retry::once())).etag;
    const auto foreign = std::get<Committed>(other_op.create("b", "body", Retry::once())).etag;
    const ConditionalRemove first{"a", token};
    const auto expect_rejected = [&](const ConditionalRemove & invalid, const String & expected_message)
    {
        SCOPED_TRACE(expected_message);
        const std::vector<ConditionalRemove> chunk{first, invalid};
        EXPECT_DEATH(
            {
                Poco::AutoPtr<Poco::ConsoleChannel> channel(new Poco::ConsoleChannel(std::cerr));
                auto & logger = Poco::Logger::root();
                logger.setChannel(channel);
                logger.setLevel(Poco::Message::PRIO_FATAL);
                DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
                op.removeManyByConditional(chunk, Retry::once());
            },
            expected_message);
#if !defined(DEBUG_OR_SANITIZER_BUILD)
        DB::Cas::tests::expectThrowsCodeWithMessage(
            DB::ErrorCodes::LOGICAL_ERROR, expected_message, [&] { op.removeManyByConditional(chunk, Retry::once()); });
#endif
    };

    expect_rejected(first, "duplicate key 'a' in one chunk");
    expect_rejected({"b", token}, fmt::format(
        "incarnation of 'a' observed on backend {} cannot be the precondition for 'b' on backend {}",
        token.backendId(), backend->backendId()));
    expect_rejected({"b", foreign}, fmt::format(
        "incarnation of 'b' observed on backend {} cannot be the precondition for 'b' on backend {}",
        foreign.backendId(), backend->backendId()));
    EXPECT_TRUE(backend->attempts.empty());
    EXPECT_TRUE(op.head("a", Retry::once()));
}

TEST(CASBulkDeleteEngine, ExactlyOneThousandKeysIsOneRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    op.removeManyWriteOnce(manifestKeys(1000), Retry::once());   /// all absent: success, one request
    EXPECT_EQ(backend->bulkRemoveCalls(), 1u);
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASBulkDeleteEngineDeathTest, MoreThanOneThousandKeysIsACallerBug)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    CAS_EXPECT_DEATH({ op.removeManyWriteOnce(manifestKeys(1001), Retry::once()); }, "removeManyWriteOnce");
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASConditionalBatchDeleteEngineDeathTest, MoreThanOneThousandKeysIsRejectedEvenIfBackendReportsALargerLimit)
#else
TEST(CASConditionalBatchDeleteEngine, MoreThanOneThousandKeysIsRejectedEvenIfBackendReportsALargerLimit)
#endif
{
    auto backend = std::make_shared<UnlimitedConditionalBatchBackend>();
    backend->pinConditionalBatchDeleteSupport(true);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    std::vector<ConditionalRemove> removals;
    removals.reserve(kBulkDeleteMaxKeys + 1);
    for (size_t i = 0; i <= kBulkDeleteMaxKeys; ++i)
    {
        const String key = "p/conditional/limit/" + std::to_string(i);
        removals.push_back({key, std::get<Committed>(op.create(key, "v", Retry::once())).etag});
    }

#if defined(DEBUG_OR_SANITIZER_BUILD)
    EXPECT_DEATH({ op.removeManyByConditional(removals, Retry::once()); }, "CAS removeManyByConditional: 1001 keys");
#else
    EXPECT_THROW(op.removeManyByConditional(removals, Retry::once()), DB::Exception);
#endif
}
