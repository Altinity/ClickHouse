#pragma once

#include <Common/IThrottler.h>
#include <Common/Scheduler/ResourceLink.h>
#include <IO/DistributedCacheSettings.h>

namespace DB
{

/// Settings to be passed to IDisk::writeFile()
struct WriteSettings
{
    /// Bandwidth throttler to use during writing
    ThrottlerPtr remote_throttler;
    ThrottlerPtr local_throttler;

    IOSchedulingSettings io_scheduling;

    /// Filesystem cache settings
    bool enable_filesystem_cache_on_write_operations = false;
    bool enable_filesystem_cache_log = false;
    bool throw_on_error_from_cache = false;
    size_t filesystem_cache_reserve_space_wait_lock_timeout_milliseconds = 1000;

    bool s3_allow_parallel_part_upload = true;
    /// Skip the post-upload existence/size HEAD (check_objects_after_upload) for this write. Set
    /// by writers of CAS-MUTABLE keys (the content-addressed shard manifests): such a key is
    /// legitimately replaced by a concurrent conditional PUT between this upload and the check's
    /// HEAD, so the size comparison false-positives ("it's a bug in S3") under normal contention.
    /// Integrity for these keys is the conditional PUT outcome + token, not a recheck.
    bool s3_skip_check_objects_after_upload = false;
    bool azure_allow_parallel_part_upload = true;

    bool use_adaptive_write_buffer = false;
    size_t adaptive_write_buffer_initial_size = 16 * 1024;

    bool write_through_distributed_cache = false;
    DistributedCacheSettings distributed_cache_settings;

    bool is_initial_access_check = false;

    std::string object_storage_write_if_none_match; /// Supported only for S3-like object storages.
    std::string object_storage_write_if_match;     /// Supported only for S3-like object storages.

    bool operator==(const WriteSettings & other) const = default;
};

WriteSettings getWriteSettings();

WriteSettings getWriteSettingsForMetadata();
}
