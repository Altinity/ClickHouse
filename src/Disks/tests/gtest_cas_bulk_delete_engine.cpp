#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>

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
    const auto reissues_before = ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load();

    op.removeManyWriteOnce(keys, Retry::standard());

    EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASRequestReissue].load() - reissues_before, 1u);
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
    EXPECT_DEATH({ op.removeManyWriteOnce(manifestKeys(1001), Retry::once()); }, "removeManyWriteOnce");
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
}
#endif

TEST(CASConditionalBatchDeleteEngine, MoreThanOneThousandKeysIsRejectedEvenIfBackendReportsALargerLimit)
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

    EXPECT_THROW(op.removeManyByConditional(removals, Retry::once()), DB::Exception);
}
