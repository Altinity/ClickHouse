#include <gtest/gtest.h>

#include <fmt/format.h>

#include <Access/AccessControl.h>
#include <Access/Common/RowPolicyDefs.h>
#include <Access/RowPolicy.h>
#include <Access/User.h>
#include <Analyzer/JoinNode.h>
#include <Analyzer/QueryNode.h>
#include <Analyzer/QueryTreeBuilder.h>
#include <Analyzer/QueryTreePassManager.h>
#include <Analyzer/TableFunctionNode.h>
#include <Analyzer/TableNode.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <Core/NamesAndTypes.h>
#include <DataTypes/DataTypesNumber.h>
#include <Databases/DatabaseMemory.h>
#include <Databases/DatabasesCommon.h>
#include <Interpreters/Context.h>
#include <Interpreters/DatabaseCatalog.h>
#include <Parsers/ParserSelectQuery.h>
#include <Parsers/parseQuery.h>
#include <Planner/findDistributedObjectStorageCandidate.h>
#include <Storages/IStorageCluster.h>
#include <Storages/MemorySettings.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/StorageMemory.h>

using namespace DB;

namespace
{

NamesAndTypesList testColumns()
{
    return {{"id", std::make_shared<DataTypeUInt64>()}};
}

/// A DatabaseMemory that reports itself as a DataLake catalog, which is what makes the tables inside it
/// eligible (findDistributedObjectStorageCandidate asks DatabaseCatalog::isDatalakeCatalog).
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

/// Minimal IStorageCluster test double, configurable per instance.
class FakeClusterStorage : public IStorageCluster
{
public:
    FakeClusterStorage(const StorageID & table_id, String cluster_name_)
        : IStorageCluster(cluster_name_, table_id, getLogger("test"))
    {
        StorageInMemoryMetadata metadata;
        metadata.setColumns(ColumnsDescription{testColumns()});
        setInMemoryMetadata(metadata);
    }

    std::string getName() const override { return "FakeClusterStorage"; }

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node *, const ActionsDAG *, const ContextPtr &, ClusterPtr, StorageMetadataPtr) const override
    {
        return {};
    }
};

/// Modelled on src/Planner/tests/gtest_planner_empty_projection.cpp.
struct State
{
    State(const State &) = delete;

    ContextMutablePtr context;

    static const State & instance()
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

        static constexpr auto database_name = "find_distributed_object_storage_candidate_test_db";
        DatabasePtr database = std::make_shared<FakeDataLakeDatabase>(database_name, context);

        auto attach_cluster_table = [&](const DatabasePtr & db, const String & table_name, String cluster_name)
        {
            db->attachTable(
                context,
                table_name,
                std::make_shared<FakeClusterStorage>(StorageID(db->getDatabaseName(), table_name), std::move(cluster_name)),
                {});
        };

        /// A distributed driver, e.g. ice.event_page.
        attach_cluster_table(database, "driver", "vig-test");
        /// A second DataLake-catalog table under the same cluster -- never an independent driver on the
        /// right of a JOIN, just a plain (if wasteful) safe partner.
        attach_cluster_table(database, "second_datalake_table", "vig-test");
        /// A safe co-resolved table with no cluster dispatch of its own (e.g. ice.geo_location_lookup).
        attach_cluster_table(database, "safe_lookup", "");

        /// A Cluster-engine table in an ordinary database -- not resolved through a DataLake catalog, so not
        /// safe to re-resolve on a worker.
        static constexpr auto plain_database_name = "find_distributed_object_storage_candidate_test_plain_db";
        DatabasePtr plain_database = std::make_shared<DatabaseMemory>(plain_database_name, context);
        attach_cluster_table(plain_database, "unsafe_cluster_table", "some-cluster");
        DatabaseCatalog::instance().attachDatabase(plain_database->getDatabaseName(), plain_database);

        database->attachTable(
            context,
            "local_table",
            std::make_shared<StorageMemory>(
                StorageID(database_name, "local_table"), ColumnsDescription{testColumns()}, ConstraintsDescription{}, String{}, MemorySettings{}),
            {});

        DatabaseCatalog::instance().attachDatabase(database->getDatabaseName(), database);
        context->setCurrentDatabase(database_name);
    }
};

QueryTreeNodePtr analyze(const String & query, const ContextMutablePtr & context)
{
    ParserSelectQuery parser;
    ASTPtr ast = parseQuery(parser, query, 1000, 1000, 1000000);
    auto query_tree = buildQueryTree(ast, context);
    QueryTreePassManager pass_manager(context);
    addQueryTreePasses(pass_manager);
    pass_manager.run(query_tree);
    return query_tree;
}

/// AccessControl is shared across every Context::createCopy() of the same test-global context, so entity
/// names must be unique per call, not just per helper, or a second test's insert() throws "already exists".
size_t nextTestAccessEntitySuffix()
{
    static std::atomic<size_t> counter{0};
    return counter++;
}

/// A context copy whose user has no grants at all -- Context::getAccess() gives the global context (no
/// setUserID() call) full access unconditionally, so a real, minimally-privileged user is needed to exercise
/// the access-denied path at all.
ContextMutablePtr contextWithNoGrants(const ContextMutablePtr & base_context)
{
    auto context = Context::createCopy(base_context);
    context->getAccessControl().addMemoryStorage("find_distributed_object_storage_candidate_test_storage", /*allow_backup_*/ false);
    auto user = std::make_shared<User>();
    user->setName(fmt::format("find_distributed_object_storage_candidate_test_no_grants_user_{}", nextTestAccessEntitySuffix()));
    auto user_id = context->getAccessControl().insert(user);
    context->setUser(user_id);
    return context;
}

/// A context copy whose user has full access, but a nontrivial row policy applies to `table_name`.
ContextMutablePtr contextWithRowPolicy(const ContextMutablePtr & base_context, const String & database_name, const String & table_name)
{
    auto context = Context::createCopy(base_context);
    context->getAccessControl().addMemoryStorage("find_distributed_object_storage_candidate_test_storage", /*allow_backup_*/ false);
    auto suffix = nextTestAccessEntitySuffix();

    auto user = std::make_shared<User>();
    user->setName(fmt::format("find_distributed_object_storage_candidate_test_row_policy_user_{}", suffix));
    user->access.grant(AccessType::ALL);
    auto user_id = context->getAccessControl().insert(user);

    auto policy = std::make_shared<RowPolicy>();
    policy->setFullName(fmt::format("find_distributed_object_storage_candidate_test_policy_{}", suffix), database_name, table_name);
    policy->filters[static_cast<size_t>(RowPolicyFilterType::SELECT_FILTER)] = "id != -1";
    policy->to_roles = RolesOrUsersSet(user_id);
    context->getAccessControl().insert(policy);

    context->setUser(user_id);
    return context;
}

}

TEST(FindDistributedObjectStorageCandidate, AcceptsDriverJoinedWithSafeDataLakeTable)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    ASSERT_NE(candidate->driver, nullptr);
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

TEST(FindDistributedObjectStorageCandidate, DoesNotInterceptPlainDistributedObjectStorageQuery)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsOrdinaryLocalTableAsRhs)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN local_table ON driver.id = local_table.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsWhenModeIsNotDistributed)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("allow"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsWhenRemoteInitiatorIsSet)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));
    state.context->setSetting("object_storage_remote_initiator", true);

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());

    state.context->setSetting("object_storage_remote_initiator", false);
}

/// additional_table_filters matching depends on the initiator's current_database and the query's own
/// aliasing, both of which shift once forwarded as fully serialized remote SQL (the same reasoning
/// Planner.cpp already uses to disable parallel replicas for this combination). Rejected as a blanket
/// disablement whenever the setting is nonempty at all, regardless of which table it names.
TEST(FindDistributedObjectStorageCandidate, RejectsWhenAdditionalTableFiltersIsSet)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));
    state.context->setSetting("additional_table_filters", String("{'driver': 'id > 0'}"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());

    state.context->setSetting("additional_table_filters", String(""));
}

/// Ordinary per-table planning checks SELECT access on every table it plans (checkAccessRights() in
/// PlannerJoinTree.cpp); this whole-query dispatch replaces that walk entirely, so it must perform the
/// same check itself for the driver and every JOIN partner, or a user without SELECT could gain access to
/// the driver's data simply by enabling 'distributed' mode.
TEST(FindDistributedObjectStorageCandidate, RejectsDriverWithoutSelectAccess)
{
    const auto & state = State::instance();
    auto context = contextWithNoGrants(state.context);
    context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, context).has_value());
}

/// Same access check, but the table without SELECT access is buried inside a subquery rather
/// than at the dispatch boundary's own top-level JOIN -- the recursive intermediate-subquery walk must reach
/// it too, not just the tables directly visible at the outermost level.
TEST(FindDistributedObjectStorageCandidate, RejectsBuriedDriverWithoutSelectAccess)
{
    const auto & state = State::instance();
    auto context = contextWithNoGrants(state.context);
    context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM (SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id) AS x "
        "INNER JOIN second_datalake_table ON x.id = second_datalake_table.id",
        context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, context).has_value());
}

/// A row policy on the driver itself: a worker re-resolves the driver from its own catalog and the initiator's
/// policy does not travel with the query text, so it could never be reapplied there.
TEST(FindDistributedObjectStorageCandidate, RejectsDriverWithRowPolicy)
{
    const auto & state = State::instance();
    auto context = contextWithRowPolicy(state.context, "find_distributed_object_storage_candidate_test_db", "driver");
    context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, context).has_value());
}

/// A row policy on a non-driver JOIN partner: unlike the driver, this table stays a plain catalog identifier
/// and is independently re-resolved by DatabaseDataLake on each worker -- there is no cheap way to prove that
/// re-resolution applies the same effective policy as the initiator's own user, so it must conservatively
/// block whole-query dispatch too, not just a policy on the driver.
TEST(FindDistributedObjectStorageCandidate, RejectsNonDriverPartnerWithRowPolicy)
{
    const auto & state = State::instance();
    auto context = contextWithRowPolicy(state.context, "find_distributed_object_storage_candidate_test_db", "safe_lookup");
    context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, AcceptsSecondDataLakeTableAsRhsEvenThoughItIsAlsoDispatchCapable)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree
        = analyze("SELECT driver.id FROM driver INNER JOIN second_datalake_table ON driver.id = second_datalake_table.id", state.context);
    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

/// A second driver-capable table sitting in its own subquery on the right of the outer JOIN is never inspected
/// as a competing driver at all -- it's just worker-local content, safe because it's a DataLake-catalog table,
/// same as AcceptsSecondDataLakeTableAsRhsEvenThoughItIsAlsoDispatchCapable above but one level deeper.
TEST(FindDistributedObjectStorageCandidate, AcceptsSecondDriverCapableSubqueryAsRhs)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM (SELECT driver.id FROM driver) AS x "
        "INNER JOIN (SELECT second_datalake_table.id FROM second_datalake_table) AS y ON x.id = y.id",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

TEST(FindDistributedObjectStorageCandidate, RejectsUnsafeClusterTableAsDriver)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree
        = analyze("SELECT t.id FROM find_distributed_object_storage_candidate_test_plain_db.unsafe_cluster_table AS t INNER JOIN safe_lookup ON t.id = safe_lookup.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsUnsafeIStorageClusterTableAsRhs)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree
        = analyze("SELECT driver.id FROM driver INNER JOIN find_distributed_object_storage_candidate_test_plain_db.unsafe_cluster_table AS t ON driver.id = t.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

/// Nested JoinNodes within one QueryNode (chained JOINs) must be fully understood, not just one JOIN.
TEST(FindDistributedObjectStorageCandidate, AcceptsDriverInMultiJoinQueryNode)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT driver.id FROM driver "
        "INNER JOIN safe_lookup ON driver.id = safe_lookup.id "
        "INNER JOIN second_datalake_table ON driver.id = second_datalake_table.id",
        state.context);
    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

/// A GROUP BY inside the intermediate subquery `x` (between the dispatch boundary and the driver, on the
/// driver's own left path) would be executed independently per worker partition if the whole query were
/// dispatched -- unsafe, since a group spanning multiple workers' partitions would never get merged. No
/// fallback: the whole query is simply not a candidate; stock planning handles it (which plans `x`'s own
/// GROUP BY correctly, as the boundary of its own ordinary subquery plan).
TEST(FindDistributedObjectStorageCandidate, RejectsWhenDriverPathIntermediateSubqueryHasGroupBy)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id, x.c FROM "
        "(SELECT driver.id, count() AS c FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id GROUP BY driver.id) AS x "
        "INNER JOIN second_datalake_table ON x.id = second_datalake_table.id",
        state.context);

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

/// Same hazard as GROUP BY: a LIMIT inside a driver-path intermediate subquery would apply per worker
/// partition, not globally, if the whole query were dispatched. No fallback.
TEST(FindDistributedObjectStorageCandidate, RejectsWhenDriverPathIntermediateSubqueryHasLimit)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM "
        "(SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id LIMIT 10) AS x "
        "INNER JOIN second_datalake_table ON x.id = second_datalake_table.id",
        state.context);

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

/// A plain WHERE/projection-only intermediate subquery is partition-preserving (each row stays independent),
/// so the outer query is still the accepted dispatch boundary, same as before this safety check existed.
TEST(FindDistributedObjectStorageCandidate, AcceptsOuterCandidateWhenIntermediateSubqueryIsWhereProjectionOnly)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM "
        "(SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id WHERE driver.id > 0) AS x "
        "INNER JOIN second_datalake_table ON x.id = second_datalake_table.id",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
}

/// The right side of a JOIN is never searched for a driver, no matter what it contains -- so a driver-bearing
/// subquery sitting there simply isn't found; the left side alone decides whether there's a candidate at all
/// (here it doesn't have one, since `safe_lookup` has no cluster of its own). No fallback: rejected outright.
TEST(FindDistributedObjectStorageCandidate, RejectsWhenLeftSpineHasNoDriverEvenThoughRhsSubqueryDoes)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM safe_lookup LEFT JOIN "
        "(SELECT driver.id FROM driver) AS x "
        "ON safe_lookup.id = x.id",
        state.context);

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsUnsafeSubqueryInWhereClause)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id "
        "WHERE driver.id IN (SELECT id FROM local_table)",
        state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsCrossJoin)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver CROSS JOIN safe_lookup", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsDriverOnTheRightOfRightJoin)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM safe_lookup RIGHT JOIN driver ON safe_lookup.id = driver.id", state.context);
    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, RejectsExplicitClusterTableFunctionAsRhs)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze("SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id", state.context);

    /// Build a TableFunctionNode directly -- no real TableFunctionFactory registration needed.
    auto & query_node = query_tree->as<QueryNode &>();
    auto & join_node = query_node.getJoinTree()->as<JoinNode &>();

    auto table_function_node = std::make_shared<TableFunctionNode>("icebergS3Cluster");
    auto explicit_cluster_storage = std::make_shared<FakeClusterStorage>(
        StorageID("system", "explicit_cluster_table"), "other-cluster");
    table_function_node->resolve(nullptr, explicit_cluster_storage, state.context, {});
    join_node.getRightTableExpression() = table_function_node;

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

/// Driver buried in a subquery joined against another safe table; the outer query is the candidate.
TEST(FindDistributedObjectStorageCandidate, AcceptsBuriedDriverWithSafeOuterJoin)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM (SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id) AS x "
        "INNER JOIN second_datalake_table ON x.id = second_datalake_table.id",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

/// Same shape, but the outer JOIN partner is an ordinary local table: whole-query dispatch would have to
/// forward it too (it's part of the same dispatch boundary), which isn't safe -- rejected outright, no
/// fallback to dispatching the inner subquery alone even though it would be safe on its own.
TEST(FindDistributedObjectStorageCandidate, RejectsWhenOuterJoinPartnerIsUnsafe)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT x.id FROM (SELECT driver.id FROM driver INNER JOIN safe_lookup ON driver.id = safe_lookup.id) AS x "
        "INNER JOIN local_table ON x.id = local_table.id",
        state.context);

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

/// The full buried-driver shape: the driver sits behind one intermediate subquery (`transaction_event`, standing in for
/// `txnlog`) on the left of the outer LEFT JOIN; the right side (`alert_events`) is itself a LEFT JOIN against
/// a further subquery with its own GROUP BY (`policy_matches`), and `alert_events` itself also has a GROUP BY.
/// None of that RHS structure is inspected for a competing driver or restricted for GROUP BY/JOIN -- it's
/// worker-local content, recomputed whole on every worker. The outer query's own GROUP BY/ORDER BY/LIMIT are
/// the dispatch boundary's own, handled by stock finalization on top of the dispatched read.
TEST(FindDistributedObjectStorageCandidate, AcceptsBuriedDriverWithNestedRightSide)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT transaction_event.id, count() AS c FROM "
        "(SELECT driver.id FROM driver WHERE driver.id > 0) AS transaction_event "
        "LEFT JOIN "
        "(SELECT safe_lookup.id FROM safe_lookup LEFT JOIN "
        "(SELECT second_datalake_table.id FROM second_datalake_table GROUP BY second_datalake_table.id) AS policy_matches "
        "ON safe_lookup.id = policy_matches.id GROUP BY safe_lookup.id) AS alert_events "
        "ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id "
        "ORDER BY transaction_event.id "
        "LIMIT 10",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}

/// Direct-driver shape: one root LEFT JOIN between the driver and a safe lookup table, with WHERE, GROUP BY,
/// ORDER BY and LIMIT all sitting directly on the dispatch boundary itself (not an intermediate subquery) --
/// freely allowed there, unlike on a driver-path intermediate subquery.
/// The buried-driver shape expressed with CTEs: the driver sits inside a CTE used as the JOIN's left side. The analyzer
/// resolves a CTE reference into the same QueryNode a derived table would produce, so the left-spine walk must
/// find the driver through it exactly as it does through a subquery.
TEST(FindDistributedObjectStorageCandidate, AcceptsBuriedDriverInsideCommonTableExpression)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "WITH transaction_event AS (SELECT driver.id FROM driver), "
        "alert_events AS (SELECT safe_lookup.id FROM safe_lookup) "
        "SELECT transaction_event.id, count() FROM transaction_event "
        "LEFT JOIN alert_events ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().getTableName(), "driver");
}

/// A CTE on the driver's own left path is still an intermediate subquery: if it aggregates, each worker would
/// finalize its own slice of the driver as though it were the whole group.
TEST(FindDistributedObjectStorageCandidate, RejectsAggregatingCommonTableExpressionOnDriverPath)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "WITH transaction_event AS (SELECT driver.id FROM driver GROUP BY driver.id), "
        "alert_events AS (SELECT safe_lookup.id FROM safe_lookup) "
        "SELECT transaction_event.id, count() FROM transaction_event "
        "LEFT JOIN alert_events ON transaction_event.id = alert_events.id "
        "GROUP BY transaction_event.id",
        state.context);

    EXPECT_FALSE(findDistributedObjectStorageCandidate(query_tree, state.context).has_value());
}

TEST(FindDistributedObjectStorageCandidate, AcceptsDirectDriverWithFilterAndAggregation)
{
    const auto & state = State::instance();
    state.context->setSetting("object_storage_cluster_join_mode", String("distributed"));

    auto query_tree = analyze(
        "SELECT driver.id, count() FROM driver LEFT JOIN safe_lookup ON driver.id = safe_lookup.id "
        "WHERE driver.id > 0 GROUP BY driver.id ORDER BY driver.id LIMIT 10",
        state.context);

    auto candidate = findDistributedObjectStorageCandidate(query_tree, state.context);
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->driver->getStorageID().table_name, "driver");
}
