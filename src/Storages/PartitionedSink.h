#pragma once

#include <Columns/IColumn.h>
#include <Common/HashTable/HashMap.h>
#include <Common/Arena.h>
#include <Common/PODArray.h>
#include <absl/container/flat_hash_map.h>
#include <Processors/Sinks/SinkToStorage.h>
#include <Interpreters/ExpressionAnalyzer.h>
#include <Interpreters/Context_fwd.h>
#include <Storages/IPartitionStrategy.h>


namespace DB
{

class PartitionedSink : public SinkToStorage
{
public:
    static constexpr auto PARTITION_ID_WILDCARD = "{_partition_id}";

<<<<<<< HEAD
    PartitionedSink(const ASTPtr & partition_by, ContextPtr context_, const Block & sample_block_);
=======
    PartitionedSink(
        std::shared_ptr<IPartitionStrategy> partition_strategy_,
        ContextPtr context_,
        SharedHeader source_header_);
>>>>>>> 790e1be4c1e (Add Hive-style S3 partitioned reads/writes)

    ~PartitionedSink() override;

    String getName() const override { return "PartitionedSink"; }

    void consume(Chunk & chunk) override;

    void onException(std::exception_ptr exception) override;

    void onFinish() override;

    virtual SinkPtr createSinkForPartition(const String & partition_id) = 0;

    static void validatePartitionKey(const String & str, bool allow_slash);

    static String replaceWildcards(const String & haystack, const String & partition_id);

protected:
    std::shared_ptr<IPartitionStrategy> partition_strategy;

private:
    ContextPtr context;
<<<<<<< HEAD
    Block sample_block;

    ExpressionActionsPtr partition_by_expr;
    String partition_by_column_name;
=======
    SharedHeader source_header;
>>>>>>> 790e1be4c1e (Add Hive-style S3 partitioned reads/writes)

    absl::flat_hash_map<StringRef, SinkPtr> partition_id_to_sink;
    HashMapWithSavedHash<StringRef, size_t> partition_id_to_chunk_index;
    IColumn::Selector chunk_row_index_to_partition_index;
    Arena partition_keys_arena;

    SinkPtr getSinkForPartitionKey(StringRef partition_key);
};

}
