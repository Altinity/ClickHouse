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
#include <Interpreters/Context.h>
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

bool isObjectStorageClusterDriverEligible(const IStorage & storage, const ContextPtr & context)
{
    if (!dynamic_cast<const StorageObjectStorageCluster *>(&storage))
        return false;

    /// Deliberately does NOT check parallel_replicas_for_cluster_engines: stock
    /// QueryAnalyzer::resolveQuery() (TableFunctionsWithClusterAlternativesVisitor's has_join check,
    /// untouched by this redesign -- see ICEBERG_JOIN_EXPERIMENT.md §12) unconditionally forces that
    /// setting to false on any query node containing a JOIN, before the Planner ever runs. Since q17/q21
    /// both contain a JOIN, that flag would read false here regardless of what the query's own SETTINGS
    /// requested, making it useless as an opt-in signal for this mechanism. object_storage_cluster_bypass_join_wrap
    /// is untouched by any analyzer logic and is the sole, correctly-scoped opt-in gate.
    ///
    /// query_kind != SECONDARY_QUERY guards against recursive re-fanout: once the whole-query dispatch built
    /// here reaches a worker (via ReadFromCluster), that worker's own analysis of the received query text
    /// sees the same settings (object_storage_cluster_bypass_join_wrap, cluster_for_parallel_replicas
    /// propagate like any other setting) and, without this guard, would find any other eligible
    /// StorageObjectStorageCluster JOIN still embedded in that text (e.g. IcebergBench q21's alert_events:
    /// event_alert LEFT JOIN policy_matches) and try to dispatch it *again* from within an already-distributed
    /// execution. A worker executing a query it received this way always has query_kind==SECONDARY_QUERY, so
    /// this confines whole-query dispatch to the initiator's own top-level analysis.
    ///
    /// Deliberately does NOT check context->isDistributed(): that flag does not mean "executing as a
    /// distributed sub-query" in this codebase. QueryAnalyzer.cpp sets it, per query SCOPE, to
    /// storage->isRemote() the moment that scope's own join tree resolves a remote-capable table --
    /// IStorageCluster::isRemote() is unconditionally true, so q17's and q21's own top-level scopes get
    /// isDistributed()=true purely because event_page/txnlog are IStorageCluster-derived, on the initiator,
    /// during ordinary analysis, before this function is ever consulted. Checking it here made this predicate
    /// always false for exactly the tables it needs to select -- confirmed live (q17 stopped distributing
    /// entirely). query_kind is the correct, unrelated "am I a worker" signal; isDistributed() is not.
    const auto & settings = context->getSettingsRef();
    return settings[Setting::object_storage_cluster_bypass_join_wrap]
        && !settings[Setting::cluster_for_parallel_replicas].value.empty()
        && context->getClientInfo().query_kind != ClientInfo::QueryKind::SECONDARY_QUERY;
}

bool isTableNodeEligibleForParallelReplicas(const TableNode & table_node, const StoragePtr & storage, const ContextPtr & context)
{
    if (isObjectStorageClusterDriverEligible(*storage, context))
        return true;

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
///
/// `non_driver_branch_roots`, when non-null, collects the *other* side of every JOIN encountered along this
/// walk -- exactly the child NOT followed toward the driver (see the JOIN case below). Used by
/// findQueryForParallelReplicas() to let an object-storage-cluster driver's candidate widen past a JOIN
/// whose non-driver branch needs its own finalization (e.g. GROUP BY) -- safe there because every worker can
/// fully recompute that branch from the same shared catalog, unlike MergeTree's non-driver side. See
/// ICEBERG_JOIN_EXPERIMENT.md §12.11.
static std::vector<const QueryNode *> getSupportingParallelReplicasQueries(
    const IQueryTreeNode * query_tree_node,
    const ContextPtr & context,
    std::vector<const IQueryTreeNode *> * non_driver_branch_roots = nullptr)
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

                /// DIAGNOSTIC (temporary, see ICEBERG_JOIN_EXPERIMENT.md §12.12): pin down whether the
                /// dummy-substituted driver (StorageDummy, not the real StorageObjectStorageCluster) fails
                /// this eligibility check -- would explain PR_CANDIDATE_CLASSIFY's selected_is_null=true
                /// despite non_driver_plan_roots being correctly populated.
                if (context->getSettingsRef()[Setting::object_storage_cluster_bypass_join_wrap])
                {
                    const auto & storage = table_node.getStorage();
                    LOG_WARNING(getLogger("ParallelReplicasClusterDiag"),
                        "PR_TABLE_ELIGIBILITY_FAIL table={} is_storage_dummy={} is_merge_tree={} "
                        "supports_replication={} parallel_replicas_for_non_replicated_merge_tree={}",
                        table_node.getStorageID().getFullTableName(),
                        typeid_cast<const StorageDummy *>(storage.get()) != nullptr,
                        storage->isMergeTree(),
                        storage->supportsReplication(),
                        context->getSettingsRef()[Setting::parallel_replicas_for_non_replicated_merge_tree].value);
                }

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
                {
                    if (non_driver_branch_roots)
                        non_driver_branch_roots->push_back(join_node.getRightTableExpression().get());
                    query_tree_node = join_node.getLeftTableExpression().get();
                }
                else if (join_kind == JoinKind::Right && join_strictness != JoinStrictness::RightAny
                    && supported_table_expression_types.contains(join_node.getLeftTableExpression()->getNodeType()))
                {
                    if (non_driver_branch_roots)
                        non_driver_branch_roots->push_back(join_node.getLeftTableExpression().get());
                    query_tree_node = join_node.getRightTableExpression().get();
                }
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

            /// `StorageDummy` erases the original storage's type entirely -- isTableNodeEligibleForParallelReplicas()'s
            /// own isObjectStorageClusterDriverEligible() check (a dynamic_cast<StorageObjectStorageCluster*>) can
            /// never succeed against a StorageDummy, so an object-storage-cluster driver silently fails the later
            /// (dummy-tree) eligibility re-check inside getSupportingParallelReplicasQueries() -- even though the
            /// *same* driver was already confirmed eligible against its real storage earlier in
            /// findQueryForParallelReplicas()'s public overload. StorageDummy's own MergeTree/replication fallback
            /// then also fails for a DataLake table (StorageObjectStorage::supportsReplication() reports the
            /// underlying catalog's replication semantics, not "is this a valid parallel-replicas candidate").
            /// Confirmed live: q21's dummy-substituted `txnlog` reported `supports_replication=false`, making the
            /// whole dummy-tree candidate stack come back empty, silently discarding a driver that was correctly
            /// found eligible moments earlier. Fix: decide eligibility once, against the *real* storage, before it
            /// is replaced, and fold it into the one signal the dummy can still carry forward. See
            /// ICEBERG_JOIN_EXPERIMENT.md §12.13.
            const bool dummy_supports_replication = storage.supportsReplication() || isObjectStorageClusterDriverEligible(storage, getContext());

            auto storage_dummy = std::make_shared<StorageDummy>(
                storage.getStorageID(),
                /// To preserve information about alias columns, column description must be extracted directly from storage metadata.
                storage_snapshot->metadata->getColumns(),
                storage_snapshot,
                dummy_supports_replication);

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
///
/// `non_driver_plan_roots` / `relax_for_object_storage_driver`: when the eventual driver is an
/// object-storage-cluster table, a step needing initiator finalization (e.g. GROUP BY) found *inside a
/// JOIN's non-driver branch* -- one of the `non_driver_plan_roots` recorded by
/// getSupportingParallelReplicasQueries(), or any of its descendants -- does not have to narrow the
/// candidate the way it must for MergeTree (whose non-driver side cannot be assumed independently
/// re-computable per replica); every worker can safely and fully recompute an object-storage JOIN's
/// non-driver branch from the same shared catalog. `non_driver_plan_roots` is empty and
/// `relax_for_object_storage_driver` is false for MergeTree, so this parameter has zero effect there --
/// MergeTree's own decision still goes entirely through the untouched, last-write `currently_inside_join`
/// exactly as before. Confirmed live: without this relaxation, IcebergBench q21's `alert_events` branch
/// (itself a JOIN with its own GROUP BY) forced the outer q21 query to be rejected in favor of a narrower
/// candidate, splitting the dispatch into two separate `ReadFromCluster` calls with mismatched internal
/// column identifiers between them, instead of one whole-query dispatch. See ICEBERG_JOIN_EXPERIMENT.md §12.11.
///
/// This does not weaken the *other* rejection path just below (`if (!res) return nullptr;`), which still
/// unconditionally excludes a driver directly wrapped by such a step with no enclosing JOIN at all (e.g.
/// IcebergBench q2's `appinfo_d`, a window function over the driver table with no JOIN in its own scope).
static const QueryNode * findQueryForParallelReplicas(
    std::vector<const QueryNode *> stack,
    const std::unordered_map<const QueryNode *, const QueryPlan::Node *> & mapping,
    const Settings & settings,
    const std::unordered_set<const QueryPlan::Node *> & non_driver_plan_roots,
    bool relax_for_object_storage_driver)
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
        /// Sticky, propagated to every descendant once set: true once this frame's `node` is, or descends
        /// from (possibly through passthrough wrapper steps such as the "Pre Join Actions" Expression a JOIN
        /// commonly inserts above a subquery's own recorded plan root -- see this function's own comment),
        /// one of `non_driver_plan_roots`. Checked at *every* visited node (not just immediate JOIN children)
        /// specifically to see through such wrapper steps without needing a separate subtree search.
        bool on_non_driver_branch = false;
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
        nodes_to_check.push({.node = it->second, .inside_join = false, .on_non_driver_branch = false});
        bool can_distribute_full_node = true;
        bool currently_inside_join = false;
        /// Cumulative, unlike currently_inside_join above: the walk below does not stop at the first
        /// non-passthrough step it finds, so a later, safe (non-driver-branch) failure must not erase an
        /// earlier, unsafe one, or vice versa -- order must not matter. Only ever consulted when
        /// relax_for_object_storage_driver is true; MergeTree's own decision never reads this.
        bool saw_unsafe_join_branch_failure = false;

        while (!nodes_to_check.empty())
        {
            /// Copy to avoid container overflow (we call pop() in the next line).
            const auto [next_node_to_check, inside_join, inherited_on_non_driver_branch] = nodes_to_check.top();
            nodes_to_check.pop();
            const bool on_non_driver_branch = inherited_on_non_driver_branch || non_driver_plan_roots.contains(next_node_to_check);
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
                    if (inside_join && !on_non_driver_branch)
                        saw_unsafe_join_branch_failure = true;
                }

                nodes_to_check.push({.node = children.front(), .inside_join = inside_join, .on_non_driver_branch = on_non_driver_branch});
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
                    nodes_to_check.push({.node = child, .inside_join = true, .on_non_driver_branch = on_non_driver_branch});
            }
        }

        if (!can_distribute_full_node)
        {
            /// Current query node does not contain subqueries.
            /// We can execute parallel replicas over storage::read.
            if (!res)
                return nullptr;

            const bool should_narrow = relax_for_object_storage_driver ? saw_unsafe_join_branch_failure : currently_inside_join;
            return should_narrow ? res : subquery_node;
        }

        /// Query is simple enough to be fully distributed.
        res = subquery_node;
    }

    return res;
}

/// Forward declaration: defined below, needed here (§ "Determine the eventual driver's storage kind" in
/// findQueryForParallelReplicas() just below) to pick the finalization policy before the dummy-plan walk runs.
static const TableNode * findTableForParallelReplicas(const IQueryTreeNode * query_tree_node, const ContextPtr & context);

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

    /// Determine the eventual driver's storage kind *before* running the dummy-plan walk below, purely
    /// structurally (same traversal `stack` above already performed, just continued down to the TABLE
    /// instead of stopping at the last eligible QUERY boundary) -- cheap, no dummy-table substitution or
    /// nested Planner construction. Controls which finalization policy the walk uses; see
    /// findQueryForParallelReplicas(stack, mapping, settings, ...)'s own comment.
    const TableNode * driver_for_policy = findTableForParallelReplicas(query_tree_node.get(), context);
    bool relax_for_object_storage_driver
        = driver_for_policy && dynamic_cast<const StorageObjectStorageCluster *>(driver_for_policy->getStorage().get());

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
    /// Only object-storage-cluster drivers need non-driver-branch roots collected at all -- skip the
    /// (harmless but pointless) extra bookkeeping for MergeTree.
    std::vector<const IQueryTreeNode *> non_driver_branch_roots;
    auto new_stack = getSupportingParallelReplicasQueries(
        updated_query_tree.get(), context, relax_for_object_storage_driver ? &non_driver_branch_roots : nullptr);
    const auto & mapping = planner.getQueryNodeToPlanStepMapping();

    /// Translate the collected non-driver QueryTreeNode siblings into plan-node identity once, up front --
    /// the vocabulary the walk below actually operates in. Only QUERY-type siblings matter: a bare TABLE
    /// non-driver side (e.g. IcebergBench q17's geo_location_lookup) has no internal steps to protect.
    std::unordered_set<const QueryPlan::Node *> non_driver_plan_roots;
    for (const auto * sibling : non_driver_branch_roots)
    {
        if (const auto * sibling_query_node = sibling->as<QueryNode>())
        {
            if (auto mapping_it = mapping.find(sibling_query_node); mapping_it != mapping.end())
                non_driver_plan_roots.insert(mapping_it->second);
        }
    }

    const auto * res = findQueryForParallelReplicas(
        new_stack, mapping, context->getSettingsRef(), non_driver_plan_roots, relax_for_object_storage_driver);

    /// DIAGNOSTIC (temporary, see ICEBERG_JOIN_EXPERIMENT.md §12.11): confirm the classification q21 was
    /// getting wrong before this fix. Removed once validated live.
    if (relax_for_object_storage_driver)
        LOG_WARNING(getLogger("ParallelReplicasClusterDiag"),
            "PR_CANDIDATE_CLASSIFY stack_size={} new_stack_size={} non_driver_branch_roots={} non_driver_plan_roots={} "
            "selected_is_outermost={} selected_is_innermost={} selected_is_null={}",
            stack.size(), new_stack.size(), non_driver_branch_roots.size(), non_driver_plan_roots.size(),
            !new_stack.empty() && res == new_stack.front(),
            !new_stack.empty() && res == new_stack.back(),
            res == nullptr);

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

const TableNode * findParallelReplicasCandidateDriver(const QueryNode * candidate)
{
    if (!candidate)
        return nullptr;

    /// Reuses the same private, unguarded findTableForParallelReplicas(IQueryTreeNode*, ContextPtr&)
    /// traversal buildQueryPlanForParallelReplicas() itself already relies on internally for MergeTree
    /// (`findTableForParallelReplicas(modified_query_tree.get(), context)`). Deliberately NOT the public
    /// findTableForParallelReplicas(QueryTreeNodePtr, SelectQueryOptions&) overload above: that overload's
    /// `serialize_query_plan || canUseParallelReplicasOnFollower()` gate exists specifically for
    /// PlannerJoinTree.cpp's View/MaterializedView follower-recursion-safety check
    /// (GlobalPlannerContext::parallel_replicas_table) and returns nullptr unconditionally on a normal
    /// initiator by design -- using it here would make this lookup always fail exactly where it's needed.
    ///
    /// `candidate` must be the exact QueryNode findQueryForParallelReplicas() selected (GlobalPlannerContext::
    /// parallel_replicas_node), not the original root query tree passed to it: getSupportingParallelReplicasQueries()'s
    /// traversal is deterministic and structural, so walking from `candidate` finds the identical driver a
    /// walk from the root would -- but deriving it directly from the actual selected candidate, rather than
    /// independently re-deriving it from the root and relying on the two traversals agreeing, keeps the
    /// relationship correct by construction. candidate->getContext() is used rather than threading a
    /// separate context parameter through the Planner constructor, since findQueryForParallelReplicas()'s own
    /// internal walk only ever produces genuine QueryNode entries (see its `res.push_back(&query_node_to_process)`),
    /// so `candidate`, when non-null, always owns a valid context of its own.
    return findTableForParallelReplicas(candidate, candidate->getContext());
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

/// Execution backend for an object-storage-cluster driver -- see findQueryForParallelReplicas.h for the full
/// contract and the two candidate-selection call sites that share this. Unlike the MergeTree path, does NOT go
/// through ClusterProxy::executeQueryWithParallelReplicas (task/part-range-based coordinator, MergeTree-
/// specific); instead it hands the whole (CTE-inlined) candidate query straight to the driver storage's own
/// IStorageCluster::readPreparedClusterQuery() / ReadFromCluster path.
///
/// Follows StorageDistributed::buildQueryTreeDistributed()'s exact-node replacement pattern: build the driver's
/// own *Cluster table-function form as a resolved TableFunctionNode, then replace `driver_table_node` inside
/// `query_tree` via IQueryTreeNode::cloneAndReplace() -- one call, no AST-position or StorageID search, correct
/// for self-joins by construction. The replacement is keyed by raw node identity (cloneAndReplace()'s
/// ReplacementMap is `unordered_map<const IQueryTreeNode *, QueryTreeNodePtr>`), so `driver_table_node` need not
/// be reached via a QueryTreeNodePtr owned by the caller -- both findTableForParallelReplicas() (returns a raw
/// `const TableNode *`) and PlannerJoinTree.cpp's leftmost TableNode reference work directly. cloneAndReplace()
/// rebinds every ColumnNode's column_source weak pointer from the old TableNode to the new TableFunctionNode
/// automatically, so the resulting query is already correctly qualified once serialized -- no
/// RestoreQualifiedNamesVisitor needed.
JoinTreeQueryPlan buildQueryPlanForObjectStorageCluster(
    const QueryTreeNodePtr & query_tree,
    const TableNode & driver_table_node,
    const SelectQueryInfo & select_query_info,
    const PlannerContextPtr & planner_context)
{
    auto processed_stage = QueryProcessingStage::WithMergeableState;
    auto context = planner_context->getQueryContext();

    auto driver_storage = std::dynamic_pointer_cast<StorageObjectStorageCluster>(driver_table_node.getStorage());
    if (!driver_storage)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Object storage cluster whole-query dispatch driver {} is not a StorageObjectStorageCluster",
            driver_table_node.getStorageID().getNameForLogs());

    auto driver_storage_snapshot = driver_table_node.getStorageSnapshot();

    /// driver_table_node's own getClusterName() may well be empty here -- e.g. for a driver nested under a
    /// CTE, whose own resolution scope never independently resolved a cluster name. Read
    /// cluster_for_parallel_replicas explicitly and pass it only into the driver's own *Cluster table-function
    /// arguments below (as a literal in the query text, never as a propagated Context setting -- see
    /// StorageObjectStorageCluster::buildClusterTableFunctionAST()) -- every other table in the query resolves
    /// normally, per-worker, from the query text itself.
    const auto & cluster_name = context->getSettingsRef()[Setting::cluster_for_parallel_replicas].value;
    if (cluster_name.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "object_storage_cluster_bypass_join_wrap whole-query dispatch requires cluster_for_parallel_replicas to be set");

    /// Build the driver's own *Cluster table-function form (e.g. icebergS3Cluster('vig-test', ...)) as a
    /// standalone AST fragment, then turn it into a resolved TableFunctionNode exactly the way
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

    table_function_node->setAlias(driver_table_node.getAlias());

    auto initial_header = InterpreterSelectQueryAnalyzer::getSampleBlock(
        query_tree->clone(), context, SelectQueryOptions(processed_stage).analyze());

    /// Exact-node QueryTree replacement (StorageDistributed's pattern): clone the whole candidate tree and
    /// replace only this precise TableNode, however deeply it is nested under CTEs/subqueries.
    IQueryTreeNode::ReplacementMap replacement_map;
    replacement_map.emplace(&driver_table_node, table_function_node);
    QueryTreeNodePtr modified_query_tree = query_tree->cloneAndReplace(replacement_map);

    auto [header, new_planner_context] = InterpreterSelectQueryAnalyzer::getSampleBlockAndPlannerContext(
        modified_query_tree, context, SelectQueryOptions(processed_stage).analyze());

    auto modified_query_tree_for_ast = modified_query_tree->clone();
    removeGroupingFunctionSpecializations(modified_query_tree_for_ast);
    ASTPtr modified_query_ast = queryNodeToDistributedSelectQuery(modified_query_tree_for_ast);

    /// DIAGNOSTIC (temporary, see ICEBERG_JOIN_EXPERIMENT.md §12.9): log the exact text dispatched to workers,
    /// to pin down a NOT_FOUND_COLUMN_IN_BLOCK confirmed live on q21 -- a header/alias mismatch that survives
    /// only the serialize (queryNodeToDistributedSelectQuery) -> wire -> re-parse-on-worker round trip, since
    /// the identical query tree executes correctly in-process (undistributed) today. Removed once diagnosed.
    LOG_WARNING(getLogger("ObjectStorageClusterDispatch"), "OSC_DISPATCH query={}", modified_query_ast->formatForLogging());

    /// Preserve the *real* enclosing SelectQueryInfo (storage limits, has_aggregates/has_window, etc.) instead
    /// of fabricating a near-empty one, mirroring PlannerJoinTree.cpp's buildQueryPlanForTableExpression() /
    /// StorageDistributed::read()'s `SelectQueryInfo modified_query_info = select_query_info;` pattern.
    SelectQueryInfo query_info = select_query_info;
    query_info.query = modified_query_ast;
    query_info.query_tree = modified_query_tree;
    /// table_expression must be a node actually present in query_info.query_tree, since
    /// new_planner_context's TableExpressionData is keyed by node identity within *this* (modified) tree --
    /// driver_table_node (the old TableNode) was replaced and no longer exists in it.
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
