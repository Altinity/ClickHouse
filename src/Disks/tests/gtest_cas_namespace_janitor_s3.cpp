#include "config.h"

#if USE_AWS_S3

#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/tests/cas_scripted_s3_server.h>
#include <IO/S3Common.h>
#include <Common/ThreadPool.h>

/// Janitor tests that need `S3Exception` classification or the real S3 delete adapter.

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;
using namespace DB::Cas::tests::s3;

namespace
{

const Layout & kLayout = janitorLayout();

std::exception_ptr accessDenied()
{
    return std::make_exception_ptr(DB::S3Exception("Access Denied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
}

/// The in-memory store whose `removeManyWriteOnce` goes through a real `S3ObjectStorage` against a scripted
/// server: the adapter's request shapes, error names and capability learning, every other verb in memory.
/// After each call it mirrors the keys the server reports removed.
class S3DeletePathBackend : public CountingBackend
{
public:
    S3DeletePathBackend(std::shared_ptr<DB::S3ObjectStorage> storage_, std::shared_ptr<ScriptedDeletes> deleted_)
        : storage(std::move(storage_)), deleted(std::move(deleted_)) {}

    size_t bulkDeleteKeyLimit() const override { return storage->batchDeleteKeyLimit(); }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        DB::StoredObjects objects;
        for (const WriteOnceKey & key : keys)
            objects.emplace_back(key.str());
        std::exception_ptr failure;
        try
        {
            storage->removeObjectsIfExistUnderProfile(objects, DB::ObjectStorageControlRequest{});
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        std::vector<WriteOnceKey> gone;
        for (const WriteOnceKey & key : keys)
        {
            if (deleted->contains(key.str()))
                gone.push_back(key);
        }
        InMemoryBackend::removeManyWriteOnce(gone, access); // NOLINT(bugprone-parent-virtual-call)
        if (failure)
            std::rethrow_exception(failure);
    }

private:
    std::shared_ptr<DB::S3ObjectStorage> storage;
    std::shared_ptr<ScriptedDeletes> deleted;
};

/// Accepts every delete: a one-key DELETE and every key of a `DeleteObjects` are recorded removed.
void acceptAll(ScriptedDeletes & deleted, const Poco::Net::HTTPServerRequest & request, const std::string & body,
               Poco::Net::HTTPServerResponse & response)
{
    if (request.getMethod() == "DELETE")
    {
        deleted.add(keyOfDeleteObject(request));
        sendDeleteObjectSuccess(response);
        return;
    }
    for (const std::string & key : keysOfDeleteObjects(body))
        deleted.add(key);
    sendDeleteObjectsResult(response, {});
}

/// The janitor over `S3DeletePathBackend`. `respond` must be set before the first request.
struct S3PathFixture
{
    std::shared_ptr<ScriptedDeletes> deleted = std::make_shared<ScriptedDeletes>();
    std::function<void(const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse &)> respond
        = [this](const auto & request, const std::string & body, auto & response) { acceptAll(*deleted, request, body, response); };
    ScriptedS3Server server{[this](const auto & request, const std::string & body, auto & response) { respond(request, body, response); }};
    FakeClock clock;
    std::shared_ptr<S3DeletePathBackend> backend;
    std::unique_ptr<CasRequests> requests;

    explicit S3PathFixture(const DB::S3Capabilities & capabilities)
    {
        (void)contextForTest();
        backend = std::make_shared<S3DeletePathBackend>(makeStorageForTest(server.getUrl(), capabilities), deleted);
        requests = std::make_unique<CasRequests>(backend, Fence::open(), clock.nowFn(), clock.sleepFn());
        seedCatalog(*backend, kLayout);
    }

    JanitorRunContext context()
    {
        JanitorRunContext result;
        result.batch_keys = backend->bulkDeleteKeyLimit();
        result.now_ms = clock.nowFn();
        return result;
    }

    NamespaceJanitorResult run(const JanitorRunContext & context) { return runPhase(*requests, kLayout, context); }
    std::optional<String> cursor() { return publishedCursor(*requests, kLayout); }
};

}

TEST(CASNamespaceJanitorS3, PermanentRefusalLeaksAndAdvances)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 311), 2000);
    std::atomic<bool> refuse{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (refuse.exchange(false))
            std::rethrow_exception(accessDenied());
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.backend->bulkCalls().size(), 2u) << "one attempt for the refused batch, one for page 2";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    for (size_t i = 1000; i < 2000; ++i)
        EXPECT_FALSE(present(*f.backend, keys[i])) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, NameOnlyRefusalLeaksAndAdvances)
{
    S3PathFixture f(DB::S3Capabilities{});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 314), 1500);
    f.respond = [&](const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        bool refuse = false;
        for (const std::string & key : keysOfDeleteObjects(body))
        {
            if (key == keys[3])
                refuse = true;
            else
                f.deleted->add(key);
        }
        if (refuse)
            sendDeleteObjectsResult(response, {{keys[3], "EntityTooLarge"}});
        else
            sendDeleteObjectsResult(response, {});
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.server.countMethod("POST"), 2u) << "one attempt for the refused batch, one for page 2";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_TRUE(present(*f.backend, keys[3]));
    for (size_t i = 1000; i < 1500; ++i)
        EXPECT_FALSE(present(*f.backend, keys[i])) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, UnknownCapabilityConcurrentProbesAllStop)
{
    S3PathFixture f(DB::S3Capabilities{});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 422), 3000);
    std::atomic<size_t> rejected{0};
    f.respond = [&](const Poco::Net::HTTPServerRequest & request, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        if (request.getMethod() == "POST")
        {
            ++rejected;
            sendBatchNotImplemented(response);
            return;
        }
        acceptAll(*f.deleted, request, body, response);
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = f.context();
    context.io_pool = pool.get();

    (void)f.run(context);

    EXPECT_GE(rejected.load(), 1u);
    EXPECT_LE(rejected.load(), 4u);
    EXPECT_EQ(f.server.countMethod("DELETE"), 0u);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(f.backend->bulkDeleteKeyLimit(), 1u) << "the storage remembers false";

    JanitorRunContext next = f.context();
    next.io_pool = pool.get();
    const NamespaceJanitorResult drained = f.run(next);
    EXPECT_EQ(drained.batch_keys, 1u);
    EXPECT_EQ(drained.deleted, 3000u);
    EXPECT_EQ(f.server.countMethod("DELETE"), 3000u);
}

#endif
