#include <Analyzer/ArrayJoinNode.h>
#include <Analyzer/FunctionNode.h>
#include <Analyzer/InDepthQueryTreeVisitor.h>
#include <Analyzer/JoinNode.h>
#include <Analyzer/Passes/QueryAnalysisPass.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Analyzer/UnionNode.h>
#include <Common/logger_useful.h>
#include <Core/Settings.h>
#include <Interpreters/ClusterProxy/SelectStreamFactory.h>
#include <Interpreters/ClusterProxy/executeQuery.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Planner/PlannerJoinTree.h>
#include <Planner/Utils.h>
#include <Planner/findQueryForParallelReplicas.h>
#include <Processors/QueryPlan/CreatingSetsStep.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/JoinStep.h>
#include <Processors/QueryPlan/JoinStepLogical.h>
#include <Processors/QueryPlan/SortingStep.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorageCluster.h>
#include <Storages/MergeTree/MergeTreeData.h>
#include <Storages/ObjectStorage/StorageObjectStorageCluster.h>
#include <Storages/SelectQueryInfo.h>
#include <Storages/StorageDummy.h>
#include <Storages/StorageMaterializedView.h>
#include <Storages/StorageView.h>
#include <Storages/buildQueryTreeForShard.h>
#include <Storages/removeGroupingFunctionSpecializations.h>
#include <stack>

namespace DB
{
namespace Setting
{
    extern const SettingsBool parallel_replicas_allow_in_with_subquery;
    extern const SettingsBool parallel_replicas_for_non_replicated_merge_tree;
    extern const SettingsBool parallel_replicas_allow_materialized_views;
    extern const SettingsBool serialize_query_plan;
    extern const SettingsBool parallel_replicas_allow_view_over_mergetree;
    extern const SettingsBool object_storage_cluster_bypass_join_wrap;
    extern const SettingsString cluster_for_parallel_replicas;
}

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int UNSUPPORTED_METHOD;
}

bool isTableNodeEligibleForParallelReplicas(const TableNode & table_node, const StoragePtr & storage, const ContextPtr & context)
{
    const auto & settings = context->getSettingsRef();

    if (!storage->isMergeTree() && !typeid_cast<const StorageDummy *>(storage.get()))
        return false;

    if (!storage->supportsReplication() && !settings[Setting::parallel_replicas_for_non_replicated_merge_tree])
        return false;

    /// Parallel replicas not supported with FINAL.
    if (table_node.hasTableExpressionModifiers() && table_node.getTableExpressionModifiers()->hasFinal())
        return false;

    return true;
}

static bool canUseTableForParallelReplicas(const TableNode & table_node, const ContextPtr & context)
{
    const auto & settings = context->getSettingsRef();
    auto storage = table_node.getStorage();

    if (settings[Setting::parallel_replicas_allow_view_over_mergetree])
    {
        const auto * view = typeid_cast<const StorageView *>(storage.get());
        if (view)
        {
            auto underlying_storage = view->getUnderlyingMergeTreeStorageForParallelReplicas(context);
            if (!underlying_storage)
                return false;

            return true;
        }
    }

    const auto * mv = typeid_cast<const StorageMaterializedView *>(storage.get());
    if (mv)
    {
        if (!settings[Setting::parallel_replicas_allow_materialized_views])
            return false;

        /// Address refreshable MVs separately, currently leads to logical error.
        if (mv->isRefreshable())
            return false;

        storage = mv->getTargetTable();
    }

    return isTableNodeEligibleForParallelReplicas(table_node, storage, context);
}

/// Returns a list of (sub)queries (candidates) which may support parallel replicas.
/// The rule is :
/// subquery has only LEFT / RIGHT / ALL INNER JOIN (or none), and left / right part is MergeTree table or subquery candidate as well.
///
/// Additional checks are required, so we return many candidates. The innermost subquery is on top.
static std::vector<const QueryNode *> getSupportingParallelReplicasQueries(const IQueryTreeNode * query_tree_node, const ContextPtr & context)
{
    std::vector<const QueryNode *> res;

    while (query_tree_node)
    {
        auto join_tree_node_type = query_tree_node->getNodeType();

        switch (join_tree_node_type)
        {
            case QueryTreeNodeType::TABLE:
            {
                const auto & table_node = query_tree_node->as<TableNode &>();
                if (canUseTableForParallelReplicas(table_node, context))
                    return res;

                return {};
            }
            case QueryTreeNodeType::TABLE_FUNCTION:
            {
                return {};
            }
            case QueryTreeNodeType::QUERY:
            {
                const auto & query_node_to_process = query_tree_node->as<QueryNode &>();
                query_tree_node = query_node_to_process.getJoinTree().get();
                res.push_back(&query_node_to_process);
                break;
            }
            case QueryTreeNodeType::UNION:
            {
                const auto & union_node = query_tree_node->as<UnionNode &>();
                const auto & union_queries = union_node.getQueries().getNodes();

                if (union_queries.empty())
                    return {};

                query_tree_node = union_queries.front().get();
                break;
            }
            case QueryTreeNodeType::ARRAY_JOIN:
            {
                const auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
                query_tree_node = array_join_node.getTableExpression().get();
                break;
            }
            case QueryTreeNodeType::CROSS_JOIN:
            {
                /// TODO: We can parallelize one table
                return {};
            }
            case QueryTreeNodeType::JOIN:
            {
                const auto & join_node = query_tree_node->as<JoinNode &>();
                const auto join_kind = join_node.getKind();
                const auto join_strictness = join_node.getStrictness();

                /// Do not apply for non-leftmost RIGHT JOIN
                std::unordered_set<QueryTreeNodeType> supported_table_expression_types = {QueryTreeNodeType::TABLE, QueryTreeNodeType::QUERY, QueryTreeNodeType::UNION};

                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_strictness == JoinStrictness::All))
                    query_tree_node = join_node.getLeftTableExpression().get();
                else if (join_kind == JoinKind::Right && join_strictness != JoinStrictness::RightAny
                    && supported_table_expression_types.contains(join_node.getLeftTableExpression()->getNodeType()))
                    query_tree_node = join_node.getRightTableExpression().get();
                else
                    return {};

                break;
            }
            default:
            {
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Unexpected node type for table expression. "
                                "Expected table, table function, query, union, join or array join. Actual {}",
                                query_tree_node->getNodeTypeName());
            }
        }
    }

    return res;
}

class ReplaceTableNodeToDummyVisitor : public InDepthQueryTreeVisitorWithContext<ReplaceTableNodeToDummyVisitor>
{
public:
    using Base = InDepthQueryTreeVisitorWithContext<ReplaceTableNodeToDummyVisitor>;
    using Base::Base;

    void enterImpl(QueryTreeNodePtr & node)
    {
        auto * table_node = node->as<TableNode>();
        auto * table_function_node = node->as<TableFunctionNode>();

        if (table_node || table_function_node)
        {
            const auto & storage_snapshot = table_node ? table_node->getStorageSnapshot() : table_function_node->getStorageSnapshot();
            const auto & storage = storage_snapshot->storage;

            auto storage_dummy = std::make_shared<StorageDummy>(
                storage.getStorageID(),
                /// To preserve information about alias columns, column description must be extracted directly from storage metadata.
                storage_snapshot->metadata->getColumns(),
                storage_snapshot,
                storage.supportsReplication());

            auto dummy_table_node = std::make_shared<TableNode>(std::move(storage_dummy), getContext());
            if (table_node && table_node->hasTableExpressionModifiers())
                dummy_table_node->getTableExpressionModifiers() = table_node->getTableExpressionModifiers();

            dummy_table_node->setAlias(node->getAlias());
            replacement_map.emplace(node.get(), std::move(dummy_table_node));
        }
    }

    std::unordered_map<const IQueryTreeNode *, QueryTreeNodePtr> replacement_map;
};

static QueryTreeNodePtr replaceTablesWithDummyTables(QueryTreeNodePtr query, const ContextPtr & context)
{
    ReplaceTableNodeToDummyVisitor visitor(context);
    visitor.visit(query);

    return query->cloneAndReplace(visitor.replacement_map);
}

#ifdef DUMP_PARALLEL_REPLICAS_QUERY_CANDIDATES
#include <ranges>

static void dumpStack(const std::vector<const QueryNode *> & stack)
{
    std::ranges::reverse_view rv{stack};
    for (const auto * node : rv)
        LOG_DEBUG(getLogger(__PRETTY_FUNCTION__), "{}\n{}", CityHash_v1_0_2::Hash128to64(node->getTreeHash()), node->dumpTree());
}
#endif

/// Find the best candidate for parallel replicas execution by verifying query plan.
/// If query plan has only Expression, Filter or Join steps, we can execute it fully remotely and check the next query.
/// Otherwise we can execute current query up to WithMergableStage only.
static const QueryNode * findQueryForParallelReplicas(
    std::vector<const QueryNode *> stack,
    const std::unordered_map<const QueryNode *, const QueryPlan::Node *> & mapping,
    const Settings & settings)
{
#ifdef DUMP_PARALLEL_REPLICAS_QUERY_CANDIDATES
    dumpStack(stack);
#endif

    struct Frame
    {
        const QueryPlan::Node * node = nullptr;
        /// Below we will check subqueries from `stack` to find outermost subquery that could be executed remotely.
        /// Currently traversal algorithm considers only steps with 0 or 1 children and JOIN specifically.
        /// When we found some step that requires finalization on the initiator (e.g. GROUP BY) there are two options:
        /// 1. If plan looks like a single path (e.g. AggregatingStep -> ExpressionStep -> Reading) we can execute
        /// current subquery as a whole with replicas.
        /// 2. If we were inside JOIN we cannot offload the whole subquery to replicas because at least one side
        /// of the JOIN needs to be finalized on the initiator.
        /// So this flag is used to track what subquery to return once we hit a step that needs finalization.
        bool inside_join = false;
    };

    const QueryNode * res = nullptr;

    while (!stack.empty())
    {
        const QueryNode * const subquery_node = stack.back();
        stack.pop_back();

        auto it = mapping.find(subquery_node);
        /// This should not happen ideally.
        if (it == mapping.end())
            break;

        std::stack<Frame> nodes_to_check;
        nodes_to_check.push({.node = it->second, .inside_join = false});
        bool can_distribute_full_node = true;
        bool currently_inside_join = false;

        while (!nodes_to_check.empty())
        {
            /// Copy to avoid container overflow (we call pop() in the next line).
            const auto [next_node_to_check, inside_join] = nodes_to_check.top();
            nodes_to_check.pop();
            const auto & children = next_node_to_check->children;
            auto * step = next_node_to_check->step.get();

            if (children.empty())
            {
                /// Found a source step.
            }
            else if (children.size() == 1)
            {
                const auto * expression = typeid_cast<ExpressionStep *>(step);
                const auto * filter = typeid_cast<FilterStep *>(step);

                const auto * creating_sets = typeid_cast<DelayedCreatingSetsStep *>(step);
                const bool allowed_creating_sets = settings[Setting::parallel_replicas_allow_in_with_subquery] && creating_sets;

                const auto * sorting = typeid_cast<SortingStep *>(step);
                /// Sorting for merge join is supposed to be done locally before join itself, so it doesn't need finalization.
                const bool allowed_sorting = sorting && sorting->isSortingForMergeJoin();

                if (!expression && !filter && !allowed_creating_sets && !allowed_sorting)
                {
                    can_distribute_full_node = false;
                    currently_inside_join = inside_join;
                }

                nodes_to_check.push({.node = children.front(), .inside_join = inside_join});
            }
            else
            {
                const auto * join = typeid_cast<JoinStep *>(step);
                const auto * join_logical = typeid_cast<JoinStepLogical *>(step);
                if (join_logical && typeid_cast<JoinStepLogicalLookup *>(children.back()->step.get()))
                    /// JoinStepLogical with prepared storage is converted to FilledJoinStep, not regular JoinStep.
                    join_logical = nullptr;

                /// We've checked that JOIN is INNER/LEFT/RIGHT on query tree level before.
                /// Don't distribute UNION node.
                if (!join && !join_logical)
                    return res;

                for (const auto & child : children)
                    nodes_to_check.push({.node = child, .inside_join = true});
            }
        }

        if (!can_distribute_full_node)
        {
            /// Current query node does not contain subqueries.
            /// We can execute parallel replicas over storage::read.
            if (!res)
                return nullptr;

            return currently_inside_join ? res : subquery_node;
        }

        /// Query is simple enough to be fully distributed.
        res = subquery_node;
    }

    return res;
}

const QueryNode * findQueryForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options)
{
    if (select_query_options.only_analyze)
        return nullptr;

    auto * query_node = query_tree_node->as<QueryNode>();
    auto * union_node = query_tree_node->as<UnionNode>();

    if (!query_node && !union_node)
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
            "Expected QUERY or UNION node. Actual {}",
            query_tree_node->formatASTForErrorMessage());

    auto context = query_node ? query_node->getContext() : union_node->getContext();

    if (!context->canUseParallelReplicasOnInitiator())
        return nullptr;

    auto stack = getSupportingParallelReplicasQueries(query_tree_node.get(), context);
    /// Empty stack means that storage does not support parallel replicas.
    if (stack.empty())
        return nullptr;

    /// We don't have any subquery and storage can process parallel replicas by itself.
    if (stack.back() == query_tree_node.get())
        return nullptr;

    /// This is needed to avoid infinite recursion.
    auto mutable_context = Context::createCopy(context);
    mutable_context->setSetting("allow_experimental_parallel_reading_from_replicas", Field(0));

    /// Here we replace tables to dummy, in order to build a temporary query plan for parallel replicas analysis.
    ResultReplacementMap replacement_map;
    auto updated_query_tree = replaceTablesWithDummyTables(query_tree_node, mutable_context);

    SelectQueryOptions options;
    Planner planner(updated_query_tree, options, std::make_shared<GlobalPlannerContext>(nullptr, nullptr, nullptr, FiltersForTableExpressionMap{}));
    planner.buildQueryPlanIfNeeded();

    /// This part is a bit clumsy.
    /// We updated a query_tree with dummy storages, and mapping is using updated_query_tree now.
    /// But QueryNode result should be taken from initial query tree.
    /// So that we build a list of candidates again, and call findQueryForParallelReplicas for it.
    auto new_stack = getSupportingParallelReplicasQueries(updated_query_tree.get(), context);
    const auto & mapping = planner.getQueryNodeToPlanStepMapping();
    const auto * res = findQueryForParallelReplicas(new_stack, mapping, context->getSettingsRef());

    if (res)
    {
        // find query in initial stack
        while (!new_stack.empty())
        {
            if (res == new_stack.back())
            {
                res = stack.back();
                break;
            }

            stack.pop_back();
            new_stack.pop_back();
        }
    }
    return res;
}

static const TableNode * findTableForParallelReplicas(const IQueryTreeNode * query_tree_node, const ContextPtr & context)
{
    std::stack<const IQueryTreeNode *> join_nodes;
    while (query_tree_node || !join_nodes.empty())
    {
        if (!query_tree_node)
        {
            query_tree_node = join_nodes.top();
            join_nodes.pop();
        }

        auto join_tree_node_type = query_tree_node->getNodeType();

        switch (join_tree_node_type)
        {
            case QueryTreeNodeType::TABLE:
            {
                const auto & table_node = query_tree_node->as<TableNode &>();
                if (canUseTableForParallelReplicas(table_node, context))
                    return &table_node;

                query_tree_node = nullptr;
                break;
            }
            case QueryTreeNodeType::TABLE_FUNCTION:
            {
                query_tree_node = nullptr;
                break;
            }
            case QueryTreeNodeType::QUERY:
            {
                const auto & query_node_to_process = query_tree_node->as<QueryNode &>();
                query_tree_node = query_node_to_process.getJoinTree().get();
                break;
            }
            case QueryTreeNodeType::UNION:
            {
                const auto & union_node = query_tree_node->as<UnionNode &>();
                const auto & union_queries = union_node.getQueries().getNodes();

                query_tree_node = nullptr;
                if (!union_queries.empty())
                    query_tree_node = union_queries.front().get();

                break;
            }
            case QueryTreeNodeType::ARRAY_JOIN:
            {
                const auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
                query_tree_node = array_join_node.getTableExpression().get();
                break;
            }
            case QueryTreeNodeType::CROSS_JOIN:
            {
                /// TODO: We can parallelize one table
                return nullptr;
            }
            case QueryTreeNodeType::JOIN:
            {
                const auto & join_node = query_tree_node->as<JoinNode &>();
                const auto join_kind = join_node.getKind();
                const auto join_strictness = join_node.getStrictness();

                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_strictness == JoinStrictness::All))
                {
                    query_tree_node = join_node.getLeftTableExpression().get();
                    join_nodes.push(join_node.getRightTableExpression().get());
                }
                else if (join_kind == JoinKind::Right)
                {
                    query_tree_node = join_node.getRightTableExpression().get();
                    join_nodes.push(join_node.getLeftTableExpression().get());
                }
                else
                {
                    return nullptr;
                }
                break;
            }
            default:
            {
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                                "Unexpected node type for table expression. "
                                "Expected table, table function, query, union, join or array join. Actual {}",
                                query_tree_node->getNodeTypeName());
            }
        }
    }

    return nullptr;
}

const TableNode * findTableForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options)
{
    if (select_query_options.only_analyze)
        return nullptr;

    auto * query_node = query_tree_node->as<QueryNode>();
    auto * union_node = query_tree_node->as<UnionNode>();

    if (!query_node && !union_node)
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
            "Expected QUERY or UNION node. Actual {}",
            query_tree_node->formatASTForErrorMessage());

    auto context = query_node ? query_node->getContext() : union_node->getContext();

    if (!context->getSettingsRef()[Setting::serialize_query_plan] && !context->canUseParallelReplicasOnFollower())
        return nullptr;

    return findTableForParallelReplicas(query_tree_node.get(), context);
}

namespace
{

/// DIAGNOSTIC ONLY (see object_storage_cluster_bypass_join_wrap in PlannerJoinTree.cpp / IStorageCluster.cpp):
/// self-contained copies of getSupportingParallelReplicasQueries()/findTableForParallelReplicas() above, with
/// eligibility relaxed from "MergeTree" to "StorageObjectStorageCluster" (Iceberg/DataLake specifically, not
/// every IStorageCluster). Used purely to observe -- via logging -- whether the existing whole-query/leftmost-
/// table candidate-finding traversal (which already sees through CTEs/subqueries, unlike PlannerJoinTree's
/// buildJoinTreeQueryPlan) would identify a multi-CTE query like IcebergBench's q21 as a distributable
/// candidate and its leftmost table as the driver, the way it already does for MergeTree. Deliberately NOT
/// wired into GlobalPlannerContext / real execution -- buildQueryPlanForParallelReplicas() below is
/// MergeTree/task-based-parallel-replicas machinery (ClusterProxy::executeQueryWithParallelReplicas) and would
/// not be correct for an IStorageCluster driver.
///
/// Narrowed from "any IStorageCluster" to "StorageObjectStorageCluster" specifically: the real dispatch
/// (buildQueryPlanForObjectStorageCluster) only knows how to drive a StorageObjectStorageCluster (it calls
/// StorageObjectStorageCluster::buildClusterTableFunctionAST(), which no other IStorageCluster subtype has).
/// A driver of some other IStorageCluster type (e.g. a plain s3Cluster/hdfsCluster table nested under a CTE)
/// must not be picked here -- it should stay on its existing (unsupported-by-this-prototype) path rather than
/// be selected and then hit the LOGICAL_ERROR throw in buildQueryPlanForObjectStorageCluster.
bool isObjectStorageClusterTable(const IQueryTreeNode & table_node_untyped)
{
    const auto & table_node = table_node_untyped.as<const TableNode &>();
    return dynamic_cast<const StorageObjectStorageCluster *>(table_node.getStorage().get()) != nullptr;
}

/// saw_join, when non-null, is set to true iff the walk passes through an actual JOIN node (not
/// ARRAY_JOIN/CROSS_JOIN -- CROSS_JOIN is rejected outright above, and ARRAY_JOIN doesn't introduce a second,
/// independently-planned source the way a two-sided JOIN does). Used by findObjectStorageClusterWholeQueryDriver()
/// to reject a candidate that is merely a chain of derived-table subqueries wrapping one table with no JOIN
/// anywhere -- see its own comment for why that shape must never be treated as a whole-query JOIN-bypass
/// candidate (confirmed live: IcebergBench q2's `appinfo_d` CTE body -- SELECT * FROM (SELECT ... FROM appinfo
/// WHERE ...) t WHERE rn = 1 -- has no JOIN at all, yet this traversal "succeeds" down to `appinfo` as if it
/// were one, since it walks straight through QUERY nodes to the driver TABLE regardless of whether a JOIN was
/// ever involved).
std::vector<const QueryNode *> getSupportingObjectStorageClusterQueriesForDiagnostic(const IQueryTreeNode * query_tree_node, bool * saw_join = nullptr)
{
    std::vector<const QueryNode *> res;

    while (query_tree_node)
    {
        switch (query_tree_node->getNodeType())
        {
            case QueryTreeNodeType::TABLE:
            {
                if (isObjectStorageClusterTable(*query_tree_node))
                    return res;
                return {};
            }
            case QueryTreeNodeType::TABLE_FUNCTION:
            {
                return {};
            }
            case QueryTreeNodeType::QUERY:
            {
                const auto & query_node_to_process = query_tree_node->as<QueryNode &>();
                query_tree_node = query_node_to_process.getJoinTree().get();
                res.push_back(&query_node_to_process);
                break;
            }
            case QueryTreeNodeType::UNION:
            {
                const auto & union_node = query_tree_node->as<UnionNode &>();
                const auto & union_queries = union_node.getQueries().getNodes();
                if (union_queries.empty())
                    return {};
                query_tree_node = union_queries.front().get();
                break;
            }
            case QueryTreeNodeType::ARRAY_JOIN:
            {
                const auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
                query_tree_node = array_join_node.getTableExpression().get();
                break;
            }
            case QueryTreeNodeType::CROSS_JOIN:
            {
                return {};
            }
            case QueryTreeNodeType::JOIN:
            {
                const auto & join_node = query_tree_node->as<JoinNode &>();
                const auto join_kind = join_node.getKind();
                const auto join_strictness = join_node.getStrictness();
                std::unordered_set<QueryTreeNodeType> supported_table_expression_types
                    = {QueryTreeNodeType::TABLE, QueryTreeNodeType::QUERY, QueryTreeNodeType::UNION};

                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_strictness == JoinStrictness::All))
                    query_tree_node = join_node.getLeftTableExpression().get();
                else if (join_kind == JoinKind::Right && join_strictness != JoinStrictness::RightAny
                    && supported_table_expression_types.contains(join_node.getLeftTableExpression()->getNodeType()))
                    query_tree_node = join_node.getRightTableExpression().get();
                else
                    return {};
                if (saw_join)
                    *saw_join = true;
                break;
            }
            default:
                return {};
        }
    }

    return res;
}

/// Same traversal as findObjectStorageClusterDriverTableForDiagnostic used to be, but carrying QueryTreeNodePtr
/// (shared_ptr) instead of raw pointers, since the real dispatch (buildQueryPlanForObjectStorageCluster) needs
/// the exact driver node as a QueryTreeNodePtr to key IQueryTreeNode::cloneAndReplace() -- analogous to how
/// StorageDistributed::buildQueryTreeDistributed() uses query_info.table_expression as that key. No longer
/// diagnostic-only; used for real by findObjectStorageClusterWholeQueryDriver() below.
QueryTreeNodePtr findObjectStorageClusterDriverTableNode(QueryTreeNodePtr query_tree_node)
{
    std::stack<QueryTreeNodePtr> join_nodes;
    while (query_tree_node || !join_nodes.empty())
    {
        if (!query_tree_node)
        {
            query_tree_node = join_nodes.top();
            join_nodes.pop();
        }

        switch (query_tree_node->getNodeType())
        {
            case QueryTreeNodeType::TABLE:
            {
                if (isObjectStorageClusterTable(*query_tree_node))
                    return query_tree_node;
                query_tree_node = nullptr;
                break;
            }
            case QueryTreeNodeType::TABLE_FUNCTION:
            {
                query_tree_node = nullptr;
                break;
            }
            case QueryTreeNodeType::QUERY:
            {
                auto & query_node_to_process = query_tree_node->as<QueryNode &>();
                query_tree_node = query_node_to_process.getJoinTree();
                break;
            }
            case QueryTreeNodeType::UNION:
            {
                auto & union_node = query_tree_node->as<UnionNode &>();
                const auto & union_queries = union_node.getQueries().getNodes();
                query_tree_node = nullptr;
                if (!union_queries.empty())
                    query_tree_node = union_queries.front();
                break;
            }
            case QueryTreeNodeType::ARRAY_JOIN:
            {
                auto & array_join_node = query_tree_node->as<ArrayJoinNode &>();
                query_tree_node = array_join_node.getTableExpression();
                break;
            }
            case QueryTreeNodeType::CROSS_JOIN:
            {
                return nullptr;
            }
            case QueryTreeNodeType::JOIN:
            {
                auto & join_node = query_tree_node->as<JoinNode &>();
                const auto join_kind = join_node.getKind();

                if (join_kind == JoinKind::Left || (join_kind == JoinKind::Inner && join_node.getStrictness() == JoinStrictness::All))
                {
                    auto right_table_expression = join_node.getRightTableExpression();
                    query_tree_node = join_node.getLeftTableExpression();
                    join_nodes.push(std::move(right_table_expression));
                }
                else if (join_kind == JoinKind::Right)
                {
                    auto left_table_expression = join_node.getLeftTableExpression();
                    query_tree_node = join_node.getRightTableExpression();
                    join_nodes.push(std::move(left_table_expression));
                }
                else
                {
                    return nullptr;
                }
                break;
            }
            default:
                return nullptr;
        }
    }

    return nullptr;
}

}

void logObjectStorageClusterParallelReplicasCandidate(
    const QueryTreeNodePtr & query_tree_node, const ContextPtr & context, const SelectQueryOptions & select_query_options)
{
    if (!context->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap])
        return;

    /// This constructor also runs for the recursive per-subquery Planner instances that plan each table
    /// expression independently (see buildJoinTreeQueryPlan()'s `Planner subquery_planner(...)`), not only for
    /// the outermost query. Only the outermost query's traversal is meaningful for "does the whole query,
    /// CTEs included, reach the driver table" -- a recursive subquery's own Planner only ever sees its own
    /// already-flattened-out subtree, so it trivially reports candidates=1 for itself.
    if (select_query_options.is_subquery)
        return;

    auto * query_node = query_tree_node->as<QueryNode>();
    auto * union_node = query_tree_node->as<UnionNode>();
    if (!query_node && !union_node)
        return;

    auto candidates = getSupportingObjectStorageClusterQueriesForDiagnostic(query_tree_node.get());
    QueryTreeNodePtr driver_table = findObjectStorageClusterDriverTableNode(query_tree_node);

    LOG_WARNING(
        getLogger("ParallelReplicasClusterDiag"),
        "PR_CLUSTER_DIAG depth={} is_subquery={} candidates={} outermost_candidate_is_top_query={} driver_table={}",
        select_query_options.subquery_depth,
        select_query_options.is_subquery,
        candidates.size(),
        (!candidates.empty() && candidates.front() == query_tree_node.get()),
        driver_table ? driver_table->as<TableNode &>().getStorageID().getFullTableName() : "<none>");
}

/// Walk the query tree looking for a UNION node whose every child query
/// ultimately reads from a table eligible for parallel replicas.
/// Returns the first such UNION node, or nullptr if none found.
static const UnionNode * findTableUnionForParallelReplicas(const IQueryTreeNode * query_tree_node, const ContextPtr & context)
{
    while (query_tree_node)
    {
        switch (query_tree_node->getNodeType())
        {
            case QueryTreeNodeType::QUERY:
            {
                const auto & query_node = query_tree_node->as<QueryNode &>();
                query_tree_node = query_node.getJoinTree().get();
                break;
            }
            case QueryTreeNodeType::UNION:
            {
                const auto & union_node = query_tree_node->as<UnionNode &>();
                if (union_node.getUnionMode() != SelectUnionMode::UNION_ALL)
                    return nullptr;

                const auto & union_queries = union_node.getQueries().getNodes();

                if (union_queries.empty())
                    return nullptr;

                /// Check that every child query in the UNION has an eligible table.
                bool all_children_support_parallel_replicas = true;
                for (const auto & child : union_queries)
                {
                    if (!findTableForParallelReplicas(child.get(), context))
                    {
                        all_children_support_parallel_replicas = false;
                        break;
                    }
                }

                if (all_children_support_parallel_replicas)
                    return &union_node;

                return nullptr;
            }
            case QueryTreeNodeType::TABLE:
            {
                /// Single table, not a UNION — no UNION node to return.
                return nullptr;
            }
            default:
                return nullptr;
        }
    }
    return nullptr;
}

const UnionNode * findTableUnionForParallelReplicas(const QueryTreeNodePtr & query_tree_node, const SelectQueryOptions & select_query_options)
{
    if (select_query_options.only_analyze)
        return nullptr;

    auto * query_node = query_tree_node->as<QueryNode>();
    auto * union_node = query_tree_node->as<UnionNode>();

    if (!query_node && !union_node)
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD,
            "Expected QUERY or UNION node. Actual {}",
            query_tree_node->formatASTForErrorMessage());

    auto context = query_node ? query_node->getContext() : union_node->getContext();

    const auto & settings = context->getSettingsRef();
    if (!settings[Setting::parallel_replicas_allow_view_over_mergetree])
        return nullptr;

    if (!settings[Setting::serialize_query_plan] && !context->canUseParallelReplicasOnFollower())
        return nullptr;

    return findTableUnionForParallelReplicas(query_tree_node.get(), context);
}

JoinTreeQueryPlan buildQueryPlanForParallelReplicas(
    const QueryNode & query_node,
    const PlannerContextPtr & planner_context,
    std::shared_ptr<const StorageLimitsList> storage_limits)
{
    auto processed_stage = QueryProcessingStage::WithMergeableState;
    auto context = planner_context->getQueryContext();

    QueryTreeNodePtr modified_query_tree = query_node.clone();

    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    rewriteJoinToGlobalJoin(modified_query_tree, context);
    modified_query_tree = buildQueryTreeForShard(planner_context, modified_query_tree, /*allow_global_join_for_right_table*/ true);

    auto [header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    auto modified_query_tree_for_ast = modified_query_tree->clone();
    removeGroupingFunctionSpecializations(modified_query_tree_for_ast);
    ASTPtr modified_query_ast = queryNodeToDistributedSelectQuery(modified_query_tree_for_ast);

    const TableNode * table_node = findTableForParallelReplicas(modified_query_tree.get(), context);
    if (!table_node)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Can't determine table for parallel replicas");

    QueryPlan query_plan;
    ClusterProxy::executeQueryWithParallelReplicas(
        query_plan,
        table_node->getStorageID(),
        header,
        processed_stage,
        modified_query_ast,
        std::move(modified_query_tree),
        std::move(new_planner_context),
        context,
        storage_limits,
        nullptr);

    auto converting = ActionsDAG::makeConvertingActions(
        header->getColumnsWithTypeAndName(),
        initial_header->getColumnsWithTypeAndName(),
        ActionsDAG::MatchColumnsMode::Position,
        context,
        false /*ignore_constant_values*/,
        false /*add_cast_columns*/,
        nullptr /*new_names*/);

    /// initial_header is a header expected by initial query.
    /// header is a header which is returned by the follower.
    /// They are different because tables will have different aliases (e.g. _table1 or _table5).
    /// Here we just rename columns by position, with the hope the types would match.
    auto step = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), std::move(converting));
    step->setStepDescription("Convert distributed names");
    query_plan.addStep(std::move(step));

    return {std::move(query_plan), std::move(processed_stage), {}, {}, {}};
}

namespace
{

/// True when query_node's JOIN tree, walked leftmost-first through any number of JOINs, reaches a TABLE or
/// TABLE_FUNCTION directly -- i.e. the driving table (if any) is exactly the shape PlannerJoinTree.cpp's
/// existing should_wrap_left_table bypass (see IStorageCluster-JOIN-pushdown experiment, PlannerJoinTree.cpp
/// §3.1) already handles. Used to make sure the whole-query dispatch below only takes over for the shape it
/// doesn't handle -- the driver nested at least one CTE/subquery level down (e.g. q21) -- and never second-
/// guesses the already-validated flat-JOIN case (e.g. q17).
bool isObjectStorageClusterDriverImmediateLeftmostTableExpression(const QueryNode & query_node)
{
    const IQueryTreeNode * join_tree_node = query_node.getJoinTree().get();
    if (!join_tree_node)
        return false;

    while (join_tree_node->getNodeType() == QueryTreeNodeType::JOIN)
        join_tree_node = join_tree_node->as<const JoinNode &>().getLeftTableExpression().get();

    auto node_type = join_tree_node->getNodeType();
    return node_type == QueryTreeNodeType::TABLE || node_type == QueryTreeNodeType::TABLE_FUNCTION;
}

}

/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): returns the driving IStorageCluster
/// TableNode (as an exact QueryTreeNodePtr, suitable for cloneAndReplace()) for query_tree's whole-query
/// dispatch (see buildQueryPlanForObjectStorageCluster below), or nullptr if this query isn't eligible. Reuses
/// the exact structural traversal already proven live against IcebergBench q21 by
/// logObjectStorageClusterParallelReplicasCandidate() (§3.4).
///
/// Deliberately NOT the full MergeTree dummy-plan eligibility walk (findQueryForParallelReplicas()'s
/// StorageDummy-based step-by-step validation that every intermediate plan node between candidate and driver
/// is Expression/Filter/Join/mergeable-Sorting) -- that walk is tuned for MergeTree's semantics, where the
/// *other* side of a JOIN may not be independently readable on every replica, so it deliberately falls back to
/// a narrower candidate once it sees e.g. an Aggregating step inside a JOIN branch. For a DataLake/object
/// storage JOIN, the non-driver side (e.g. q21's alert_events, itself a JOIN+Aggregating tree) is backed by the
/// same shared catalog and *is* safely re-computable in full by every worker -- recomputing it once per worker
/// is the intended tradeoff (driver gets partitioned via the task iterator, everything else re-reads/re-
/// aggregates locally). Porting the MergeTree walk here unmodified would very likely reject q21's outer query
/// and fall back to a narrower (wrong-for-us) candidate; a DataLake-specific eligibility policy needs its own
/// design, not a blind port. Tracked as an open item, not solved by this prototype.
QueryTreeNodePtr findObjectStorageClusterWholeQueryDriver(const QueryTreeNodePtr & query_tree, const ContextPtr & context)
{
    if (!context->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap])
        return nullptr;

    auto * query_node = query_tree->as<QueryNode>();
    if (!query_node)
        return nullptr;

    if (isObjectStorageClusterDriverImmediateLeftmostTableExpression(*query_node))
        return nullptr;

    /// Require an actual JOIN somewhere between query_tree and the driver -- this whole mechanism exists to
    /// bypass IStorageCluster's JOIN-wrap guard, so it must never fire for a candidate that is merely a chain
    /// of derived-table subqueries wrapping one table with no JOIN at all (see saw_join's comment on
    /// getSupportingObjectStorageClusterQueriesForDiagnostic() above -- confirmed live: without this check,
    /// IcebergBench q2's `appinfo_d` CTE body, which has no JOIN, was wrongly picked up here with `appinfo` as
    /// driver, dispatching it to the cluster independently and producing a column-identifier mismatch
    /// ("Not found column __tableN.appinfo_app in block") once the outer q2 JOIN tried to read its result).
    bool saw_join = false;
    auto candidates = getSupportingObjectStorageClusterQueriesForDiagnostic(query_tree.get(), &saw_join);
    if (candidates.empty() || candidates.front() != query_tree.get() || !saw_join)
        return nullptr;

    return findObjectStorageClusterDriverTableNode(query_tree);
}

/// EXPERIMENTAL PROTOTYPE (see object_storage_cluster_bypass_join_wrap): sibling of
/// buildQueryPlanForParallelReplicas() above for an IStorageCluster driver (e.g. Iceberg
/// StorageObjectStorageCluster) instead of MergeTree. Unlike the MergeTree path, does NOT go through
/// ClusterProxy::executeQueryWithParallelReplicas (task/part-range-based coordinator, MergeTree-specific);
/// instead it hands the whole (CTE-inlined) candidate query straight to the driver storage's own
/// IStorageCluster::readPreparedClusterQuery() / ReadFromCluster path -- the same execution mechanism already
/// proven correct for q17 (see §2), just invoked here with the driver possibly several CTE/subquery scopes
/// below query_tree (q21's shape, see §3.4), rather than only when the driver is the immediate JOIN leftmost
/// table (PlannerJoinTree.cpp's existing bypass, §3.1).
///
/// Follows StorageDistributed::buildQueryTreeDistributed()'s exact-node replacement pattern: build the
/// driver's own *Cluster table-function form as a resolved TableFunctionNode, then
/// query_tree->cloneAndReplace(driver_table_expression, table_function_node) -- one call, no AST-position or
/// StorageID search, correct for self-joins by construction. cloneAndReplace() rebinds every ColumnNode's
/// column_source weak pointer from the old TableNode to the new TableFunctionNode automatically, so the
/// resulting query is already correctly qualified once serialized -- no RestoreQualifiedNamesVisitor needed.
JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
    const QueryTreeNodePtr & query_tree,
    const QueryTreeNodePtr & driver_table_expression,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context)
{
    auto processed_stage = QueryProcessingStage::WithMergeableState;
    auto context = planner_context->getQueryContext();

    auto & driver_table_node = driver_table_expression->as<TableNode &>();
    auto driver_storage = std::dynamic_pointer_cast<StorageObjectStorageCluster>(driver_table_node.getStorage());
    if (!driver_storage)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Object storage cluster whole-query dispatch driver {} is not a StorageObjectStorageCluster",
            driver_table_node.getStorageID().getNameForLogs());

    auto driver_storage_snapshot = driver_table_node.getStorageSnapshot();

    /// EXPERIMENTAL (see object_storage_cluster_bypass_join_wrap, §3.6 in ICEBERG_JOIN_EXPERIMENT.md):
    /// driver_table_node's own getClusterName() may well be empty here -- parallel_replicas_for_cluster_engines
    /// could already have been forced false for the driver's own resolution scope by an ancestor query's
    /// analyzer decision (§3.2), long before this whole-query dispatch ever ran. Rather than relaxing that
    /// analyzer decision for the whole scope (which would make every DataLake table under the query
    /// cluster-aware, not just the chosen driver), read cluster_for_parallel_replicas explicitly and pass it
    /// only into the driver's own *Cluster table-function arguments below -- every other table in the query
    /// resolves normally, per-worker, from the query text itself.
    const auto & cluster_name = context->getSettingsRef()[Setting::cluster_for_parallel_replicas].value;
    if (cluster_name.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "object_storage_cluster_bypass_join_wrap whole-query dispatch requires cluster_for_parallel_replicas to be set");

    /// Build the driver's own *Cluster table-function form (e.g. icebergS3Cluster('vig-test', ...)), the same
    /// shape already proven correct for the leftmost-table (q17) dispatch, as a standalone AST fragment, then
    /// turn it into a resolved TableFunctionNode exactly the way
    /// StorageDistributed::buildQueryTreeDistributed()'s `remote_table_function` branch does.
    ASTPtr cluster_function_ast = driver_storage->buildClusterTableFunctionAST(cluster_name, driver_storage_snapshot, context);

    auto function_query_tree = buildQueryTree(cluster_function_ast, context);
    auto & function_node = function_query_tree->as<FunctionNode &>();

    auto table_function_node = std::make_shared<TableFunctionNode>(function_node.getFunctionName());
    table_function_node->getArgumentsNode() = function_node.getArgumentsNode();
    table_function_node->setSettingsChanges(function_node.getSettingsChanges());

    {
        QueryAnalysisPass query_analysis_pass;
        QueryTreeNodePtr node = table_function_node;
        query_analysis_pass.run(node, context);
    }

    table_function_node->setAlias(driver_table_expression->getAlias());

    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
        query_tree->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Exact-node QueryTree replacement (StorageDistributed's pattern): clone the whole candidate tree and
    /// replace only this precise TableNode, however deeply it is nested under CTEs/subqueries.
    QueryTreeNodePtr modified_query_tree = query_tree->cloneAndReplace(driver_table_expression, table_function_node);

    auto [header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    auto modified_query_tree_for_ast = modified_query_tree->clone();
    removeGroupingFunctionSpecializations(modified_query_tree_for_ast);
    ASTPtr modified_query_ast = queryNodeToDistributedSelectQuery(modified_query_tree_for_ast);

    /// Preserve the *real* enclosing SelectQueryInfo (storage limits, has_aggregates/has_window, etc.) instead
    /// of fabricating a near-empty one, mirroring PlannerJoinTree.cpp's buildQueryPlanForTableExpression() /
    /// StorageDistributed::read()'s `SelectQueryInfo modified_query_info = select_query_info;` pattern.
    SelectQueryInfo query_info = select_query_info;
    query_info.query = modified_query_ast;
    query_info.query_tree = modified_query_tree;
    /// table_expression must be a node actually present in query_info.query_tree, since
    /// new_planner_context's TableExpressionData is keyed by node identity within *this* (modified) tree --
    /// driver_table_expression (the old txnlog TableNode) was replaced and no longer exists in it.
    /// table_function_node is the exact replacement registered here; ReadFromCluster's task-iterator generation
    /// still goes through driver_storage/driver_storage_snapshot below, passed separately -- this field is only
    /// planner bookkeeping (e.g. SourceStepWithFilter::applyFilters()'s buildNodeNameToInputNodeColumn()).
    query_info.table_expression = table_function_node;
    query_info.planner_context = new_planner_context;

    /// KNOWN GAP, tracked in ICEBERG_JOIN_EXPERIMENT.md (review issue #3): ideally column_names and
    /// query_info.filter_actions_dag would come from the driver's own TableExpressionData (exact required
    /// columns, plus its filter DAG so StorageObjectStorageCluster::getTaskIteratorExtension() can still prune
    /// Iceberg files/partitions at the initiator) the way buildQueryPlanForTableExpression() does for a normal
    /// per-table read(). That data isn't available here: collectTableExpressionData() deliberately does not
    /// descend into subquery/CTE scopes (each subquery normally gets its own collectTableExpressionData() pass
    /// when planned recursively -- exactly the recursion this whole-query dispatch bypasses). Using
    /// AllPhysical and leaving filter_actions_dag unset is correct but not minimal: workers still apply the
    /// query's own WHERE clause correctly, so results are right; the only cost is the initiator's task
    /// iterator not pruning files/partitions by predicate for this driver, i.e. more work, never wrong data.
    Names column_names = driver_storage_snapshot->getColumns(GetColumnsOptions(GetColumnsOptions::AllPhysical)).getNames();

    QueryPlan query_plan;
    driver_storage->readPreparedClusterQuery(
        query_plan,
        column_names,
        driver_storage_snapshot,
        query_info,
        context,
        header,
        modified_query_ast,
        processed_stage,
        cluster_name);

    if (query_plan.isInitialized())
    {
        /// Same position-based rename as buildQueryPlanForParallelReplicas() above: re-analyzing the replaced
        /// tree can produce different internal column names/aliases (e.g. _table1) than the original.
        auto converting = ActionsDAG::makeConvertingActions(
            query_plan.getCurrentHeader()->getColumnsWithTypeAndName(),
            initial_header->getColumnsWithTypeAndName(),
            ActionsDAG::MatchColumnsMode::Position,
            context,
            false /*ignore_constant_values*/,
            false /*add_cast_columns*/,
            nullptr /*new_names*/);

        auto step = std::make_unique<ExpressionStep>(query_plan.getCurrentHeader(), std::move(converting));
        step->setStepDescription("Convert object storage cluster whole-query names");
        query_plan.addStep(std::move(step));
    }

    return {std::move(query_plan), processed_stage, {}, {}, {}};
}

}
