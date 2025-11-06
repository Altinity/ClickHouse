#include <Interpreters/StorageID.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>

namespace DB
{

struct MergeTreeExportManifest
{
    using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;


    MergeTreeExportManifest(
        const StorageID & destination_storage_id_,
        const DataPartPtr & data_part_,
        bool overwrite_file_if_exists_,
        bool parallel_formatting_,
        bool parallel_formatting_parquet_,
        std::size_t max_threads_)
        : destination_storage_id(destination_storage_id_),
          data_part(data_part_),
          overwrite_file_if_exists(overwrite_file_if_exists_),
          parallel_formatting(parallel_formatting_),
          parquet_parallel_encoding(parallel_formatting_parquet_),
          max_threads(max_threads_),
          create_time(time(nullptr)) {}

    StorageID destination_storage_id;
    DataPartPtr data_part;
    bool overwrite_file_if_exists;
    bool parallel_formatting;
    /// parquet has a different setting for parallel formatting
    bool parquet_parallel_encoding;
    std::size_t max_threads;

    time_t create_time;
    mutable bool in_progress = false;

    bool operator<(const MergeTreeExportManifest & rhs) const 
    {
        // Lexicographic comparison: first compare destination storage, then part name
        auto lhs_storage = destination_storage_id.getQualifiedName();
        auto rhs_storage = rhs.destination_storage_id.getQualifiedName();
        
        if (lhs_storage != rhs_storage)
            return lhs_storage < rhs_storage;
            
        return data_part->name < rhs.data_part->name;
    }

    bool operator==(const MergeTreeExportManifest & rhs) const 
    {
        return destination_storage_id.getQualifiedName() == rhs.destination_storage_id.getQualifiedName()
            && data_part->name == rhs.data_part->name;
    }
};

}
