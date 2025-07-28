#pragma once

#include <Interpreters/ExpressionActions.h>
#include <Parsers/IAST_fwd.h>

#include <Processors/Chunk.h>

namespace DB
{

struct PartitionStrategy
{
    struct PartitionExpressionActionsAndColumnName
    {
        ExpressionActionsPtr actions;
        std::string column_name;
    };

    static std::unordered_map<std::string, bool> partition_strategy_to_wildcard_acceptance;

    PartitionStrategy(ASTPtr partition_by_, const Block & sample_block_, ContextPtr context_);

    virtual ~PartitionStrategy() = default;

    ColumnPtr computePartitionKey(const Chunk & chunk)
    {
        Block block_with_partition_by_expr = sample_block.cloneWithoutColumns();
        block_with_partition_by_expr.setColumns(chunk.getColumns());

        return computePartitionKey(block_with_partition_by_expr);
    }

    ColumnPtr computePartitionKey(Block & block)
    {
        actions_with_column_name.actions->execute(block);

        return block.getByName(actions_with_column_name.column_name).column;
    }

    virtual Chunk getFormatChunk(const Chunk & chunk) { return chunk.clone(); }

    virtual Block getFormatHeader() { return sample_block; }

    const NamesAndTypesList & getPartitionColumns() const;

protected:
    ASTPtr partition_by;
    Block sample_block;
    ContextPtr context;
    NamesAndTypesList partition_columns;
    PartitionExpressionActionsAndColumnName actions_with_column_name;
};

struct PartitionStrategyFactory
{
    static std::shared_ptr<PartitionStrategy> get(
        ASTPtr partition_by,
        const Block & sample_block,
        ContextPtr context,
        bool globbed_path,
        const std::string & partition_strategy,
        bool partition_columns_in_data_file);

    static std::shared_ptr<PartitionStrategy> get(
        ASTPtr partition_by,
        const NamesAndTypesList & partition_columns,
        ContextPtr context,
        bool globbed_path,
        const std::string & partition_strategy,
        bool partition_columns_in_data_file);
};

struct StringifiedPartitionStrategy : PartitionStrategy
{
    StringifiedPartitionStrategy(ASTPtr partition_by_, const Block & sample_block_, ContextPtr context_);
};

struct HiveStylePartitionStrategy : PartitionStrategy
{
    HiveStylePartitionStrategy(
        ASTPtr partition_by_,
        const Block & sample_block_,
        ContextPtr context_,
        bool partition_columns_in_data_file_);

    Chunk getFormatChunk(const Chunk & chunk) override;
    Block getFormatHeader() override;

private:
    bool partition_columns_in_data_file;
    std::unordered_set<std::string> partition_columns_name_set;
    Block block_without_partition_columns;
};

}
