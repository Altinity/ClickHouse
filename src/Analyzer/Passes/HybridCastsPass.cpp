#include <Analyzer/Passes/HybridCastsPass.h>

#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryTreePassManager.h>
#include <Analyzer/Passes/QueryAnalysisPass.h>
#include <Analyzer/Utils.h>
#include <Analyzer/Resolve/IdentifierResolver.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/ColumnNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>

#include <Storages/IStorage.h>
#include <Storages/StorageDistributed.h>

#include <Core/Settings.h>
#include <Core/SettingsEnums.h>
#include <Common/Exception.h>

namespace DB
{

namespace Setting
{
    extern const SettingsBool hybrid_table_auto_cast_columns;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace
{

struct HybridCastTask
{
    QueryTreeNodePtr table_expression;
    ColumnsDescription cast_schema;
};

// Visitor replaces all usages of the column with CAST(column, type) in the query tree.
class HybridCastVisitor : public InDepthQueryTreeVisitor<HybridCastVisitor>
{
public:
    HybridCastVisitor(
        const std::unordered_map<const IQueryTreeNode *, ColumnsDescription> & cast_map_,
        ContextPtr context_)
        : cast_map(cast_map_)
        , context(std::move(context_))
    {}

    bool shouldTraverseTopToBottom() const { return false; }

    static bool needChildVisit(QueryTreeNodePtr &, QueryTreeNodePtr & child)
    {
        auto child_type = child->getNodeType();
        return !(child_type == QueryTreeNodeType::QUERY || child_type == QueryTreeNodeType::UNION);
    }

    void visitImpl(QueryTreeNodePtr & node)
    {
        auto * column_node = node->as<ColumnNode>();
        if (!column_node)
            return;

        auto column_source = column_node->getColumnSourceOrNull();
        if (!column_source)
            return;

        auto it = cast_map.find(column_source.get());
        if (it == cast_map.end())
            return;

        const auto & column_name = column_node->getColumnName();
        auto expected_column_opt = it->second.tryGetPhysical(column_name);
        if (!expected_column_opt)
            return;

        auto column_clone = std::static_pointer_cast<ColumnNode>(column_node->clone());
        column_clone->setColumnType(expected_column_opt->type);

        auto cast_node = buildCastFunction(column_clone, expected_column_opt->type, context);
        const auto & alias = node->getAlias();
        if (!alias.empty())
            cast_node->setAlias(alias);
        else
            cast_node->setAlias(expected_column_opt->name);

        node = cast_node;
    }

private:
    const std::unordered_map<const IQueryTreeNode *, ColumnsDescription> & cast_map;
    ContextPtr context;
};


} // namespace

void collectHybridTables(const QueryTreeNodePtr & join_tree, std::unordered_map<const IQueryTreeNode *, ColumnsDescription> & cast_map)
{
    if (!join_tree)
        return;
    if (const auto * table = join_tree->as<TableNode>())
    {
        const auto * storage = table->getStorage().get();
        if (const auto * distributed = typeid_cast<const StorageDistributed *>(storage))
        {
            ColumnsDescription to_cast = distributed->getColumnsToCast();
            if (!to_cast.empty())
                cast_map.emplace(join_tree.get(), std::move(to_cast)); // repeated table_expression can overwrite
        }
        return;
    }
    if (const auto * func = join_tree->as<FunctionNode>())
    {
        for (auto & child : func->getArguments().getNodes())
            collectHybridTables(child, cast_map);
        return;
    }
    if (const auto * query = join_tree->as<QueryNode>())
    {
        collectHybridTables(query->getJoinTree(), cast_map);
    }
    if (const auto * union_node = join_tree->as<UnionNode>())
    {
        for (auto & child : union_node->getQueries().getNodes())
            collectHybridTables(child, cast_map);
    }
}

void HybridCastsPass::run(QueryTreeNodePtr & query_tree_node, ContextPtr context)
{
    const auto & settings = context->getSettingsRef();
    if (!settings[Setting::hybrid_table_auto_cast_columns])
        return;

    auto * query = query_tree_node->as<QueryNode>();
    if (!query)
        return;

    std::unordered_map<const IQueryTreeNode *, ColumnsDescription> cast_map;
    collectHybridTables(query->getJoinTree(), cast_map);
    if (cast_map.empty())
        return;

    HybridCastVisitor visitor(cast_map, context);
    visitor.visit(query_tree_node);
}

}
