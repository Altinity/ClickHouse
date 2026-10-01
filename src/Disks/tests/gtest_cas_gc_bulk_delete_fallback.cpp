#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

#include <variant>
#include <vector>

/// `removeCohortWriteOnce` (CasGc.h) cuts a cohort into requests of the storage's batch size and never
/// re-sends a rejected request in smaller pieces.

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

namespace
{

const Layout kLayout{"p"};
const RootNamespace kNs{"test/aa@cas@"};

WriteOnceKey manifestKey(uint32_t ordinal)
{
    return kLayout.writeOnceManifestKey(
        ManifestId{kNs, ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = ordinal}});
}

std::vector<WriteOnceKey> manifestKeys(uint32_t count)
{
    std::vector<WriteOnceKey> keys;
    for (uint32_t ordinal = 1; ordinal <= count; ++ordinal)
        keys.push_back(manifestKey(ordinal));
    return keys;
}

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend)
{
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Pool::open(backend, config);
}

/// Creates `keys`, one committed body each.
void seedBodies(CasOperation & op, const std::vector<WriteOnceKey> & keys)
{
    for (const WriteOnceKey & key : keys)
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key.str(), "b", Retry::once())));
}

}

TEST(CASGCCohortDelete, RequestsAreCutAtTheRequestSize)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    CasOperation op = store->openRequests().admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(5);
    seedBodies(op, keys);

    std::vector<std::pair<size_t, size_t>> requests;
    removeCohortWriteOnce(op, keys, 2, Retry::once(), [&](size_t begin, size_t end) { requests.emplace_back(begin, end); });

    EXPECT_EQ(requests, (std::vector<std::pair<size_t, size_t>>{{0, 2}, {2, 4}, {4, 5}}));
    EXPECT_EQ(backend->bulkRemoveCalls(), 3u);
    for (const WriteOnceKey & key : keys)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASGCCohortDelete, CohortHelperPropagatesNotImplemented)
{
    {
        SCOPED_TRACE("request size 1000, the first request is rejected");
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPlainPool(backend);
        CasOperation op = store->openRequests().admit();
        const std::vector<WriteOnceKey> keys = manifestKeys(1000);
        seedBodies(op, keys);
        backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "no batch delete")));
        size_t callbacks = 0;

        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
            [&] { removeCohortWriteOnce(op, keys, 1000, Retry::once(), [&](size_t, size_t) { ++callbacks; }); });

        EXPECT_EQ(backend->bulkRemoveCalls(), 1u) << "a rejected request is never re-sent key by key";
        EXPECT_EQ(callbacks, 0u);
        for (const WriteOnceKey & key : keys)
            EXPECT_TRUE(op.head(key.str(), Retry::once()).has_value()) << key.str();
    }
    {
        SCOPED_TRACE("request size 250, the second request is rejected");
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPlainPool(backend);
        CasOperation op = store->openRequests().admit();
        const std::vector<WriteOnceKey> keys = manifestKeys(1000);
        seedBodies(op, keys);
        /// The hook runs only on a call that applies, so it arms the rejection for the call after the first.
        backend->onBeforeBulkRemove([&]
        {
            backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "no batch delete")));
        });
        size_t callbacks = 0;

        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
            [&] { removeCohortWriteOnce(op, keys, 250, Retry::once(), [&](size_t, size_t) { ++callbacks; }); });

        EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
        EXPECT_EQ(callbacks, 1u);
        for (size_t i = 0; i < keys.size(); ++i)
            EXPECT_EQ(op.head(keys[i].str(), Retry::once()).has_value(), i >= 250) << keys[i].str();
    }
}

TEST(CASGCCohortDelete, ARealErrorStopsTheRemainingRequestsAndPropagates)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    CasOperation op = store->openRequests().admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(6);
    seedBodies(op, keys);
    backend->onBeforeBulkRemove([&]
    {
        backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "not a capability problem")));
    });

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { removeCohortWriteOnce(op, keys, 2, Retry::once(), [](size_t, size_t) {}); });

    EXPECT_EQ(backend->bulkRemoveCalls(), 2u) << "request 1 applied, request 2 failed, request 3 never sent";
    for (size_t i = 0; i < keys.size(); ++i)
        EXPECT_EQ(op.head(keys[i].str(), Retry::once()).has_value(), i >= 2) << keys[i].str();
}
