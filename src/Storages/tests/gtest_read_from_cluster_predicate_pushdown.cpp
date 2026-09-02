#include <set>
#include <sstream>

#include <gtest/gtest.h>

#include <Poco/AutoPtr.h>
#include <Poco/Util/XMLConfiguration.h>

#include <Core/Block.h>
#include <Core/NamesAndTypes.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/ActionsDAG.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTSelectQuery.h>
#include <Processors/QueryPlan/BuildQueryPipelineSettings.h>
#include <QueryPipeline/QueryPipelineBuilder.h>
#include <Storages/ColumnsDescription.h>
#include <Storages/IStorageCluster.h>
#include <Storages/StorageInMemoryMetadata.h>
#include <Storages/StorageSnapshot.h>
#include <Common/Exception.h>
#include <Common/Logger.h>
#include <Common/tests/gtest_global_context.h>

using namespace DB;

namespace
{

/// Records every call to getTaskIteratorExtension(), including which columns' conditions were
/// present in the filter DAG it was handed.
class RecordingStorageCluster : public IStorageCluster
{
public:
    RecordingStorageCluster(const String & cluster_name, const StorageID & table_id, LoggerPtr log_)
        : IStorageCluster(cluster_name, table_id, log_)
    {
    }

    std::string getName() const override { return "RecordingStorageCluster"; }

    RemoteQueryExecutor::Extension getTaskIteratorExtension(
        const ActionsDAG::Node *,
        const ActionsDAG * filter_actions_dag,
        const ContextPtr &,
        ClusterPtr,
        StorageMetadataPtr) const override
    {
        ++call_count;
        last_filter_input_names.clear();
        if (filter_actions_dag)
            for (const auto * input : filter_actions_dag->getInputs())
                last_filter_input_names.insert(input->result_name);
        return {};
    }

    mutable size_t call_count = 0;
    mutable std::set<std::string> last_filter_input_names;
};

/// Port 1 is never listening, so ReadFromCluster's connection attempt fails immediately
/// (connection refused) instead of timing out — keeps the test fast and hermetic.
ClusterPtr makeSingleUnreachableReplicaCluster(const ContextMutablePtr & context)
{
    std::istringstream config_stream{
        "<clickhouse><remote_servers><test_cluster><shard><replica>"
        "<host>127.0.0.1</host><port>1</port>"
        "</replica></shard></test_cluster></remote_servers></clickhouse>"};
    Poco::AutoPtr<Poco::Util::XMLConfiguration> config = new Poco::Util::XMLConfiguration(config_stream);
    context->setClustersConfig(config, /*enable_discovery=*/false);
    return context->getCluster("test_cluster");
}

ActionsDAG identityFilterDAG(const String & column_name)
{
    return ActionsDAG(NamesAndTypesList{{column_name, std::make_shared<DataTypeUInt8>()}});
}

}

/// Regression test: query plan optimizations call ReadFromCluster::applyFilters() more than
/// once as the plan is progressively refined (optimizeTreeSecondPass revisits the same subtree
/// whenever another pass reports an update; a GROUP BY on the partition/time column is one such
/// trigger). The task/file list must be built from the FINAL accumulated filter, not whatever
/// was known on the first call — building it early and freezing it silently drops conditions
/// discovered by later passes, which previously caused Iceberg partition/row-group pruning to
/// under-prune (icebergCluster() reading 6-13x more rows than the equivalent ice.`ns.table`
/// query for the same WHERE clause, whenever the plan needed more than one optimization pass to
/// stabilize).
TEST(ReadFromCluster, BuildsTaskIteratorFromFinalPredicateNotFirstSnapshot)
{
    const auto & context_holder = getContext();
    auto context = Context::createCopy(context_holder.context);
    auto cluster = makeSingleUnreachableReplicaCluster(context);

    auto storage = std::make_shared<RecordingStorageCluster>("test_cluster", StorageID("db", "table"), getLogger("test"));

    auto metadata = std::make_shared<StorageInMemoryMetadata>();
    metadata->setColumns(ColumnsDescription{NamesAndTypesList{
        {"col_a", std::make_shared<DataTypeUInt8>()},
        {"col_b", std::make_shared<DataTypeUInt8>()}}});

    auto storage_snapshot = std::make_shared<StorageSnapshot>(*storage, metadata);

    Block header;
    header.insert({std::make_shared<DataTypeUInt8>()->createColumn(), std::make_shared<DataTypeUInt8>(), "col_a"});
    header.insert({std::make_shared<DataTypeUInt8>()->createColumn(), std::make_shared<DataTypeUInt8>(), "col_b"});
    auto shared_header = std::make_shared<const Block>(header);

    SelectQueryInfo query_info;

    ReadFromCluster step(
        Names{"col_a", "col_b"},
        query_info,
        storage_snapshot,
        context,
        shared_header,
        storage,
        std::make_shared<ASTSelectQuery>(),
        QueryProcessingStage::Complete,
        cluster,
        getLogger("test"),
        std::nullopt);

    /// Round 1: an early optimize pass only sees the condition on col_a (e.g. before a later
    /// pass — such as aggregation-in-order for a GROUP BY on the partition column — inserts a
    /// second FilterStep above this source).
    step.addFilter(identityFilterDAG("col_a"), "col_a");
    step.applyFilters();

    /// Round 2: a later pass has made col_b's condition visible above the source too.
    /// optimizePrimaryKeyConditionAndLimit re-walks every FilterStep currently above the
    /// source on each call, so both col_a and col_b are re-added here.
    step.addFilter(identityFilterDAG("col_a"), "col_a");
    step.addFilter(identityFilterDAG("col_b"), "col_b");
    step.applyFilters();

    QueryPipelineBuilder builder;
    BuildQueryPipelineSettings settings(context);
    try
    {
        step.initializePipeline(builder, settings);
    }
    catch (const Exception &)
    {
        /// Expected: the configured replica is unreachable. What matters is what predicate
        /// getTaskIteratorExtension() was called with before that connection attempt.
    }

    ASSERT_EQ(storage->call_count, 1u);
    EXPECT_TRUE(storage->last_filter_input_names.contains("col_a"));
    EXPECT_TRUE(storage->last_filter_input_names.contains("col_b"))
        << "task iterator was built from a stale predicate that predates the second applyFilters() call";
}
