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
    struct SinkCreator
    {
        virtual ~SinkCreator() = default;
        virtual SinkPtr createSinkForPartition(const String & partition_id) = 0;
    };

    static constexpr auto PARTITION_ID_WILDCARD = "{_partition_id}";

    PartitionedSink(
        std::shared_ptr<IPartitionStrategy> partition_strategy_,
        std::shared_ptr<SinkCreator> sink_creator_,
        ContextPtr context_,
        const Block & source_header_
        );

    struct ChunkSplitStatistics
    {
        uint64_t time_spent_on_partition_calculation = 0;
        uint64_t time_spent_on_chunk_split = 0;
    };

    ~PartitionedSink() override;

    String getName() const override { return "PartitionedSink"; }

    void consume(Chunk & chunk) override;

    void onException(std::exception_ptr exception) override;

    void onFinish() override;

    void assumeSamePartition() {
        assume_same_partition = true;
    }

    static void validatePartitionKey(const String & str, bool allow_slash);

    static String replaceWildcards(const String & haystack, const String & partition_id);

    ChunkSplitStatistics getPartitioningStats() const;

private:
    std::shared_ptr<IPartitionStrategy> partition_strategy;
    std::shared_ptr<SinkCreator> sink_creator;
    ContextPtr context;
    Block source_header;

    absl::flat_hash_map<StringRef, SinkPtr> partition_id_to_sink;
    HashMapWithSavedHash<StringRef, size_t> partition_id_to_chunk_index;
    IColumn::Selector chunk_row_index_to_partition_index;
    Arena partition_keys_arena;

    SinkPtr getSinkForPartitionKey(StringRef partition_key);
    ChunkSplitStatistics partitioning_stats;
    bool assume_same_partition = false;
    std::shared_ptr<SinkToStorage> sink_to_storage;

    void consumeAssumeSamePartition(Chunk & chunk);
};

}
