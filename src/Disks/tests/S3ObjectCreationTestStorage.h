#pragma once

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <IO/S3/Client.h>
#include <IO/S3/PocoHTTPClient.h>
#include <IO/S3/URI.h>
#include <IO/S3Settings.h>
#include <Common/RemoteHostFilter.h>

#include <functional>
#include <memory>
#include <string>

namespace DB::S3RequestSetting
{
extern const S3RequestSettingsUInt64 max_single_operation_copy_size;
extern const S3RequestSettingsUInt64 max_single_part_upload_size;
extern const S3RequestSettingsUInt64 min_upload_part_size;
}

/// `S3ObjectStorage` instances for tests against `CountingS3::CountingS3Server`.
namespace S3ObjectCreationTestStorage
{

/// A genuine `S3ObjectStorage` whose client retries a retryable failure twice, as a disk's client does.
/// `adaptive_timeouts` gives each request's first attempt the short adaptive timeouts, as on a disk
/// with the default `use_adaptive_timeouts`.
inline std::shared_ptr<DB::S3ObjectStorage> makeRetryingStorage(
    const std::string & endpoint, const std::function<void(DB::S3Settings &)> & adjust_settings = {},
    bool adaptive_timeouts = false, long request_timeout_ms = 10000)
{
    DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 2, .initial_delay_ms = 1, .max_delay_ms = 1},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ true,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = endpoint;
    cfg.connectTimeoutMs = 10000;
    cfg.requestTimeoutMs = request_timeout_ms;
    cfg.s3_use_adaptive_timeouts = adaptive_timeouts;
    /// A fresh connection per request: a pooled one can outlive this test's ephemeral-port server.
    cfg.http_keep_alive_timeout = 0;
    auto client = DB::S3::ClientFactory::instance().create(
        cfg,
        DB::S3::ClientSettings{
            .use_virtual_addressing = false,
            .disable_checksum = false,
            .gcs_issue_compose_request = false,
            .is_s3express_bucket = false,
        },
        "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});

    auto settings = std::make_unique<DB::S3Settings>();
    if (adjust_settings)
        adjust_settings(*settings);
    return std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::move(settings), DB::S3::URI(endpoint + "/test-bucket/"), DB::S3Capabilities{},
        DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

/// A body above the single-part ceiling that fits one part, so a write is exactly one `UploadPart`.
inline void forceOnePartMultipart(DB::S3Settings & settings)
{
    settings.request_settings[DB::S3RequestSetting::max_single_part_upload_size] = 16;
    settings.request_settings[DB::S3RequestSetting::min_upload_part_size] = 1024 * 1024;
}

/// A staged object above the single-operation copy ceiling that fits one part, so a copy is exactly one
/// `UploadPartCopy`.
inline void forceOnePartMultipartCopy(DB::S3Settings & settings)
{
    settings.request_settings[DB::S3RequestSetting::max_single_operation_copy_size] = 16;
    settings.request_settings[DB::S3RequestSetting::min_upload_part_size] = 1024 * 1024;
}

}

#endif
