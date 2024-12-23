#include <Core/BaseSettings.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTSetQuery.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Storages/ObjectStorage/StorageObjectStorageSettings.h>
#include <Common/Exception.h>

namespace DB
{

IMPLEMENT_SETTINGS_TRAITS(StorageObjectStorageSettingsTraits, LIST_OF_STORAGE_OBJECT_STORAGE_SETTINGS)

void StorageObjectStorageSettings::loadFromQuery(ASTStorage & storage_def)
{
    if (storage_def.settings)
    {
        applyChanges(storage_def.settings->changes);
    }
}

}
