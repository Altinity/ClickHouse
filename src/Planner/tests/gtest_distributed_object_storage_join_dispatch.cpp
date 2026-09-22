#include <sstream>

#include <gtest/gtest.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryTreePassManager.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <Core/NamesAndTypes.h>
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

/// The builder resolves its driver replacement via QueryAnalysisPass, which -- like any table function
/// reference -- looks `fakeDriverFunction` up in the real TableFunctionFactory (QueryAnalyzer::resolveTableFunction()).
/// Registers a minimal stand-in that just returns the test's own driver storage, so the resolved
/// TableFunctionNode carries the same columns real production code would get back from e.g. icebergS3Cluster().
/// executeImpl() is defined out-of-line, after State, since it needs State to be a complete type.
class FakeDriverTableFunction : public ITableFunction
{
public:
    static constexpr auto name = "fakeDriverFunction";
    std::string getName() const override { return name; }
    bool hasStaticStructure() const override { return true; }
    ColumnsDescription getActualTableStructure(ContextPtr, bool) const override { return ColumnsDescription{driverColumns()}; }

protected:
    /// The default implementation looks getStorageEngineName() up in StorageFactory for source-access checking,
    /// which "FakeDriverStorage" (a test-only stand-in, never registered there) doesn't have.
    std::optional<AccessTypeObjects::Source> getSourceAccessObject() const override { return std::nullopt; }

private:
    StoragePtr executeImpl(const ASTPtr &, ContextPtr, const std::string &, ColumnsDescription, bool) const override;
    const char * getStorageEngineName() const override { return "FakeDriverStorage"; }
};

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
    /// Mirrors StorageObjectStorageCluster::updateQueryForDistributedEngineIfNeeded()'s alias handling
    /// closely enough to exercise buildDistributedObjectStorageQueryPlan.cpp's own fallback-alias fix:
    /// transfers whatever alias the driver's table identifier already had (empty if none) onto the
    /// replacement table function, exactly like the real rewrite.
    void updateQueryToSendIfNeeded(ASTPtr & query, const StorageSnapshotPtr &, const ContextPtr &, bool make_cluster_function) override
    {
        if (!make_cluster_function)
            return;

        auto * select_query = query->as<ASTSelectQuery>();
        if (!select_query || !select_query->tables())
            return;

        auto * tables = select_query->tables()->as<ASTTablesInSelectQuery>();
        auto * table_expression = tables->children.at(0)->as<ASTTablesInSelectQueryElement>()->table_expression->as<ASTTableExpression>();
        if (!table_expression || !table_expression->database_and_table_name)
            return;

        auto table_alias = table_expression->database_and_table_name->tryGetAlias();
        auto function_ast = makeASTFunction("fakeDriverFunction");
        function_ast->setAlias(table_alias);

        table_expression->database_and_table_name = nullptr;
        table_expression->table_function = function_ast;
        table_expression->children[0] = function_ast;
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

        /// The driver rewrite resolves its replacement TableFunctionNode via QueryAnalysisPass, which (like any
        /// table function resolution -- QueryAnalyzer::resolveTableFunction()) requires a real query context
        /// (context->getQueryContext() throws THERE_IS_NO_QUERY otherwise); every real query already has one.
        context->makeQueryContext();

        TableFunctionFactory::instance().registerFunction<FakeDriverTableFunction>(FunctionDocumentation{});

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

StoragePtr FakeDriverTableFunction::executeImpl(const ASTPtr &, ContextPtr, const std::string &, ColumnsDescription, bool) const
{
    return State::instance().driver;
}

/// getQueryPlan() returns a reference into the interpreter's own move-only plan, so build+explain in one scope.
String planAndExplain(const String & query, const ContextMutablePtr & context)
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

/// Nothing in the dispatched query is rewritten: the driver crosses the wire as the catalog table the user
/// wrote, and which table drives the dispatch travels separately, in
/// `object_storage_distributed_driver_database`/`_table`. That is only unambiguous while the driver is named
/// once, which the planner checks on the serialized query -- so pin both halves here: no cluster function
/// appears, and the driver's own name appears exactly once.
TEST(DistributedObjectStorageJoinDispatch, DriverCrossesTheWireUnrewrittenAndNamedOnce)
{
    auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto plan_text = planAndExplain(
        "SELECT driver.id, safe_lookup.lookup_id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.lookup_id", state.context);

    ASSERT_NE(plan_text.find("ReadFromCluster"), String::npos) << plan_text;

    EXPECT_EQ(plan_text.find("fakeDriverFunction("), String::npos)
        << "expected the driver to stay an ordinary catalog table, not be rewritten to a cluster function, got:\n" << plan_text;

    size_t driver_mentions = 0;
    for (size_t pos = plan_text.find("driver"); pos != String::npos; pos = plan_text.find("driver", pos + 1))
        ++driver_mentions;
    EXPECT_GE(driver_mentions, 1u) << "expected the driver to be named in the dispatched query, got:\n" << plan_text;
}

/// Default mode ('allow'): unaffected, JOIN still executes locally.
TEST(DistributedObjectStorageJoinDispatch, DriverIsWrappedWhenModeIsNotDistributed)
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

/// The same shape, asserting that dispatch rewrites nothing: the driver (`driver`, buried inside
/// `transaction_event`) and every other DataLake table reachable from the RHS (`safe_lookup`, `dim2`) all cross
/// the wire as ordinary catalog identifiers. Only the announced driver reads the initiator's file-task queue.
TEST(DistributedObjectStorageJoinDispatch, DispatchesBuriedDriverWithoutRewritingAnyTable)
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

    EXPECT_EQ(plan_text.find("fakeDriverFunction("), String::npos)
        << "expected no table to be rewritten into a cluster function, got:\n" << plan_text;

    EXPECT_NE(plan_text.find("safe_lookup"), String::npos) << "expected safe_lookup to remain an ordinary catalog identifier, got:\n" << plan_text;
    EXPECT_NE(plan_text.find("dim2"), String::npos) << "expected dim2 to remain an ordinary catalog identifier, got:\n" << plan_text;
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

    /// No dangling CTE name: every CTE body must appear inlined in the forwarded query, and nothing is rewritten.
    EXPECT_EQ(plan_text.find("fakeDriverFunction("), String::npos)
        << "expected no table to be rewritten into a cluster function, got:\n" << plan_text;

    EXPECT_NE(plan_text.find("safe_lookup"), String::npos) << plan_text;
    EXPECT_NE(plan_text.find("dim2"), String::npos) << plan_text;
}

/// SourceStepWithFilter::required_source_columns is checked against the driver's own StorageSnapshot
/// (updatePrewhereInfo() calls storage_snapshot->getSampleBlockForColumns(required_source_columns)) -- it must
/// be the driver's physical columns, never the whole dispatched query's own output projection (that's a
/// separate concept, carried by ReadFromCluster's header/sample_block instead).
TEST(DistributedObjectStorageJoinDispatch, RequiredSourceColumnsAreDriverColumnsNotOuterProjection)
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

    auto * read_from_cluster = findReadFromCluster(plan.getRootNode());
    ASSERT_NE(read_from_cluster, nullptr);
    EXPECT_EQ(read_from_cluster->requiredSourceColumns(), Names{"id"})
        << "expected the driver's own physical columns, not the outer query's projection (dst_city, c)";
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
