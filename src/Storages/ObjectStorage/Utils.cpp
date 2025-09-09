#include <Storages/ObjectStorage/Utils.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <Disks/ObjectStorages/ObjectStorageFactory.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Poco/Util/MapConfiguration.h>
#include <IO/S3/URI.h>


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
    const StorageObjectStorage::Configuration & configuration,
    const StorageObjectStorage::QuerySettings & settings,
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
    StorageObjectStorage::ConfigurationPtr configuration,
    const std::optional<FormatSettings> & format_settings,
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
    const StorageObjectStorage::Configuration & configuration)
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

std::string makeAbsolutePath(const std::string & table_location, const std::string & path)
{
    if (!isRelativePath(path))
        return path;

    auto base = SchemeAuthorityKey(table_location);

    std::string base_dir = base.key.empty() ? std::string("/") : base.key;
    if (!base_dir.empty() && base_dir.back() != '/')
        base_dir.push_back('/');

    std::string rel = path;
    if (!rel.empty() && rel.front() == '/')
        rel.erase(0, 1);

    std::string abs_path = base_dir + rel;
    if (abs_path.empty() || abs_path.front() != '/')
        abs_path.insert(abs_path.begin(), '/');

    if (!base.scheme.empty())
        return base.scheme + "://" + base.authority + abs_path;

    return std::string("file://") + abs_path;
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
        // For relative paths, we need to construct the "full" key: table location + relative path
        std::string full_key = path;

        if (!table_location.empty())
        {
            SchemeAuthorityKey base{table_location};
            if (!base.key.empty())
            {
                // Combine base key with relative path
                full_key = base.key;
                if (!full_key.empty() && full_key.back() != '/')
                    full_key += '/';

                std::string rel_path = path;
                if (!rel_path.empty() && rel_path.front() == '/')
                    rel_path = rel_path.substr(1);

                full_key += rel_path;
            }
        }

        if (base_storage)
            std::cerr << "\nRelative path: " << base_storage->getName() << ", " << base_storage->getObjectsNamespace() << ". full_key: " << full_key << "\n";
        else
            std::cerr << "\nRelative, returning base, which is: NULL\n";
        return {base_storage, full_key}; // Relative path definitely goes to base storage
    }

    SchemeAuthorityKey base{table_location};
    SchemeAuthorityKey target{path};

    if (target.scheme.empty())
    {
        if (base_storage)
            std::cerr << "\nNo scheme, returning base, which is: " << base_storage->getName() << ", " << base_storage->getObjectsNamespace() << "\n";
        else

            std::cerr << "\nNo scheme, returning base, which is: NULL\n";
        return {base_storage, target.key};
    }

    const std::string base_norm = normalizeSchema(base.scheme);
    const std::string target_norm = normalizeSchema(target.scheme);

    // For S3 URIs, use S3::URI to properly URLs: https://s3.amazonaws.com/.....
    #if USE_AWS_S3
    if (target_norm == "s3" || target_norm == "https" || target_norm == "http")
    {
        try
        {
            S3::URI s3_uri(path);

            if (base_norm == "s3" || base_norm == "https" || base_norm == "http")
            {
                S3::URI base_s3_uri(table_location);

                if (s3_uri.bucket == base_s3_uri.bucket && s3_uri.endpoint == base_s3_uri.endpoint)
                {
                    std::cerr << "\nPath: " << path << "\n";
                    if (base_storage)
                        std::cerr << "\nSame S3 location, returning base, which is: " << base_storage->getName() << ", " << base_storage->getObjectsNamespace() << "\n";
                    else
                        std::cerr << "\nSame S3 location, returning base, which is: NULL\n";
                    return {base_storage, s3_uri.key};
                }
                else
                {
                    std::cerr << "\nS3 storages are different:\n" << base_s3_uri.bucket << " @ " << base_s3_uri.endpoint
                              << " vs " << s3_uri.bucket << " @ " << s3_uri.endpoint << "\n";
                }
            }

            const std::string cache_key = "s3://" + s3_uri.bucket + "@" + (s3_uri.endpoint.empty() ? "amazonaws.com" : s3_uri.endpoint);

            if (auto it = secondary_storages.find(cache_key); it != secondary_storages.end())
            {
                if (it->second)
                    std::cerr << "\nfound in cache: " << it->second->getName() << ", " << it->second->getObjectsNamespace() << "\n";
                else
                    std::cerr << "\nfound in cache: NULL\n";
                return {it->second, s3_uri.key};
            }

            /// TODO: maybe do not invent new configuration. Use old one and clean up later
            Poco::AutoPtr<Poco::Util::MapConfiguration> cfg(new Poco::Util::MapConfiguration);

            const std::string config_prefix = "object_storages." + cache_key;

            cfg->setString(config_prefix + ".object_storage_type", "s3");

            // Use the full endpoint or construct it from bucket
            std::string endpoint = s3_uri.endpoint.empty()
                ? ("https://" + s3_uri.bucket + ".s3.amazonaws.com")
                : s3_uri.endpoint;
            cfg->setString(config_prefix + ".endpoint", endpoint);

            auto & factory = DB::ObjectStorageFactory::instance();

            DB::ObjectStoragePtr storage = factory.create(cache_key, *cfg, config_prefix, context, /*skip_access_check*/ true);

            secondary_storages.emplace(cache_key, storage);
            if (storage)
                std::cerr << "\ncreated new S3 storage: " << storage->getName() << ", " << storage->getObjectsNamespace() << ":\ndescr: " << storage->getDescription() << "\n";
            else
                std::cerr << "\ncreated new S3 storage: it is NULL\n";
            return {storage, s3_uri.key};
        }
        catch (...)
        {
            // If S3::URI parsing fails, fall back to the old logic
        }
    }
    #endif


    // Reuse base storage if scheme and authority (bucket) matches
    if (base_norm == target_norm && base.authority == target.authority)
    {
        if (base_storage)
            std::cerr << "\nSame location, returning base, which is: " << base_storage->getName() << ", " << base_storage->getObjectsNamespace() << "\n";
        else
            std::cerr << "\nSame location, returning base, which is: NULL\n";
        return {base_storage, target.key};
    }
    else
    {
        std::cerr << "\nStorages are different:\n" << base.scheme << "://" << base.authority
                  << " vs " << target.scheme << "://" << target.authority << "\n";
    }

    const std::string cache_key = endpoint_cache_key(target_norm, target.authority);
    if (auto it = secondary_storages.find(cache_key); it != secondary_storages.end())
    {
        if (it->second)
            std::cerr << "\nfound in cache: " << it->second->getName() << ", " << it->second->getObjectsNamespace() << "\n";
        else
            std::cerr << "\nfound in cache: NULL\n";
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
    // No extra config needed for local storage (file://)

    auto & factory = DB::ObjectStorageFactory::instance();

    // Also use the cache key as a (unique) storage name
    DB::ObjectStoragePtr storage = factory.create(cache_key, *cfg, config_prefix, context, /*skip_access_check*/ true);

    secondary_storages.emplace(cache_key, storage);
    if (storage)
        std::cerr << "\ncreated new storage: " << storage->getName() << ", " << storage->getObjectsNamespace() << ":\ndescr: " << storage->getDescription() << "\n";
    else
        std::cerr << "\ncreated new storage: it is NULL\n";
    return {storage, target.key};
}

}
