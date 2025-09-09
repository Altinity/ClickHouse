#pragma once
#include "StorageObjectStorage.h"

#include <Disks/ObjectStorages/IObjectStorage_fwd.h>

namespace DB
{

class IObjectStorage;

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
    const StorageObjectStorage::Configuration & configuration,
    const StorageObjectStorage::QuerySettings & settings,
    const std::string & key,
    size_t sequence_number);

void resolveSchemaAndFormat(
    ColumnsDescription & columns,
    ObjectStoragePtr object_storage,
    StorageObjectStorage::ConfigurationPtr configuration,
    const std::optional<FormatSettings> & format_settings,
    std::string & sample_path,
    const ContextPtr & context);

void validateSupportedColumns(
    ColumnsDescription & columns,
    const StorageObjectStorage::Configuration & configuration);

bool isRelativePath(const std::string & path);

bool isSameStorageSchema(const std::string & schema1, const std::string & schema2);

// Returns lowercased and normalized schema / storage type (e.g. s3:// -> "s3")
std::string extractStorageType(const std::string & path);

std::string makeAbsolutePath(const std::string & table_location, const std::string & path);

// Resolve object storage for reading a file from
// * If path is relative -- it must be read from "base" storage
// * Otherwise, lookup if suitable storage already exists in secondary_storages
// * Also, TODO: come back here and make some comments
std::pair<DB::ObjectStoragePtr, std::string> resolveObjectStorageForPath(
    const std::string & table_location,
    const std::string & path,
    const DB::ObjectStoragePtr & base_storage,
    std::map<std::string, DB::ObjectStoragePtr> & secondary_storages,
    const DB::ContextPtr & context);

}
