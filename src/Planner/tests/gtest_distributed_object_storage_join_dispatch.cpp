#include <sstream>

#include <gtest/gtest.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryTreePassManager.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <Core/NamesAndTypes.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypesNumber.h>
#include <Databases/DatabaseMemory.h>
#include <Databases/DatabasesCommon.h>
#include <IO/WriteBufferFromString.h>
#include <Interpreters/ClientInfo.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Interpreters/InterpreterSelectQueryAnalyzer.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/ParserSelectQuery.h>
#include <Parsers/parseQuery.h>
#include <Processors/QueryPlan/Optimizations/QueryPlanOptimizationSettings.h>
#include <Processors/QueryPlan/QueryPlan.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Processors/Sources/NullSource.h>
#include <QueryPipeline/Pipe.h>
#include <Storages/IStorageCluster.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionFactory.h>

using namespace DB;

namespace
{

NamesAndTypesList driverColumns()
{
    return {{"id", std::make_shared<DataTypeUInt64>()}};
}

NamesAndTypesList lookupColumns()
{
    return {{"lookup_id", std::make_shared<DataTypeUInt64>()}};
}

struct State;

/// A DatabaseMemory that reports itself as a DataLake catalog: that is what makes the tables inside it eligible
/// for dispatch (findDistributedObjectStorageCandidate asks DatabaseCatalog::isDatalakeCatalog).
class FakeDataLakeDatabase : public DatabaseWithOwnTablesBase
{
public:
    explicit FakeDataLakeDatabase(const String & name_, ContextPtr context_)
        : DatabaseWithOwnTablesBase(name_, "FakeDataLakeDatabase(" + name_ + ")", context_)
    {
    }

    String getEngineName() const override { return "FakeDataLakeCatalog"; }
    bool isDatalakeCatalog() const override { return true; }

private:
    ASTPtr getCreateDatabaseQueryImpl() const override { return nullptr; }
};

/// Rewrites the query's first table into a call of `function_name`, keeping its alias, as
/// StorageObjectStorageCluster::updateQueryToSendIfNeeded rewrites a catalog table into the table function
/// that reads the same files.
void rewriteFirstTableToFunction(ASTPtr & query, const String & function_name)
{
    auto & tables = query->as<ASTSelectQuery &>().tables()->as<ASTTablesInSelectQuery &>();
    auto & table_expression = tables.children[0]->as<ASTTablesInSelectQueryElement &>().table_expression->as<ASTTableExpression &>();
    if (!table_expression.database_and_table_name)
        return;

    auto function = makeASTFunction(function_name);
    function->setAlias(table_expression.database_and_table_name->tryGetAlias());
    table_expression.database_and_table_name = nullptr;
    table_expression.table_function = function;
    table_expression.children[0] = function;
}

/// Minimal driver stand-in; getTaskIteratorExtension() is only invoked during real pipeline execution,
/// which these tests never trigger -- they only check the plan.
class FakeDriverStorage : public IStorageCluster
{
public:
    FakeDriverStorage(const StorageID & table_id, String cluster_name_)
        : IStorageCluster(cluster_name_, table_id, getLogger("test"))
    {
        StorageInMemoryMetadata metadata;
        metadata.setColumns(ColumnsDescription{driverColumns()});
        setInMemoryMetadata(metadata);
    }

    std::string getName() const override { return "FakeDriverStorage"; }

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node *, const ActionsDAG *, const ContextPtr &, ClusterPtr, StorageMetadataPtr) const override
    {
        return {};
    }

protected:
    void updateQueryToSendIfNeeded(ASTPtr & query, const StorageSnapshotPtr &, const ContextPtr &, bool make_cluster_function) override
    {
        rewriteFirstTableToFunction(query, make_cluster_function ? "driverClusterFunction" : "driverTableFunction");
    }
};

/// Stand-in for a second table from the same DataLake catalog, e.g. ice.geo_location_lookup.
class FakeSafeLookupStorage : public IStorageCluster
{
public:
    explicit FakeSafeLookupStorage(const StorageID & table_id)
        : IStorageCluster(/*cluster_name_*/ "", table_id, getLogger("test"))
    {
        StorageInMemoryMetadata metadata;
        metadata.setColumns(ColumnsDescription{lookupColumns()});
        setInMemoryMetadata(metadata);
    }

    std::string getName() const override { return "FakeSafeLookupStorage"; }

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node *, const ActionsDAG *, const ContextPtr &, ClusterPtr, StorageMetadataPtr) const override
    {
        return {};
    }

protected:
    void updateQueryToSendIfNeeded(ASTPtr & query, const StorageSnapshotPtr &, const ContextPtr &, bool make_cluster_function) override
    {
        rewriteFirstTableToFunction(query, make_cluster_function ? "lookupClusterFunction" : "lookupTableFunction");
    }

    /// Empty cluster name -> plain reads (e.g. 'allow' mode) fall back here instead of ReadFromCluster.
    void readFallBackToPure(
        QueryPlan & query_plan,
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo &,
        ContextPtr,
        QueryProcessingStage::Enum,
        size_t,
        size_t) override
    {
        auto header = std::make_shared<const Block>(storage_snapshot->getSampleBlockForColumns(column_names));
        Pipe pipe(std::make_shared<NullSource>(header));
        query_plan.addStep(std::make_unique<ReadFromPreparedSource>(std::move(pipe)));
    }
};

/// Port 1 is never listening; irrelevant since the test only builds the QueryPlan.
void registerUnreachableCluster(const ContextMutablePtr & context, const String & cluster_name)
{
    std::ostringstream config_text;
    config_text << "<clickhouse><remote_servers><" << cluster_name << "><shard><replica>"
                << "<host>127.0.0.1</host><port>1</port>"
                << "</replica></shard></" << cluster_name << "></remote_servers></clickhouse>";
    std::istringstream config_stream(config_text.str());
    Poco::AutoPtr<Poco::Util::XMLConfiguration> config = new Poco::Util::XMLConfiguration(config_stream);
    context->setClustersConfig(config, /*enable_discovery=*/false);
}

/// Test fixture, modelled on src/Planner/tests/gtest_planner_empty_projection.cpp.
struct State
{
    State(const State &) = delete;

    ContextMutablePtr context;
    std::shared_ptr<FakeDriverStorage> driver;

    static State & instance()
    {
        static State state;
        return state;
    }

private:
    explicit State()
        : context(Context::createCopy(getContext().context))
    {
        tryRegisterFunctions();
        tryRegisterAggregateFunctions();

        /// Default test context leaves query_kind at NO_QUERY; must look like a real initiator query.
        ClientInfo client_info = context->getClientInfo();
        client_info.query_kind = ClientInfo::QueryKind::INITIAL_QUERY;
        context->setClientInfo(client_info);

        /// Table resolution during analysis requires a real query context (context->getQueryContext() throws
        /// THERE_IS_NO_QUERY otherwise); every real query already has one.
        context->makeQueryContext();

        static constexpr auto database_name = "distributed_object_storage_join_dispatch_test_db";
        static constexpr auto cluster_name = "vig-test";

        DatabasePtr database = std::make_shared<FakeDataLakeDatabase>(database_name, context);

        driver = std::make_shared<FakeDriverStorage>(StorageID(database_name, "driver"), cluster_name);
        database->attachTable(context, "driver", driver, {});

        database->attachTable(context, "safe_lookup", std::make_shared<FakeSafeLookupStorage>(StorageID(database_name, "safe_lookup")), {});
        database->attachTable(context, "dim2", std::make_shared<FakeSafeLookupStorage>(StorageID(database_name, "dim2")), {});

        DatabaseCatalog::instance().attachDatabase(database->getDatabaseName(), database);
        context->setCurrentDatabase(database_name);

        registerUnreachableCluster(context, cluster_name);
    }
};

/// getQueryPlan() returns a reference into the interpreter's own move-only plan, so build+explain in one scope.
String planAndExplain(const String & query, const ContextMutablePtr & context, SelectQueryOptions options = {})
{
    ParserSelectQuery parser;
    ASTPtr ast = parseQuery(parser, query, 1000, 1000, 1000000);
    auto query_tree = buildQueryTree(ast, context);
    QueryTreePassManager pass_manager(context);
    addQueryTreePasses(pass_manager);
    pass_manager.run(query_tree);

    InterpreterSelectQueryAnalyzer interpreter(query_tree, context, options);
    auto & plan = interpreter.getQueryPlan();

    WriteBufferFromOwnString buffer;
    plan.explainPlan(buffer, {.header = true, .description = true, .actions = true});
    return buffer.str();
}

/// Like planAndExplain(), but also runs the query plan optimizer (predicate pushdown included) before
/// explaining -- this is what actually drives ReadFromCluster::applyFilters() during a real EXPLAIN/execution,
/// which planAndExplain() alone never touches. Needed to reproduce a live exception: a WHERE on the
/// driver gets pushed down as a filter onto the whole-query ReadFromCluster step, whose SelectQueryInfo used to
/// carry a mismatched planner_context/table_expression pair for a per-table filter lookup that this step isn't.
String planOptimizeAndExplain(const String & query, const ContextMutablePtr & context)
{
    ParserSelectQuery parser;
    ASTPtr ast = parseQuery(parser, query, 1000, 1000, 1000000);
    auto query_tree = buildQueryTree(ast, context);
    QueryTreePassManager pass_manager(context);
    addQueryTreePasses(pass_manager);
    pass_manager.run(query_tree);

    SelectQueryOptions options;
    InterpreterSelectQueryAnalyzer interpreter(query_tree, context, options);
    auto & plan = interpreter.getQueryPlan();
    plan.optimize(QueryPlanOptimizationSettings(context));

    WriteBufferFromOwnString buffer;
    plan.explainPlan(buffer, {.header = true, .description = true, .actions = true});
    return buffer.str();
}

/// Finds the single ReadFromCluster step in `node`'s subtree, or nullptr.
ReadFromCluster * findReadFromCluster(QueryPlan::Node * node)
{
    if (!node)
        return nullptr;
    if (auto * read_from_cluster = dynamic_cast<ReadFromCluster *>(node->step.get()))
        return read_from_cluster;
    for (auto * child : node->children)
        if (auto * found = findReadFromCluster(child))
            return found;
    return nullptr;
}

ReadFromClusterQuery * findReadFromClusterQuery(QueryPlan::Node * node)
{
    if (!node)
        return nullptr;
    if (auto * read_from_cluster_query = dynamic_cast<ReadFromClusterQuery *>(node->step.get()))
        return read_from_cluster_query;
    for (auto * child : node->children)
        if (auto * found = findReadFromClusterQuery(child))
            return found;
    return nullptr;
}

size_t countOccurrences(const String & text, const String & what)
{
    size_t count = 0;
    for (size_t pos = text.find(what); pos != String::npos; pos = text.find(what, pos + 1))
        ++count;
    return count;
}

/// The query a worker receives names no catalog table: the driver arrives as its storage's `*Cluster` table
/// function and each of the `partners` other tables as its plain table function.
void expectEveryTableSentAsATableFunction(const String & plan_text, size_t partners)
{
    const auto query_begin = plan_text.find("Query: ");
    ASSERT_NE(query_begin, String::npos) << plan_text;
    const String query_sent = plan_text.substr(query_begin, plan_text.find('\n', query_begin) - query_begin);

    EXPECT_EQ(countOccurrences(query_sent, "driverClusterFunction("), 1u) << query_sent;
    EXPECT_EQ(countOccurrences(query_sent, "lookupTableFunction("), partners) << query_sent;
    EXPECT_EQ(countOccurrences(query_sent, "driverTableFunction("), 0u) << query_sent;
    EXPECT_EQ(countOccurrences(query_sent, "lookupClusterFunction("), 0u) << query_sent;
    EXPECT_EQ(countOccurrences(query_sent, "distributed_object_storage_join_dispatch_test_db."), 0u)
        << "a catalog table was sent by name: " << query_sent;
}

}

/// Core case: the driver is the JOIN's leftmost table, and the whole query dispatches as one
/// ReadFromCluster step with no local JOIN.
TEST(DistributedObjectStorageJoinDispatch, DriverOwnsWholeJoinWhenModeIsDistributed)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain("SELECT driver.id, safe_lookup.lookup_id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id", state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_NE(plan_text.find("INNER JOIN"), String::npos) << "expected the whole JOIN forwarded in ReadFromCluster's query, got:\n" << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
}

/// Every table in the dispatched query crosses the wire as the table function its storage sends for its own
/// cluster read. The driver becomes the `*Cluster` function, which on a worker reads the initiator's file-task
/// queue; every other table becomes the plain function, which reads all of its files. A worker resolves no
/// catalog table, so nothing has to tell it which table is the driver.
TEST(DistributedObjectStorageJoinDispatch, EveryTableCrossesTheWireAsATableFunction)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT driver.id, safe_lookup.lookup_id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id", state.context);

    ASSERT_NE(plan_text.find("ReadFromClusterQuery"), String::npos) << plan_text;
    expectEveryTableSentAsATableFunction(plan_text, /*partners=*/1);
}

/// Default mode ('allow'): unaffected, JOIN still executes locally.
TEST(DistributedObjectStorageJoinDispatch, JoinRunsLocallyWhenModeIsNotDistributed)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("allow"));

    auto plan_text = planAndExplain("SELECT driver.id, safe_lookup.lookup_id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id", state.context);

    EXPECT_NE(plan_text.find("JoinLogical"), String::npos) << "expected a local JOIN step, got:\n" << plan_text;
}

/// Driver buried in a subquery: the outer JOIN and GROUP BY still dispatch as one ReadFromCluster,
/// with stock MergingAggregated finalization reused on top.
TEST(DistributedObjectStorageJoinDispatch, BuriedDriverOwnsWholeOuterQueryWithGroupBy)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT x.id, count() FROM "
        "(SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id) AS x "
        "INNER JOIN dim2 ON x.id = dim2.lookup_id "
        "GROUP BY x.id",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_NE(plan_text.find("INNER JOIN"), String::npos) << "expected both JOINs forwarded in ReadFromCluster's query, got:\n" << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
    EXPECT_NE(plan_text.find("MergingAggregated"), String::npos)
        << "expected stock final-merge aggregation on top of the dispatched read, got:\n" << plan_text;
}

/// The ReadFromCluster header must reflect the WithMergeableState stage (unmerged aggregate states), not the
/// query's final projection types -- count() is UInt64 only after MergingAggregated, AggregateFunction(count)
/// beforehand. A header built from the final projection instead would declare the wrong type here, undetected
/// by plan structure alone since ReadFromCluster's header is never validated against nothing at plan time.
TEST(DistributedObjectStorageJoinDispatch, ReadFromClusterHeaderCarriesUnmergedAggregateState)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT x.id, count() FROM "
        "(SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id) AS x "
        "INNER JOIN dim2 ON x.id = dim2.lookup_id "
        "GROUP BY x.id",
        state.context);

    auto read_from_cluster_pos = plan_text.find("ReadFromCluster");
    ASSERT_NE(read_from_cluster_pos, String::npos) << plan_text;
    auto merging_aggregated_pos = plan_text.find("MergingAggregated");
    ASSERT_NE(merging_aggregated_pos, String::npos) << plan_text;

    /// explainPlan() prints children before parents, so ReadFromCluster's own header block sits between the
    /// two step names.
    auto read_from_cluster_block = plan_text.substr(read_from_cluster_pos, merging_aggregated_pos - read_from_cluster_pos);
    EXPECT_NE(read_from_cluster_block.find("AggregateFunction(count"), String::npos)
        << "expected ReadFromCluster's header to carry the unmerged aggregate state type, got:\n" << read_from_cluster_block;
}

/// A realistic projection: a CASE expression reading columns from both sides of the JOIN, plus count(),
/// GROUP BY, ORDER BY and LIMIT all on the dispatch boundary itself. Exercises the header/rename machinery
/// with more than one plain passthrough column, unlike the trivial single-column projections above.
TEST(DistributedObjectStorageJoinDispatch, ComplexProjectionOverBothJoinSidesBuildsSuccessfully)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT "
        "  CASE WHEN safe_lookup.lookup_id > 0 THEN driver.id ELSE safe_lookup.lookup_id END AS dst_city, "
        "  count() AS c "
        "FROM driver LEFT JOIN safe_lookup ON driver.id = safe_lookup.lookup_id "
        "GROUP BY dst_city "
        "ORDER BY dst_city "
        "LIMIT 10",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_NE(plan_text.find("LEFT JOIN"), String::npos) << "expected the whole JOIN forwarded in ReadFromCluster's query, got:\n" << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
    EXPECT_NE(plan_text.find("MergingAggregated"), String::npos)
        << "expected stock final-merge aggregation on top of the dispatched read, got:\n" << plan_text;
}

/// Regression for a live exception: a WHERE on the driver survives real plan optimization (not just
/// candidate discovery/plan construction), which pushes it down as a filter onto the whole-query
/// ReadFromCluster step and calls ReadFromCluster::applyFilters() -> SourceStepWithFilter::applyFilters() ->
/// SelectQueryInfo::buildNodeNameToInputNodeColumn(), which looks up a per-table `table_expression` in a
/// `planner_context` that here describes the *whole* dispatched query -- it throws, and used to dereference a
/// null table_expression while formatting that very error. In whole-query mode applyFilters must therefore use
/// SourceStepWithFilterBase::applyFilters, which does not build that per-table mapping at all.
TEST(DistributedObjectStorageJoinDispatch, ComplexProjectionWithWhereSurvivesPlanOptimization)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planOptimizeAndExplain(
        "SELECT "
        "  CASE WHEN safe_lookup.lookup_id > 0 THEN driver.id ELSE safe_lookup.lookup_id END AS dst_city, "
        "  count() AS c "
        "FROM driver LEFT JOIN safe_lookup ON driver.id = safe_lookup.lookup_id "
        "WHERE driver.id > 0 "
        "GROUP BY dst_city "
        "ORDER BY dst_city "
        "LIMIT 10",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
}

/// The full buried-driver shape: driver behind one intermediate subquery, outer LEFT JOIN against a RHS subquery that
/// itself LEFT JOINs a further GROUP BY subquery and also has its own GROUP BY. None of that RHS content is a
/// competing driver -- it's recomputed whole on every worker -- and the whole thing must still build into a
/// single dispatched ReadFromCluster with stock finalization for the outer GROUP BY on top.
TEST(DistributedObjectStorageJoinDispatch, BuriedDriverWithNestedRightSideBuildsSuccessfully)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT transaction_event.id, count() FROM "
        "(SELECT driver.id FROM driver) AS transaction_event "
        "LEFT JOIN "
        "(SELECT safe_lookup.lookup_id AS id FROM safe_lookup LEFT JOIN "
        "(SELECT dim2.lookup_id AS id FROM dim2 GROUP BY dim2.lookup_id) AS policy_matches "
        "ON safe_lookup.lookup_id = policy_matches.id GROUP BY safe_lookup.lookup_id) AS alert_events "
        "ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
    EXPECT_NE(plan_text.find("MergingAggregated"), String::npos)
        << "expected stock final-merge aggregation on top of the dispatched read, got:\n" << plan_text;
}

/// The same shape: the driver (`driver`, buried inside `transaction_event`) is sent as the cluster function and
/// every other table reachable from the right side (`safe_lookup`, `dim2`) as a plain one.
TEST(DistributedObjectStorageJoinDispatch, BuriedDriverIsTheOnlyClusterFunctionSent)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT transaction_event.id, count() FROM "
        "(SELECT driver.id FROM driver) AS transaction_event "
        "LEFT JOIN "
        "(SELECT safe_lookup.lookup_id AS id FROM safe_lookup LEFT JOIN "
        "(SELECT dim2.lookup_id AS id FROM dim2 GROUP BY dim2.lookup_id) AS policy_matches "
        "ON safe_lookup.lookup_id = policy_matches.id GROUP BY safe_lookup.lookup_id) AS alert_events "
        "ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id",
        state.context);

    ASSERT_NE(plan_text.find("ReadFromClusterQuery"), String::npos) << plan_text;
    expectEveryTableSentAsATableFunction(plan_text, /*partners=*/2);
}

/// The same shape written with CTEs rather than derived tables. The setting documents CTE support, and the
/// analyzer resolves a CTE into a QueryNode just as it does a derived table -- but a CTE reference serializes to
/// its bare name unless the body is inlined, which would not resolve on a worker.
/// queryNodeToDistributedSelectQuery is what inlines it; this pins that down, including for a CTE
/// (`policy_matches`) referenced from inside another CTE.
TEST(DistributedObjectStorageJoinDispatch, BuriedDriverWithCommonTableExpressions)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "WITH transaction_event AS (SELECT driver.id FROM driver), "
        "policy_matches AS (SELECT dim2.lookup_id AS id FROM dim2 GROUP BY dim2.lookup_id), "
        "alert_events AS (SELECT safe_lookup.lookup_id AS id FROM safe_lookup "
        "LEFT JOIN policy_matches ON safe_lookup.lookup_id = policy_matches.id "
        "GROUP BY safe_lookup.lookup_id) "
        "SELECT transaction_event.id, count() FROM transaction_event "
        "LEFT JOIN alert_events ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_EQ(plan_text.find("JoinLogical"), String::npos) << "expected no local JOIN step, got:\n" << plan_text;
    EXPECT_NE(plan_text.find("MergingAggregated"), String::npos)
        << "expected stock final-merge aggregation on top of the dispatched read, got:\n" << plan_text;

    /// No dangling CTE name: every CTE body appears inlined in the forwarded query, each table in it as a table
    /// function.
    expectEveryTableSentAsATableFunction(plan_text, /*partners=*/2);
}

/// Whole-query dispatch gets its own source step rather than reusing the single-table one. That is what makes
/// a whole class of mismatch unrepresentable: ReadFromCluster carries a StorageSnapshot and required columns
/// describing one table, which for a dispatched query would describe the driver while the step's output is the
/// query's result. ReadFromClusterQuery has neither, so there is nothing to disagree.
TEST(DistributedObjectStorageJoinDispatch, DispatchUsesItsOwnSourceStepNotATableRead)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    ParserSelectQuery parser;
    String query =
        "SELECT "
        "  CASE WHEN safe_lookup.lookup_id > 0 THEN driver.id ELSE safe_lookup.lookup_id END AS dst_city, "
        "  count() AS c "
        "FROM driver LEFT JOIN safe_lookup ON driver.id = safe_lookup.lookup_id "
        "GROUP BY dst_city";
    ASTPtr ast = parseQuery(parser, query, 1000, 1000, 1000000);
    auto query_tree = buildQueryTree(ast, state.context);
    QueryTreePassManager pass_manager(state.context);
    addQueryTreePasses(pass_manager);
    pass_manager.run(query_tree);

    SelectQueryOptions options;
    InterpreterSelectQueryAnalyzer interpreter(query_tree, state.context, options);
    auto & plan = interpreter.getQueryPlan();

    EXPECT_NE(findReadFromClusterQuery(plan.getRootNode()), nullptr)
        << "expected the dispatch to use its own source step";
    EXPECT_EQ(findReadFromCluster(plan.getRootNode()), nullptr)
        << "expected no single-table cluster read: the dispatched query is not one table's rows";
}

/// The mode can also arrive via a query-level SETTINGS clause rather than context->setSetting(); the header's
/// hook-disabling context copy (Context::createCopy(context)) must still see it and disable the hook for the
/// sample-block analysis, or this recurses instead of the ambient session-level context not mattering here.
TEST(DistributedObjectStorageJoinDispatch, HeaderComputationHandlesQueryLevelModeSetting)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("allow"));

    auto plan_text = planAndExplain(
        "SELECT driver.id, count() FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id "
        "GROUP BY driver.id SETTINGS object_storage_cluster_join_mode = 'distributed'",
        state.context);

    EXPECT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;
    EXPECT_NE(plan_text.find("AggregateFunction(count"), String::npos)
        << "expected header computation to still see WithMergeableState types under a query-level SETTINGS override, got:\n" << plan_text;
}

/// A driver referenced twice would read the initiator's file-task queue twice, so the query is planned ordinarily.
TEST(DistributedObjectStorageJoinDispatch, SelfJoinOnTheDriverIsNotDispatched)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain("SELECT a.id FROM driver AS a INNER JOIN driver AS b ON a.id = b.id", state.context);
    EXPECT_EQ(plan_text.find("ReadFromClusterQuery"), String::npos) << plan_text;
}

/// A plan built for a shard is serialized and sent, and `ReadFromClusterQuery` has no serialized form.
TEST(DistributedObjectStorageJoinDispatch, LogicalPlanForShardIsNotDispatched)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    SelectQueryOptions options;
    options.build_logical_plan = true;
    auto plan_text = planAndExplain(
        "SELECT driver.id, safe_lookup.lookup_id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id",
        state.context,
        options);
    EXPECT_EQ(plan_text.find("ReadFromClusterQuery"), String::npos) << plan_text;
}

/// Initiator-only settings such as `limit` shape the final result. A worker applying them would cut its partial result.
TEST(DistributedObjectStorageJoinDispatch, InitiatorOnlySettingsAreNotSentToWorkers)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));
    auto context = Context::createCopy(state.context);
    context->setSetting("limit", UInt64(1));

    ParserSelectQuery parser;
    ASTPtr ast = parseQuery(
        parser, "SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id", 1000, 1000, 1000000);
    auto query_tree = buildQueryTree(ast, context);
    QueryTreePassManager pass_manager(context);
    addQueryTreePasses(pass_manager);
    pass_manager.run(query_tree);

    InterpreterSelectQueryAnalyzer interpreter(query_tree, context, SelectQueryOptions{});
    auto * read_from_cluster_query = findReadFromClusterQuery(interpreter.getQueryPlan().getRootNode());
    ASSERT_NE(read_from_cluster_query, nullptr);

    EXPECT_FALSE(read_from_cluster_query->updateSettings()->getSettingsRef().isChanged("limit"));
}
