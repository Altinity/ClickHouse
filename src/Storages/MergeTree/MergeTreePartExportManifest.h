#pragma once

#include <Interpreters/StorageID.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>
#include <Storages/StorageSnapshot.h>
#include <QueryPipeline/QueryPipeline.h>
#include <optional>

namespace DB
{

class Exception;

class ExportPartTask;

struct MergeTreePartExportManifest
{
    using FileAlreadyExistsPolicy = MergeTreePartExportFileAlreadyExistsPolicy;

    using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;

    struct CompletionCallbackResult
    {
    private:
        CompletionCallbackResult(bool success_, const String & relative_path_in_destination_storage_, std::optional<Exception> exception_)
            : success(success_), relative_path_in_destination_storage(relative_path_in_destination_storage_), exception(std::move(exception_)) {}
    public:

        static CompletionCallbackResult createSuccess(const String & relative_path_in_destination_storage_)
        {
            return CompletionCallbackResult(true, relative_path_in_destination_storage_, std::nullopt);
        }

        static CompletionCallbackResult createFailure(Exception exception_)
        {
            return CompletionCallbackResult(false, "", std::move(exception_));
        }

        bool success = false;
        String relative_path_in_destination_storage;
        std::optional<Exception> exception;
    };

    MergeTreePartExportManifest(
        const StorageID & destination_storage_id_,
        const DataPartPtr & data_part_,
        const String & transaction_id_,
        FileAlreadyExistsPolicy file_already_exists_policy_,
        const FormatSettings & format_settings_,
        const StorageSnapshotPtr & storage_snapshot_,
        std::function<void(CompletionCallbackResult)> completion_callback_ = {})
        : destination_storage_id(destination_storage_id_),
          data_part(data_part_),
          transaction_id(transaction_id_),
          file_already_exists_policy(file_already_exists_policy_),
          format_settings(format_settings_),
          storage_snapshot(storage_snapshot_),
          completion_callback(completion_callback_),
          create_time(time(nullptr)) {}

    StorageID destination_storage_id;
    DataPartPtr data_part;
    /// Used for killing the export.
    String transaction_id;
    FileAlreadyExistsPolicy file_already_exists_policy;
    FormatSettings format_settings;

    /// Storage snapshot captured at the time of query validation to prevent race conditions with mutations
    /// Otherwise the export could fail if the schema changes between validation and execution
    StorageSnapshotPtr storage_snapshot;

    std::function<void(CompletionCallbackResult)> completion_callback;

    time_t create_time;
    mutable bool in_progress = false;
    mutable std::shared_ptr<ExportPartTask> task = nullptr;

    bool operator<(const MergeTreePartExportManifest & rhs) const 
    {
        // Lexicographic comparison: first compare destination storage, then part name
        auto lhs_storage = destination_storage_id.getQualifiedName();
        auto rhs_storage = rhs.destination_storage_id.getQualifiedName();
        
        if (lhs_storage != rhs_storage)
            return lhs_storage < rhs_storage;
            
        return data_part->name < rhs.data_part->name;
    }

    bool operator==(const MergeTreePartExportManifest & rhs) const 
    {
        return destination_storage_id.getQualifiedName() == rhs.destination_storage_id.getQualifiedName()
            && data_part->name == rhs.data_part->name;
    }
};

}
