#pragma once
#include <Storages/IStorage_fwd.h>
#include <Storages/VirtualColumnsDescription.h>

#include <memory>

namespace DB
{

class IStorage;
class ICompressionCodec;

using CompressionCodecPtr = std::shared_ptr<ICompressionCodec>;

struct StorageInMemoryMetadata;
using StorageMetadataPtr = std::shared_ptr<const StorageInMemoryMetadata>;

/// Snapshot of storage that fixes set columns that can be read in query.
/// There are 2 sources of columns: regular columns from metadata and virtual columns.
struct StorageSnapshot
{
    const IStorage & storage;
    const StorageMetadataPtr metadata;

    /// Owns `storage` when nothing else does. Empty for catalog-resident tables.
    /// `Alias` over `DataLakeCatalog` fills it: each lookup returns a new storage,
    /// and this snapshot is the only object that outlives that lookup.
    StoragePtr storage_holder;

    /// Additional data, on which set of columns may depend.
    /// E.g. data parts in MergeTree, list of blocks in Memory, etc.
    struct Data
    {
        virtual ~Data() = default;
    };

    using DataPtr = std::unique_ptr<Data>;
    DataPtr data;

    StorageSnapshot(
        const IStorage & storage_,
        StorageMetadataPtr metadata_);

    StorageSnapshot(
        const IStorage & storage_,
        StorageMetadataPtr metadata_,
        DataPtr data_);

    std::shared_ptr<StorageSnapshot> clone(DataPtr data_) const;

    /// Same `storage` and `storage_holder`, replaced metadata. Drops `data`.
    std::shared_ptr<StorageSnapshot> cloneWithMetadata(StorageMetadataPtr metadata_) const;

    /// Get columns description
    ColumnsDescription getAllColumnsDescription() const;

    /// Get all available columns with types according to options.
    NamesAndTypesList getColumns(const GetColumnsOptions & options) const;

    /// Get columns with types according to options only for requested names.
    NamesAndTypesList getColumnsByNames(const GetColumnsOptions & options, const Names & names) const;

    /// Get column with type according to options for requested name.
    std::optional<NameAndTypePair> tryGetColumn(const GetColumnsOptions & options, const String & column_name) const;
    NameAndTypePair getColumn(const GetColumnsOptions & options, const String & column_name) const;

    /// Block with ordinary + materialized + aliases + virtuals + subcolumns.
    Block getSampleBlockForColumns(const Names & column_names) const;

    ColumnsDescription getDescriptionForColumns(const Names & column_names) const;

    /// Verify that all the requested names are in the table and are set correctly:
    /// list of names is not empty and the names do not repeat.
    void check(const Names & column_names) const;

    /// Get default expression for a column.
    /// Takes into account physical and virtual columns.
    std::optional<ColumnDefault> getDefault(const String & column_name) const;
};

using StorageSnapshotPtr = std::shared_ptr<StorageSnapshot>;

}
