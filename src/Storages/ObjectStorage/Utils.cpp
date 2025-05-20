#include <Storages/ObjectStorage/Utils.h>
#include <Disks/ObjectStorages/IObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Core/Settings.h>
#include <boost/algorithm/string/replace.hpp>
#include <Functions/generateSnowflakeID.h>
#include <Storages/ObjectStorage/StorageObjectStorageSink.h>

namespace DB
{

namespace Setting
{
    extern const SettingsBool object_storage_treat_key_wildcard_as_star;
}

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
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
    std::optional<FormatSettings> format_settings,
    std::string & sample_path,
    const ContextPtr & context)
{
    /*
    * Replace `_partition_id` and `_snowflake_id` wildcards with `*` so that any files that match this pattern can be retrieved.
    */
    auto old_path = configuration->getPath();
    if (context->getSettingsRef()[Setting::object_storage_treat_key_wildcard_as_star])
    {
        const auto path_without_partition_id_wildcard = PartitionedSink::replaceWildcards(configuration->getPath(), "*");

        const auto no_key_related_wildcard_path = replaceSnowflakeIdWildcard(path_without_partition_id_wildcard, "*");

        configuration->setPath(no_key_related_wildcard_path);
    }

    if (columns.empty())
    {
        if (configuration->getFormat() == "auto")
        {
            std::string format;
            std::tie(columns, format) =
                StorageObjectStorage::resolveSchemaAndFormatFromData(object_storage, configuration, format_settings, sample_path, context);
            configuration->setFormat(format);
        }
        else
            columns = StorageObjectStorage::resolveSchemaFromData(object_storage, configuration, format_settings, sample_path, context);
    }
    else if (configuration->getFormat() == "auto")
    {
        configuration->setFormat(StorageObjectStorage::resolveFormatFromData(object_storage, configuration, format_settings, sample_path, context));
    }

    // restored globbed path
    configuration->setPath(old_path);

    if (!columns.hasOnlyOrdinary())
    {
        /// We don't allow special columns.
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Special columns are not supported for {} storage"
                        "like MATERIALIZED, ALIAS or EPHEMERAL", configuration->getTypeName());
    }
}

std::string replaceSnowflakeIdWildcard(const std::string & input, const std::string & snowflake_id)
{
    return boost::replace_all_copy(input, "{_snowflake_id}", snowflake_id);
}

std::string fillSnowflakeIdWildcard(const std::string & input)
{
    return replaceSnowflakeIdWildcard(input, std::to_string(generateSnowflakeID()));
}

}
