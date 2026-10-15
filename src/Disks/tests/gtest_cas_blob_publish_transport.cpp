#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/tests/S3ObjectCreationTestStorage.h>
#include <IO/ReadBufferFromString.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <Common/tests/gtest_global_context.h>

#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

/// Blob publication must put every object-creating request (`PutObject`, `CopyObject`,
/// `CompleteMultipartUpload`) on the wire at most once. These tests drive the backend's `publish`
/// against a counting S3 server whose client retries, as a disk's client does.

namespace DB::ErrorCodes
{
extern const int S3_ERROR;
}

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::forceOnePartMultipart;
using S3ObjectCreationTestStorage::forceOnePartMultipartCopy;
using S3ObjectCreationTestStorage::makeRetryingStorage;
using S3ObjectCreationTestStorage::thrownCode;

namespace
{

std::shared_ptr<DB::Cas::ObjectStorageBackend> nativeBackend(DB::ObjectStoragePtr storage)
{
    return std::make_shared<DB::Cas::ObjectStorageBackend>(
        std::move(storage), DB::Cas::ObjectStorageBackend::Mode::Native,
        /*single_attempt_control_plane_=*/true, /*attempt_timeout_ms_=*/5000, /*connect_timeout_cap_ms_=*/5000);
}

DB::Cas::BlobPublishRequest streamingPublication()
{
    return DB::Cas::BlobPublishRequest{
        .destination_key = "blob",
        .publication = DB::Cas::StreamingBlobPublication{
            .payload_size = 100,
            .fresh_envelope = "envelope",
            .open_payload = []
            {
                return std::make_unique<DB::ReadBufferFromOwnString>(std::string(100, 'p'));
            }}};
}

DB::Cas::BlobPublishRequest stagedCopyPublication()
{
    return DB::Cas::BlobPublishRequest{
        .destination_key = "blob",
        .publication = DB::Cas::VerbatimStagedBlobPublication{.object_key = "stage", .object_size = CountingS3::object_size}};
}

/// One engine attempt of the backend's blob publication, so every counted request comes from below it.
void publishOnce(const DB::ObjectStoragePtr & storage, const DB::Cas::BlobPublishRequest & request)
{
    DB::Cas::CasRequests requests(DB::Cas::BackendPtr(nativeBackend(storage)), DB::Cas::Fence::open());
    auto op = requests.admit();
    op.publish(request, DB::Cas::Retry::once());
}

/// A failed creating request, and the verb that fails.
struct Fault
{
    std::string name;
    CountingS3::Responder (*responder)(Verb verb);
    /// Only `CompleteMultipartUpload` can answer it.
    bool completion_only;
};

/// One blob publication path: the verb that creates the object, and how to reach it.
struct PublicationPath
{
    std::string name;
    Verb creating_verb;
    void (*adjust_settings)(DB::S3Settings &);
    bool staged_copy;
};

const PublicationPath publication_paths[] = {
    {"Put", Verb::PutObject, nullptr, false},
    {"Copy", Verb::CopyObject, nullptr, true},
    {"Complete", Verb::CompleteMultipartUpload, forceOnePartMultipart, false},
    {"MultipartCopyComplete", Verb::CompleteMultipartUpload, forceOnePartMultipartCopy, true},
};

const Fault faults[] = {
    {"LostResponse", [](Verb verb) { return CountingS3::dropping(verb); }, false},
    /// The `NO_SUCH_KEY` loops above the client would reissue the identical envelope.
    {"NoSuchKey", [](Verb verb) { return CountingS3::failing(verb, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"); }, false},
    /// An object at the key does not prove that this upload created it: it may be a condemned incarnation.
    {"NoSuchUpload",
     [](Verb verb) { return CountingS3::failing(verb, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchUpload"); },
     true},
};

using FaultedPublication = std::tuple<PublicationPath, Fault>;

class CASBlobPublishTransportFault : public ::testing::TestWithParam<FaultedPublication>
{
};

std::vector<FaultedPublication> faultedPublications()
{
    std::vector<FaultedPublication> cases;
    for (const auto & path : publication_paths)
        for (const auto & fault : faults)
            if (!fault.completion_only || path.creating_verb == Verb::CompleteMultipartUpload)
                cases.emplace_back(path, fault);
    return cases;
}

}

TEST_P(CASBlobPublishTransportFault, OneWireRequest)
{
    (void)getContext();
    const auto & [path, fault] = GetParam();
    CountingS3Server server(fault.responder(path.creating_verb));
    auto storage = makeRetryingStorage(server.getUrl(), path.adjust_settings ? path.adjust_settings : std::function<void(DB::S3Settings &)>{});

    EXPECT_EQ(
        thrownCode([&] { publishOnce(storage, path.staged_copy ? stagedCopyPublication() : streamingPublication()); }),
        DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(path.creating_verb), 1u);
}

INSTANTIATE_TEST_SUITE_P(
    CASBlobPublishTransport,
    CASBlobPublishTransportFault,
    ::testing::ValuesIn(faultedPublications()),
    [](const ::testing::TestParamInfo<FaultedPublication> & param_info)
    { return std::get<0>(param_info.param).name + "_" + std::get<1>(param_info.param).name; });

#endif
