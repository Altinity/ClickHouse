#include <Interpreters/StorageID.h>
#include <Storages/MergeTree/IMergeTreeDataPart.h>

namespace DB
{

struct MergeTreeExportManifest
{
    using DataPartPtr = std::shared_ptr<const IMergeTreeDataPart>;

    StorageID destination_storage_id;
    DataPartPtr data_part;
    time_t create_time = time(nullptr);
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
