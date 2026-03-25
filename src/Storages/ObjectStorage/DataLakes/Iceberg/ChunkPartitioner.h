#pragma once

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <Core/Range.h>
#include <Processors/Chunk.h>
#include <Storages/ObjectStorage/StorageObjectStorage.h>
#include <Functions/IFunction.h>

namespace DB
{

#if USE_AVRO

class ChunkPartitioner
{
public:
    explicit ChunkPartitioner(
        Poco::JSON::Array::Ptr partition_specification, Poco::JSON::Object::Ptr schema, ContextPtr context, SharedHeader sample_block_);

    using PartitionKey = Row;
    struct PartitionKeyHasher
    {
        size_t operator()(const PartitionKey & key) const;

        mutable std::hash<String> hasher;
    };

    std::vector<std::pair<PartitionKey, Chunk>> partitionChunk(const Chunk & chunk);

    /// Compute the partition key for a single representative row.
    /// @param source_block  A 1-row Block containing (at least) the partition source columns.
    ///                      Column names must match those referenced by the partition spec.
    Row computePartitionKey(const Block & source_block) const;

    const std::vector<String> & getColumns() const { return columns_to_apply; }

    const std::vector<DataTypePtr> & getResultTypes() const { return result_data_types; }

private:
    SharedHeader sample_block;

    std::vector<FunctionOverloadResolverPtr> functions;
    std::vector<std::optional<size_t>> function_params;
    std::vector<std::optional<String>> function_time_zones;
    std::vector<String> columns_to_apply;
    std::vector<DataTypePtr> result_data_types;

    size_t max_partitions_count;
};

#endif

}
