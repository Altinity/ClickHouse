#pragma once

#include <Core/BaseSettings.h>
#include <Core/Settings.h>
#include <Core/SettingsEnums.h>


namespace DB
{
class ASTStorage;

#define STORAGE_OBJECT_STORAGE_RELATED_SETTINGS(M, ALIAS) \
    M( \
        Bool, \
        allow_dynamic_metadata_for_data_lakes, \
        false, \
        "If enabled, indicates that metadata is taken from iceberg specification that is pulled from cloud before each query.", \
        0)

#define LIST_OF_STORAGE_OBJECT_STORAGE_SETTINGS(M, ALIAS) \
    STORAGE_OBJECT_STORAGE_RELATED_SETTINGS(M, ALIAS) \
    LIST_OF_ALL_FORMAT_SETTINGS(M, ALIAS)

DECLARE_SETTINGS_TRAITS(StorageObjectStorageSettingsTraits, LIST_OF_STORAGE_OBJECT_STORAGE_SETTINGS)

struct StorageObjectStorageSettings : public BaseSettings<StorageObjectStorageSettingsTraits>
{
    void loadFromQuery(ASTStorage & storage_def);
};

}
