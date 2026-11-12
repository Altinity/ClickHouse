#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Common/Exception.h>

#include <Poco/Exception.h>

#include <fmt/format.h>

#include <memory>
#include <vector>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int NETWORK_ERROR;
}

using namespace DB::Cas;

namespace
{

/// One seeded key and an operation whose observer records every call as a line.
struct Fixture
{
    std::shared_ptr<InMemoryBackend> backend = std::make_shared<InMemoryBackend>();
    CasRequests requests{backend, Fence::open(), [] { return uint64_t{100}; }, [](uint64_t) {}};
    Etag etag = std::get<Committed>(requests.admit().create("k", "v1", Retry::standard())).etag;
    std::vector<String> events;
    CasOperation op = requests.admit();

    Fixture()
    {
        op.observeRequests(RequestObserver{
            .on_write_attempt = [this](uint32_t attempts_sent) { events.push_back(fmt::format("attempt {}", attempts_sent)); },
            .on_failure = [this](std::string_view verb, const String & key, const std::exception & e)
            {
                events.push_back(describeRequestFailure(verb, key, e));
            },
        });
    }
};

}

TEST(CASRequestObserver, WriteAttemptsAndFailuresAreReportedAsTheyHappen)
{
    Fixture f;
    f.backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("injected write timeout")));
    f.backend->failNextReadWith("k", std::make_exception_ptr(DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected read failure")));

    ASSERT_TRUE(std::holds_alternative<Committed>(f.op.replace("k", "v2", f.etag, Retry::standard())));
    EXPECT_EQ(f.events, (std::vector<String>{
        "attempt 1",
        "PUT k: transport Timeout injected write timeout",
        "GET k: transport NETWORK_ERROR injected read failure",
        "attempt 2",
    }));
}

/// A local bug is not an answer from the store, so it is not published as the request's error.
TEST(CASRequestObserver, ALocalFailureIsNotReportedAsAStoreAnswer)
{
    Fixture f;
    f.backend->failNextWriteWith("k", std::make_exception_ptr(DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected local failure")));

    EXPECT_THROW((void)f.op.replace("k", "v2", f.etag, Retry::standard()), DB::Exception);
    EXPECT_EQ(f.events, (std::vector<String>{"attempt 1"}));
}

/// A local failure of the resolve read is not published either, only the store answer before it.
TEST(CASRequestObserver, ALocalFailureOfTheResolveReadIsNotReported)
{
    Fixture f;
    f.backend->failNextWriteWith("k", std::make_exception_ptr(Poco::TimeoutException("injected write timeout")));
    f.backend->failNextReadWith("k", std::make_exception_ptr(DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "injected local failure")));

    EXPECT_THROW((void)f.op.replace("k", "v2", f.etag, Retry::standard()), DB::Exception);
    EXPECT_EQ(f.events, (std::vector<String>{"attempt 1", "PUT k: transport Timeout injected write timeout"}));
}
