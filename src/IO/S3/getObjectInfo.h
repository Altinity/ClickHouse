#pragma once

#include "config.h"

#if USE_AWS_S3
#include <vector>
#include <IO/S3Settings.h>
#include <base/types.h>
#include <IO/S3/Client.h>

namespace DB::S3
{

struct ObjectInfo
{
    size_t size = 0;
    /// Checks if Content-Length was present in the HEAD response and we can rely on its result
    bool is_size_known = true;
    time_t last_modification_time = 0;
    String etag;
    ObjectAttributes tags; // Set only if getObjectInfo() is called with `with_tags = true`
    ObjectAttributes metadata = {}; /// Set only if getObjectInfo() is called with `with_metadata = true`.
    /// Sizes of the object's multipart-upload parts (offsets = prefix sums). Set only by
    /// getObjectIdentity() when the object was multipart-uploaded and GetObjectAttributes succeeded;
    /// empty for single-PUT objects or when the call fell back to HEAD.
    std::vector<size_t> part_sizes;
};

/// Ignore if object does not exist
ObjectInfo getObjectInfoIfExists(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {},
    bool with_metadata = false,
    bool with_tags = false);

ObjectInfo getObjectInfo(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {},
    bool with_metadata = false,
    bool with_tags = false);

/// Fetches size + etag (+ multipart part sizes) in a single GetObjectAttributes request instead of a
/// HEAD, so a caller that also wants the multipart layout gets it without a second round-trip. Falls
/// back to getObjectInfo() (HEAD) if GetObjectAttributes is unsupported, denied, or otherwise errors,
/// so it is safe against S3-compatible stores that lack the API. `part_sizes` is populated only on
/// the GetObjectAttributes path for multipart objects; it is empty on the fallback path.
ObjectInfo getObjectIdentity(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {});

ObjectAttributes getObjectTags(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {});

size_t getObjectSize(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {});

bool objectExists(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {});

/// Throws an exception if a specified object doesn't exist. `description` is used as a part of the error message.
void checkObjectExists(
    const S3::Client & client,
    const String & bucket,
    const String & key,
    const String & version_id = {},
    std::string_view description = {});

bool isNotFoundError(Aws::S3::S3Errors error);
bool isAuthenticationError(Aws::S3::S3Errors error);

}

#endif
