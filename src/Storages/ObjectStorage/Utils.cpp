#include <Core/Settings.h>
#include <Disks/IO/AsynchronousBoundedReadBuffer.h>
#include <Disks/IO/CachedOnDiskReadBufferFromFile.h>
#include <Disks/IO/getThreadPoolReader.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <Interpreters/Cache/FileCache.h>
#include <Interpreters/Cache/FileCacheFactory.h>
#include <Interpreters/Cache/FileCacheKey.h>
#include <Interpreters/Context.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Disks/ObjectStorages/ObjectStorageFactory.h>
#include <Poco/Util/MapConfiguration.h>
#include <IO/S3/URI.h>
#include <filesystem>
#if USE_AWS_S3
#include <Disks/ObjectStorages/S3/S3ObjectStorage.h>
#endif
#if USE_HDFS
#include <Disks/ObjectStorages/HDFS/HDFSObjectStorage.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
}

namespace
{

inline std::string normalizeSchema(const std::string & schema)
{
    auto schema_lowercase = Poco::toLower(schema);

    if (schema_lowercase == "s3a" || schema_lowercase == "s3n")
        schema_lowercase = "s3";
    else if (schema_lowercase == "wasb" || schema_lowercase == "wasbs" || schema_lowercase == "abfss")
        schema_lowercase = "abfs";

    return schema_lowercase;
}

inline std::string endpoint_cache_key(const std::string & normalized_scheme, const std::string & authority)
{
    return normalized_scheme + "://" + authority;
}

static std::string factoryTypeForScheme(const std::string & normalized_scheme)
{
    if (normalized_scheme == "s3") return "s3";
    if (normalized_scheme == "abfs") return "azure";
    if (normalized_scheme == "hdfs") return "hdfs";
    if (normalized_scheme == "file") return "local";
    return "";
}

}

// TODO: handle https://s3.amazonaws.com/bucketname/... properly
SchemeAuthorityKey::SchemeAuthorityKey(const std::string & uri)
{
    if (uri.empty())
        return;

    // scheme://authority/path
    if (auto scheme_sep = uri.find("://"); scheme_sep != std::string_view::npos)
    {
        scheme = Poco::toLower(uri.substr(0, scheme_sep));
        auto rest = uri.substr(scheme_sep + 3); // skip ://

        // authority is up to next '/'
        auto slash = rest.find('/');
        if (slash == std::string_view::npos)
        {
            authority = std::string(rest);
            key = "/";   // Happy debugging. FIXME: throw exception, path obviously incorrect
            return;
        }
        authority = std::string(rest.substr(0, slash));
        key = std::string(rest.substr(++slash)); // do not keep leading '/'
        return;
    }

    // if part has no scheme and starts with '/' -- it is an absolute uri for local file: file:///path
    if (uri.front() == '/')
    {
        scheme = "file";
        key = std::string(uri);
        return;
    }

    // Relative path, return as is
    key = std::string(uri);
}

std::optional<String> checkAndGetNewFileOnInsertIfNeeded(
    const IObjectStorage & object_storage,
    const StorageObjectStorageConfiguration & configuration,
    const StorageObjectStorageQuerySettings & settings,
    const String & key,
    size_t sequence_number)
{
    if (settings.truncate_on_insert
        || !object_storage.exists(StoredObject(key)))
        return std::nullopt;

    if (settings.create_new_file_on_insert)
    {
        auto pos = key.find_first_of('.');
        String new_key;
        do
        {
            new_key = key.substr(0, pos) + "." + std::to_string(sequence_number) + (pos == std::string::npos ? "" : key.substr(pos));
            ++sequence_number;
        }
        while (object_storage.exists(StoredObject(new_key)));

        return new_key;
    }

    throw Exception(
        ErrorCodes::BAD_ARGUMENTS,
        "Object in bucket {} with key {} already exists. "
        "If you want to overwrite it, enable setting {}_truncate_on_insert, if you "
        "want to create a new file on each insert, enable setting {}_create_new_file_on_insert",
        configuration.getNamespace(), key, configuration.getTypeName(), configuration.getTypeName());
}

void resolveSchemaAndFormat(
    ColumnsDescription & columns,
    ObjectStoragePtr object_storage,
    StorageObjectStorageConfigurationPtr & configuration,
    std::optional<FormatSettings> format_settings,
    std::string & sample_path,
    const ContextPtr & context)
{
    if (configuration->getFormat() == "auto")
    {
        if (configuration->isDataLakeConfiguration())
        {
            throw Exception(
                ErrorCodes::LOGICAL_ERROR,
                "Format must be already specified for {} storage.",
                configuration->getTypeName());
        }
    }

    if (columns.empty())
    {
        if (configuration->isDataLakeConfiguration())
        {
            auto table_structure = configuration->tryGetTableStructureFromMetadata();
            if (table_structure)
                columns = table_structure.value();
        }

        if (columns.empty())
        {
            if (configuration->getFormat() == "auto")
            {
                std::string format;
                std::tie(columns, format) = StorageObjectStorage::resolveSchemaAndFormatFromData(
                    object_storage, configuration, format_settings, sample_path, context);
                configuration->setFormat(format);
            }
            else
            {
                chassert(!configuration->getFormat().empty());
                columns = StorageObjectStorage::resolveSchemaFromData(object_storage, configuration, format_settings, sample_path, context);
            }
        }
    }
    else if (configuration->getFormat() == "auto")
    {
        configuration->setFormat(StorageObjectStorage::resolveFormatFromData(object_storage, configuration, format_settings, sample_path, context));
    }

    validateSupportedColumns(columns, *configuration);
}

void validateSupportedColumns(
    ColumnsDescription & columns,
    const StorageObjectStorageConfiguration & configuration)
{
    if (!columns.hasOnlyOrdinary())
    {
        /// We don't allow special columns.
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "Special columns like MATERIALIZED, ALIAS or EPHEMERAL are not supported for {} storage.",
            configuration.getTypeName());
    }
}

inline bool isSameStorageSchema(const std::string & schema1, const std::string & schema2)
{
    return normalizeSchema(schema1) == normalizeSchema(schema2);
}

std::string extractStorageType(const std::string & path)
{
    if (path.empty())
        return "";

    // Absolute POSIX path -> file
    if (path.front() == '/')
        return "file";

    // Look for "scheme://..."
    if (auto pos = path.find("://"); pos != std::string_view::npos && pos > 0)
        return Poco::toLower(path.substr(0, pos));

    // Tolerant: "scheme:..." with no slash before colon (avoid Windows drive letters like "C:\")
    {
        auto colon_pos = path.find(':');
        auto slash_pos = path.find('/');
        if (colon_pos != std::string_view::npos && (slash_pos == std::string_view::npos || colon_pos < slash_pos))
        {
            auto maybe_scheme = path.substr(0, colon_pos);
            // Heuristic: treat single-letter prefix followed by ":\\" as Windows drive, not a scheme
            if (!(maybe_scheme.size() == 1 && colon_pos + 1 < path.size() && (path[colon_pos + 1] == '\\' || path[colon_pos + 1] == '/')))
                return Poco::toLower(maybe_scheme);
        }
    }

    // Relative path or unknown
    return "";
}

bool isRelativePath(const std::string & path)
{
    if (path.empty())
        return true;

    // Non-relative if it has a scheme (e.g., s3://, file://)
    if (!extractStorageType(path).empty())
        return false;

    return true;
}


std::string normalizePathToStorageRoot(const std::string & table_location, const std::string & path)
{
    if (table_location.empty())
    {
        // Even when table_location is empty, normalize paths that start with '/'
        std::string result = path;
        if (!result.empty() && result.front() == '/')
            result = result.substr(1);
        return result;
    }

    if (!isRelativePath(path))
    {
        SchemeAuthorityKey target{path};
        return target.key;
    }

    SchemeAuthorityKey base{table_location};
    if (base.key.empty())
        return path;

    std::string rel_path = path;
    if (!rel_path.empty() && rel_path.front() == '/')
        rel_path = rel_path.substr(1);

    std::string base_key_trimmed = base.key;
    while (!base_key_trimmed.empty() && base_key_trimmed.front() == '/')
        base_key_trimmed = base_key_trimmed.substr(1);
    while (!base_key_trimmed.empty() && base_key_trimmed.back() == '/')
        base_key_trimmed.pop_back();

    // Check if the path already starts with the base.key (table location)
    // This handles cases where paths in metadata are already absolute within the storage
    if (!base_key_trimmed.empty() && (rel_path == base_key_trimmed || rel_path.starts_with(base_key_trimmed + "/")))
    {
        // Path already includes table location, return it as-is (normalized to remove any ".." or ".")
        std::filesystem::path fs_path(rel_path);
        std::filesystem::path normalized = fs_path.lexically_normal();
        
        std::string normalized_result = normalized.string();
        std::replace(normalized_result.begin(), normalized_result.end(), '\\', '/');
        
        while (!normalized_result.empty() && normalized_result.front() == '/')
            normalized_result = normalized_result.substr(1);
        
        return normalized_result;
    }

    // First, try combining and normalizing to see if the path resolves outside the table location
    // This handles cases where paths use ".." to go up from table location to shared parent
    std::string combined = base.key;
    if (!combined.empty() && combined.back() != '/')
        combined += '/';
    combined += rel_path;

    // Normalize the path to resolve "..", "."; remove duplicate slashes (just in case)
    std::filesystem::path fs_path(combined);
    std::filesystem::path normalized = fs_path.lexically_normal();
    
    std::string normalized_result = normalized.string();
    std::replace(normalized_result.begin(), normalized_result.end(), '\\', '/');
    
    while (!normalized_result.empty() && normalized_result.front() == '/')
        normalized_result = normalized_result.substr(1);

    // Check if the normalized path still starts with base.key (meaning it's under table location)
    // or if it doesn't (meaning it resolved outside, e.g., via "..")
    if (!normalized_result.empty() && !base.key.empty())
    {
        // If normalized path doesn't start with base.key, it means the original path
        // used ".." to go outside the table location - use the normalized path as-is
        if (!normalized_result.starts_with(base.key + "/") && normalized_result != base.key)
        {
            return normalized_result;
        }
    }

    return normalized_result;
}

std::string makeAbsolutePath(const std::string & table_location, const std::string & path)
{
    if (!isRelativePath(path))
        return path;

    auto base = SchemeAuthorityKey(table_location);

    std::string normalized_key = normalizePathToStorageRoot(table_location, path);
    
    std::string abs_path = "/" + normalized_key;

    if (!base.scheme.empty())
        return base.scheme + "://" + base.authority + abs_path;

    return normalized_key;
}

std::pair<DB::ObjectStoragePtr, std::string> resolveObjectStorageForPath(
    const std::string & table_location,
    const std::string & path,
    const DB::ObjectStoragePtr & base_storage,
    std::map<std::string, DB::ObjectStoragePtr> & secondary_storages,
    const DB::ContextPtr & context)
{
    if (isRelativePath(path))
    {
        // For relative paths, normalize to storage root
        std::string normalized_key = normalizePathToStorageRoot(table_location, path);
        return {base_storage, normalized_key}; // Relative path definitely goes to base storage
    }

    SchemeAuthorityKey base{table_location};
    SchemeAuthorityKey target{path};

    if (target.scheme.empty())
    {
        // Path has no scheme but wasn't caught by isRelativePath
        // Treat it as relative and normalize to storage root
        std::string normalized_key = normalizePathToStorageRoot(table_location, target.key);
        return {base_storage, normalized_key};
    }

    const std::string base_norm = normalizeSchema(base.scheme);
    const std::string target_norm = normalizeSchema(target.scheme);

    // For S3 URIs, use S3::URI to properly handle all kinds of URIs, e.g. https://s3.amazonaws.com/bucket/... == s3://bucket/...
    #if USE_AWS_S3
    if (target_norm == "s3" || target_norm == "https" || target_norm == "http")
    {
        try
        {
            // Normalize s3a:// and s3n:// to s3:// for S3::URI parsing
            std::string normalized_path = path;
            if (target.scheme == "s3a" || target.scheme == "s3n")
            {
                normalized_path = "s3://" + target.authority + "/" + target.key;
            }
            S3::URI s3_uri(normalized_path);

            std::string key_to_use = s3_uri.key;

            bool use_base_storage = false;
            if (base_storage->getType() == ObjectStorageType::S3)
            {
                if (auto s3_storage = std::dynamic_pointer_cast<S3ObjectStorage>(base_storage))
                {
                    const std::string base_bucket = s3_storage->getObjectsNamespace();
                    const std::string base_endpoint = s3_storage->getDescription();
                    
                    // If bucket matches, use base storage. For s3:// URIs (which don't have explicit endpoint),
                    // we should use base storage if bucket matches, regardless of endpoint.
                    // For explicit http(s):// URIs, we require both bucket and endpoint to match.
                    bool bucket_matches = (s3_uri.bucket == base_bucket);
                    bool endpoint_matches = (s3_uri.endpoint == base_endpoint);
                    bool is_generic_s3_uri = (target_norm == "s3");
                    
                    if (bucket_matches && (endpoint_matches || is_generic_s3_uri))
                    {
                        use_base_storage = true;
                    }
                }
            }
            
            // Also check if table_location is provided and matches
            if (!use_base_storage && (base_norm == "s3" || base_norm == "https" || base_norm == "http"))
            {
                // Normalize s3a:// and s3n:// in table_location to s3:// for S3::URI parsing
                std::string normalized_table_location = table_location;
                if (base.scheme == "s3a" || base.scheme == "s3n")
                {
                    normalized_table_location = "s3://" + base.authority + "/" + base.key;
                }
                S3::URI base_s3_uri(normalized_table_location);

                // For s3:// URIs, match by bucket only. For explicit http(s):// URIs, require both bucket and endpoint.
                bool bucket_matches = (s3_uri.bucket == base_s3_uri.bucket);
                bool endpoint_matches = (s3_uri.endpoint == base_s3_uri.endpoint);
                bool is_generic_s3_uri = (target_norm == "s3");
                
                if (bucket_matches && (endpoint_matches || is_generic_s3_uri))
                {
                    use_base_storage = true;
                }
            }
            
            if (use_base_storage)
                return {base_storage, key_to_use};

            const std::string cache_key = "s3://" + s3_uri.bucket + "@" + (s3_uri.endpoint.empty() ? "amazonaws.com" : s3_uri.endpoint);

            if (auto it = secondary_storages.find(cache_key); it != secondary_storages.end())
                return {it->second, key_to_use};

            /// TODO: maybe do not invent new configuration. Use old one and clean up later
            Poco::AutoPtr<Poco::Util::MapConfiguration> cfg(new Poco::Util::MapConfiguration);
            const std::string config_prefix = "object_storages." + cache_key;

            cfg->setString(config_prefix + ".object_storage_type", "s3");

            // Use the full endpoint or construct it from bucket
            std::string endpoint = s3_uri.endpoint.empty()
                ? ("https://" + s3_uri.bucket + ".s3.amazonaws.com")
                : s3_uri.endpoint;
            cfg->setString(config_prefix + ".endpoint", endpoint);

            // Copy credentials from base storage if it's also S3
            if (base_storage->getType() == ObjectStorageType::S3)
            {
                if (auto s3_storage = std::dynamic_pointer_cast<S3ObjectStorage>(base_storage))
                {
                    if (auto s3_client = s3_storage->tryGetS3StorageClient())
                    {
                        const auto credentials = s3_client->getCredentials();
                        const String & access_key_id = credentials.GetAWSAccessKeyId();
                        const String & secret_access_key = credentials.GetAWSSecretKey();
                        const String & session_token = credentials.GetSessionToken();
                        const String & region = s3_client->getRegion();

                        if (!access_key_id.empty())
                            cfg->setString(config_prefix + ".access_key_id", access_key_id);
                        if (!secret_access_key.empty())
                            cfg->setString(config_prefix + ".secret_access_key", secret_access_key);
                        if (!session_token.empty())
                            cfg->setString(config_prefix + ".session_token", session_token);
                        if (!region.empty())
                            cfg->setString(config_prefix + ".region", region);
                    }
                }
            }

            auto & factory = DB::ObjectStorageFactory::instance();

            DB::ObjectStoragePtr storage = factory.create(cache_key, *cfg, config_prefix, context, /*skip_access_check*/ true);

            secondary_storages.emplace(cache_key, storage);
            return {storage, key_to_use};
        }
        catch (const Exception &) // NOLINT
        {
            // If S3::URI parsing fails, fall back to the old logic
        }
    }
    #endif

    // For HDFS URIs, check if we can reuse base storage
    if (target_norm == "hdfs")
    {
        // Check if base storage is HDFS and matches the path's endpoint
        bool use_base_storage = false;
        if (base_storage->getType() == ObjectStorageType::HDFS)
        {
            #if USE_HDFS
            if (auto hdfs_storage = std::dynamic_pointer_cast<HDFSObjectStorage>(base_storage))
            {
                const std::string base_url = hdfs_storage->getDescription();
                // Extract endpoint from base URL (hdfs://namenode:port/path -> hdfs://namenode:port)
                std::string base_endpoint;
                if (auto pos = base_url.find('/', base_url.find("//") + 2); pos != std::string::npos)
                    base_endpoint = base_url.substr(0, pos);
                else
                    base_endpoint = base_url;
                
                // For HDFS, compare endpoints (namenode addresses)
                std::string target_endpoint = target_norm + "://" + target.authority;
                
                if (base_endpoint == target_endpoint)
                {
                    use_base_storage = true;
                }
            }
            #endif
        }
        
        // Also check if table_location is provided and matches
        if (!use_base_storage && base_norm == "hdfs")
        {
            std::string base_endpoint = base_norm + "://" + base.authority;
            std::string target_endpoint = target_norm + "://" + target.authority;
            
            if (base_endpoint == target_endpoint)
            {
                use_base_storage = true;
            }
        }
        
        if (use_base_storage)
            return {base_storage, target.key};
    }

    // Reuse base storage if scheme and authority (bucket) matches
    if (base_norm == target_norm && base.authority == target.authority)
    {
        return {base_storage, target.key};
    }

    const std::string cache_key = endpoint_cache_key(target_norm, target.authority);
    if (auto it = secondary_storages.find(cache_key); it != secondary_storages.end())
    {
        return {it->second, target.key};
    }

    /// TODO: maybe do not invent new configuration. Use old one and clean up later
    Poco::AutoPtr<Poco::Util::MapConfiguration> cfg(new Poco::Util::MapConfiguration);

    const std::string type_for_factory = factoryTypeForScheme(target_norm);
    if (type_for_factory.empty())
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "Unsupported storage scheme '{}' in path '{}'", target_norm, path);

    const std::string config_prefix = "object_storages." + cache_key;

    cfg->setString(config_prefix + ".object_storage_type", type_for_factory);

    if (target_norm == "s3" || target_norm == "abfs")
    {
        cfg->setString(config_prefix + ".endpoint", target_norm + "://" + target.authority);
    }
    else if (target_norm == "hdfs")
    {
        // HDFS endpoint must end with '/'
        auto endpoint = target_norm + "://" + target.authority;
        if (!endpoint.empty() && endpoint.back() != '/')
            endpoint.push_back('/');
        cfg->setString(config_prefix + ".endpoint", endpoint);
    }
    else if (target_norm == "file")
    {
        // For file:// URIs, extract the directory path
        // file:///absolute/path/to/file -> path = /absolute/path/to/, key = /absolute/path/to/file (full path)
        std::string full_path = "/" + target.key;  // Reconstruct full path
        std::filesystem::path fs_path(full_path);
        std::filesystem::path parent = fs_path.parent_path();
        std::string dir_path = parent.string();
        
        // Ensure directory path ends with /
        if (dir_path.empty() || dir_path == "/")
        {
            // Root directory
            dir_path = "/";
        }
        else if (dir_path.back() != '/')
        {
            dir_path += '/';
        }
        
        cfg->setString(config_prefix + ".path", dir_path);
        
        auto & factory = DB::ObjectStorageFactory::instance();

        // Also use the cache key as a (unique) storage name
        DB::ObjectStoragePtr storage = factory.create(cache_key, *cfg, config_prefix, context, /*skip_access_check*/ true);

        secondary_storages.emplace(cache_key, storage);

        // For local storage, getObjectMetadata expects the full path, not just the filename
        // Return the full absolute path as the key
        return {storage, full_path};
    }

    auto & factory = DB::ObjectStorageFactory::instance();

    // Also use the cache key as a (unique) storage name
    DB::ObjectStoragePtr storage = factory.create(cache_key, *cfg, config_prefix, context, /*skip_access_check*/ true);

    secondary_storages.emplace(cache_key, storage);

    return {storage, target.key};
}

}
