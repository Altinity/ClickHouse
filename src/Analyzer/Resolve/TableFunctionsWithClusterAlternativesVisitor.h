#pragma once

#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/TableFunctionNode.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <TableFunctions/TableFunctionFile.h>

namespace DB
{

class TableFunctionsWithClusterAlternativesVisitor : public InDepthQueryTreeVisitor<TableFunctionsWithClusterAlternativesVisitor, /*const_visitor=*/true>
{
public:
    void visitImpl(const QueryTreeNodePtr & node)
    {
        if (node->getNodeType() == QueryTreeNodeType::TABLE_FUNCTION)
            ++table_function_count;
        else if (node->getNodeType() == QueryTreeNodeType::TABLE)
            ++table_count;
        else if (node->getNodeType() == QueryTreeNodeType::QUERY && node->as<QueryNode>()->isSubquery())
            ++subquery_count;
        else if (node->getNodeType() == QueryTreeNodeType::JOIN)
            has_join = true;
    }

    bool needChildVisit(const QueryTreeNodePtr &, const QueryTreeNodePtr &) { return true; }

    bool shouldReplaceWithClusterAlternatives() const
    {
        return !has_join && shouldReplaceWithClusterAlternativesIgnoringJoin();
    }

    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp): same restrictions
    /// as shouldReplaceWithClusterAlternatives() except the has_join prohibition, for the narrow opt-in case
    /// of a direct DataLake-catalog JOIN where every table expression is resolvable on all cluster nodes.
    bool shouldReplaceWithClusterAlternativesIgnoringJoin() const
    {
        return subquery_count <= 1 && ((table_count + table_function_count) == 1 || (table_function_count == 0));
    }

    size_t getTableCount() const { return table_count; }
    size_t getTableFunctionCount() const { return table_function_count; }
    size_t getSubqueryCount() const { return subquery_count; }
    bool hasJoin() const { return has_join; }

private:
    size_t table_count = 0;
    size_t table_function_count = 0;
    // Number of subqueries that appear
    size_t subquery_count = 0;

    bool has_join = false;
};

}
