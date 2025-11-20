#pragma once
#include <Storages/ObjectStorage/StorageObjectStorage.h>

#include <Disks/ObjectStorages/IObjectStorage_fwd.h>
#include <mutex>
#include <map>

namespace DB
{

class IObjectStorage;

/// Thread-safe wrapper for secondary object storages map
struct SecondaryStorages
{
    mutable std::mutex mutex;
    std::map<std::string, ObjectStoragePtr> storages;
};

// A URI splitted into components
//  s3://bucket/a/b -> scheme="s3", authority="bucket", path="/a/b"
//  file:///var/x    -> scheme="file", authority="",     path="/var/x"
//  /abs/p           -> scheme="",     authority="",     path="/abs/p"
struct SchemeAuthorityKey
{
    explicit SchemeAuthorityKey(const std::string & uri);

    std::string scheme;
    std::string authority;
    std::string key;
};

std::optional<std::string> checkAndGetNewFileOnInsertIfNeeded(
    const IObjectStorage & object_storage,
    const StorageObjectStorageConfiguration & configuration,
    const StorageObjectStorageQuerySettings & settings,
    const std::string & key,
    size_t sequence_number);

void resolveSchemaAndFormat(
    ColumnsDescription & columns,
    ObjectStoragePtr object_storage,
    StorageObjectStorageConfigurationPtr & configuration,
    std::optional<FormatSettings> format_settings,
    std::string & sample_path,
    const ContextPtr & context);

void validateSupportedColumns(
    ColumnsDescription & columns,
    const StorageObjectStorageConfiguration & configuration);

std::unique_ptr<ReadBufferFromFileBase> createReadBuffer(
    ObjectInfo & object_info,
    const ObjectStoragePtr & object_storage,
    const ContextPtr & context_,
    const LoggerPtr & log,
    const std::optional<ReadSettings> & read_settings = std::nullopt);

std::string makeAbsolutePath(const std::string & table_location, const std::string & path);

/// Resolve object storage and key for reading from that storage
/// If path is relative -- it must be read from base_storage
/// Otherwise, look for a suitable storage in secondary_storages
std::pair<DB::ObjectStoragePtr, std::string> resolveObjectStorageForPath(
    const std::string & table_location,
    const std::string & path,
    const DB::ObjectStoragePtr & base_storage,
    SecondaryStorages & secondary_storages,
    const DB::ContextPtr & context);

}
