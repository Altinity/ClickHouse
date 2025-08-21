#pragma once

#include <Disks/IDisk.h>
#include <Interpreters/StorageID.h>
#include <Core/QualifiedTableName.h>
#include "IO/WriteBufferFromFileBase.h"
#include "IO/ReadBuffer.h"
#include "IO/WriteHelpers.h"
#include "IO/ReadHelpers.h"
#include "Common/Exception.h"
#include <fmt/format.h>

namespace DB
{

struct MergeTreeExportPartEntry
{
    String part_name;
    String commit_id;
    std::filesystem::path file_path;
    DiskPtr disk;
    StorageID destination_storage_id;  // Added StorageID field

    MergeTreeExportPartEntry(const String & part_name_, const String & commit_id_, DiskPtr disk_, 
                            const String & path_prefix, const StorageID & destination_storage_id_)
        : part_name(part_name_)
        , commit_id(commit_id_)
        , file_path(std::filesystem::path(path_prefix)  / ("export_part_" + part_name + "_" + commit_id))
        , disk(std::move(disk_))
        , destination_storage_id(destination_storage_id_)
    {}

    MergeTreeExportPartEntry(DiskPtr disk_, const String & file_path_)
    : file_path(file_path_)
    , disk(std::move(disk_))
    , destination_storage_id(StorageID::createEmpty())  // Initialize with empty StorageID
    {
        constexpr std::string_view prefix = "export_part_";

        const auto & filename = file_path.filename().string();

        if (!filename.starts_with(prefix))
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Export part entry file path does not start with 'export_part_': {}", file_path_);

        auto rest = filename.substr(prefix.size());

        auto pos = rest.rfind('_');
        if (pos == std::string::npos)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Export part entry file name does not contain commit id: {}", filename);

        part_name = rest.substr(0, pos);
        commit_id = rest.substr(pos + 1);
        
        // Parse StorageID from the file content
        destination_storage_id = parseStorageIDFromFile();
    }

    void write() const
    {
        try
        {
            auto out = disk->writeFile(file_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite);
            
            // Serialize the QualifiedTableName (database.table format)
            String serialized_qualified_name = destination_storage_id.getQualifiedName().getFullName();
            writeString(serialized_qualified_name, *out);
            
            out->finalize();
            out->sync();
        }
        catch (const Exception &)
        {
            remove();
            throw;
        }
    }

    void remove() const
    {
        disk->removeFileIfExists(file_path);
    }

private:
    StorageID parseStorageIDFromFile() const
    {
        try
        {
            auto in = disk->readFile(file_path, ReadSettings{});
            
            String serialized_qualified_name;
            readString(serialized_qualified_name, *in);
            
            return StorageID(QualifiedTableName::parseFromString(serialized_qualified_name));
        }
        catch (const Exception &)
        {
            // If file doesn't exist or is corrupted, return empty StorageID
            // This handles backward compatibility with existing files
            return StorageID::createEmpty();
        }
    }
};

}
