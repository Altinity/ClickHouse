#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <Storages/ExportReplicatedMergeTreeTaskEntry.h>
#include <Storages/MergeTree/ExportTaskUtils.h>
#include <Storages/MergeTree/MergeTreeExportTask.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeFactory.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

namespace DB
{

namespace Setting
{
    extern const SettingsMergeTreePartExportSchemaMatchMode export_merge_tree_part_schema_match_mode;
    extern const SettingsBool export_merge_tree_part_ignore_extra_source_columns;
}

namespace ErrorCodes
{
    extern const int NO_SUCH_DATA_PART;
    extern const int UNKNOWN_TABLE;
    extern const int BAD_ARGUMENTS;
    extern const int NETWORK_ERROR;
    extern const int INCOMPATIBLE_COLUMNS;
}

namespace
{
    ExportReplicatedMergeTreeTaskManifest makeValidManifest()
    {
        ExportReplicatedMergeTreeTaskManifest manifest;
        manifest.transaction_id = "tx1";
        manifest.query_id = "query1";
        manifest.parts = {"2020_1_1_0"};
        manifest.destination_database = "db1";
        manifest.destination_table = "table1";
        manifest.source_replica = "r1";
        manifest.number_of_parts = 1;
        manifest.create_time = 1000;
        manifest.task_timeout_seconds = 60;
        manifest.max_threads = 1;
        manifest.parallel_formatting = true;
        manifest.parquet_parallel_encoding = true;
        manifest.max_bytes_per_file = 1000000;
        manifest.max_rows_per_file = 1000;
        manifest.file_already_exists_policy = MergeTreePartExportManifest::FileAlreadyExistsPolicy::error;
        manifest.filename_pattern = "{part_name}";
        return manifest;
    }
}

class ExportTaskOrderingTest : public ::testing::Test
{
protected:
    ExportTaskEntriesContainer container;
    ExportTaskEntriesContainer::index<ExportTaskEntryTagByTransactionId>::type & by_key;
    ExportTaskEntriesContainer::index<ExportTaskEntryTagByCreateTime>::type & by_create_time;

    ExportTaskOrderingTest()
        : by_key(container.get<ExportTaskEntryTagByTransactionId>())
        , by_create_time(container.get<ExportTaskEntryTagByCreateTime>())
    {
    }
};

class ExportTaskManifestBackCompatTest : public ::testing::Test
{
};

TEST_F(ExportTaskOrderingTest, IterationOrderMatchesCreateTime)
{
    time_t base_time = 1000;
    
    ExportReplicatedMergeTreeTaskManifest manifest1;
    manifest1.parts = {"2020_1_1_0"};
    manifest1.destination_database = "db1";
    manifest1.destination_table = "table1";
    manifest1.transaction_id = "tx1";
    manifest1.create_time = base_time + 300; // Latest
    
    ExportReplicatedMergeTreeTaskManifest manifest2;
    manifest2.parts = {"2021_1_1_0"};
    manifest2.destination_database = "db1";
    manifest2.destination_table = "table1";
    manifest2.transaction_id = "tx2";
    manifest2.create_time = base_time + 100; // Middle
    
    ExportReplicatedMergeTreeTaskManifest manifest3;
    manifest3.parts = {"2022_1_1_0"};
    manifest3.destination_database = "db1";
    manifest3.destination_table = "table1";
    manifest3.transaction_id = "tx3";
    manifest3.create_time = base_time; // Oldest

    ExportReplicatedMergeTreeTaskEntry entry1{manifest1, ExportReplicatedMergeTreeTaskEntry::Status::PENDING, {}, {}, {}, {}};
    ExportReplicatedMergeTreeTaskEntry entry2{manifest2, ExportReplicatedMergeTreeTaskEntry::Status::PENDING, {}, {}, {}, {}};
    ExportReplicatedMergeTreeTaskEntry entry3{manifest3, ExportReplicatedMergeTreeTaskEntry::Status::PENDING, {}, {}, {}, {}};

    // Insert in reverse order
    by_key.insert(entry1);
    by_key.insert(entry2);
    by_key.insert(entry3);

    // Verify iteration order matches create_time (ascending)
    auto it = by_create_time.begin();
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.transaction_id, "tx3"); // Oldest first
    EXPECT_EQ(it->manifest.create_time, base_time);
    
    ++it;
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.transaction_id, "tx2");
    EXPECT_EQ(it->manifest.create_time, base_time + 100);
    
    ++it;
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.transaction_id, "tx1");
    EXPECT_EQ(it->manifest.create_time, base_time + 300);
    
    ++it;
    EXPECT_EQ(it, by_create_time.end());
}


TEST_F(ExportTaskManifestBackCompatTest, MissingSchemaMatchModeParsesAsNullopt)
{
    auto manifest = makeValidManifest();
    manifest.schema_match_mode = MergeTreePartExportSchemaMatchMode::NAME;

    Poco::JSON::Parser parser;
    auto json = parser.parse(manifest.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("schema_match_mode");
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    auto parsed = ExportReplicatedMergeTreeTaskManifest::fromJsonString(oss.str());
    EXPECT_FALSE(parsed.schema_match_mode.has_value());
}

TEST_F(ExportTaskManifestBackCompatTest, SchemaMatchModeRoundTripsForEveryValue)
{
    for (const auto value : magic_enum::enum_values<MergeTreePartExportSchemaMatchMode>())
    {
        auto manifest = makeValidManifest();
        manifest.schema_match_mode = value;

        auto parsed = ExportReplicatedMergeTreeTaskManifest::fromJsonString(manifest.toJsonString());

        ASSERT_TRUE(parsed.schema_match_mode.has_value()) << "value=" << magic_enum::enum_name(value);
        EXPECT_EQ(*parsed.schema_match_mode, value) << "value=" << magic_enum::enum_name(value);
    }
}

TEST_F(ExportTaskManifestBackCompatTest, MissingIgnoreExtraSourceColumnsParsesAsNullopt)
{
    auto manifest = makeValidManifest();
    manifest.ignore_extra_source_columns = true;

    Poco::JSON::Parser parser;
    auto json = parser.parse(manifest.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("ignore_extra_source_columns");
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    auto parsed = ExportReplicatedMergeTreeTaskManifest::fromJsonString(oss.str());
    EXPECT_FALSE(parsed.ignore_extra_source_columns.has_value());
}

TEST_F(ExportTaskManifestBackCompatTest, IgnoreExtraSourceColumnsRoundTripsForEveryValue)
{
    for (const bool value : {false, true})
    {
        auto manifest = makeValidManifest();
        manifest.ignore_extra_source_columns = value;

        auto parsed = ExportReplicatedMergeTreeTaskManifest::fromJsonString(manifest.toJsonString());

        ASSERT_TRUE(parsed.ignore_extra_source_columns.has_value()) << "value=" << value;
        EXPECT_EQ(*parsed.ignore_extra_source_columns, value) << "value=" << value;
    }
}

TEST_F(ExportTaskManifestBackCompatTest, MissingSchemaMatchSettingsFallBackToDefaultsInWorkerContext)
{
    auto manifest = makeValidManifest();
    ASSERT_FALSE(manifest.schema_match_mode.has_value());
    ASSERT_FALSE(manifest.ignore_extra_source_columns.has_value());

    auto worker_context = ExportTaskUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

    EXPECT_EQ(
        worker_context->getSettingsRef()[Setting::export_merge_tree_part_schema_match_mode].value,
        MergeTreePartExportSchemaMatchMode::POSITION);
    EXPECT_EQ(
        worker_context->getSettingsRef()[Setting::export_merge_tree_part_ignore_extra_source_columns].value,
        false);
}

TEST_F(ExportTaskManifestBackCompatTest, SchemaMatchModeAppliedToWorkerContextForEveryValue)
{
    for (const auto value : magic_enum::enum_values<MergeTreePartExportSchemaMatchMode>())
    {
        auto manifest = makeValidManifest();
        manifest.schema_match_mode = value;

        auto worker_context = ExportTaskUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

        EXPECT_EQ(
            worker_context->getSettingsRef()[Setting::export_merge_tree_part_schema_match_mode].value,
            value) << "value=" << magic_enum::enum_name(value);
    }
}

TEST_F(ExportTaskManifestBackCompatTest, IgnoreExtraSourceColumnsAppliedToWorkerContextForEveryValue)
{
    for (const bool value : {false, true})
    {
        auto manifest = makeValidManifest();
        manifest.ignore_extra_source_columns = value;

        auto worker_context = ExportTaskUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

        EXPECT_EQ(
            worker_context->getSettingsRef()[Setting::export_merge_tree_part_ignore_extra_source_columns].value,
            value) << "value=" << value;
    }
}

TEST_F(ExportTaskManifestBackCompatTest, TaskSourceAndCommitIdRoundTrip)
{
    auto manifest = makeValidManifest();
    manifest.source = ExportTaskSource::ttl;
    manifest.commit_id = "tx0";

    const auto parsed = ExportReplicatedMergeTreeTaskManifest::fromJsonString(manifest.toJsonString());
    EXPECT_EQ(parsed.source, ExportTaskSource::ttl);
    EXPECT_EQ(parsed.commit_id, "tx0");
}

TEST_F(ExportTaskManifestBackCompatTest, MissingCommitIdIsTheTransactionId)
{
    auto manifest = makeValidManifest();
    manifest.commit_id = "tx0";

    Poco::JSON::Parser parser;
    auto json = parser.parse(manifest.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("commit_id");
    json->set("retry_of", Poco::JSON::Array::Ptr(new Poco::JSON::Array()));
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    EXPECT_EQ(ExportReplicatedMergeTreeTaskManifest::fromJsonString(oss.str()).commit_id, "tx1");
}

TEST_F(ExportTaskManifestBackCompatTest, PlainTaskMissingCommitIdIsTheTransactionId)
{
    MergeTreeExportTask task;
    task.transaction_id = "tx1";
    task.commit_id = "tx0";
    task.source = ExportTaskSource::ttl;
    task.parts.push_back({.part_name = "p_1_3_1", .done = true, .paths_in_destination = {"a.parquet"}});
    EXPECT_EQ(MergeTreeExportTask::fromJsonString(task.toJsonString()).commit_id, "tx0");

    Poco::JSON::Parser parser;
    auto json = parser.parse(task.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("commit_id");
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    EXPECT_EQ(MergeTreeExportTask::fromJsonString(oss.str()).commit_id, "tx1");
}

namespace
{
    /// Whether a manual export, which does not allow lossy casts by default, may export a column of
    /// type `from` to a column of type `to`.
    bool isCastAllowedByDefault(const String & from, const String & to)
    {
        tryRegisterFunctions();
        const auto metadata_with_column_of_type = [](const String & type)
        {
            auto metadata = std::make_shared<StorageInMemoryMetadata>();
            metadata->setColumns(ColumnsDescription(NamesAndTypesList{{"ts", DataTypeFactory::instance().get(type)}}));
            return metadata;
        };

        try
        {
            ExportTaskUtils::verifyExportSchemaCastable(
                metadata_with_column_of_type(from), metadata_with_column_of_type(to), StorageID("db", "destination"), getContext().context);
            return true;
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::INCOMPATIBLE_COLUMNS)
                return false;
            throw;
        }
    }
}

TEST(ExportTaskUtils, DateAndTimeWideningIsNotLossy)
{
    EXPECT_TRUE(isCastAllowedByDefault("Date", "Date32"));
    EXPECT_TRUE(isCastAllowedByDefault("DateTime", "DateTime64(6)"));
    EXPECT_TRUE(isCastAllowedByDefault("DateTime", "DateTime64(9)"));
    EXPECT_TRUE(isCastAllowedByDefault("DateTime64(3)", "DateTime64(6)"));
    EXPECT_TRUE(isCastAllowedByDefault("DateTime64(9)", "DateTime64(9)"));

    EXPECT_FALSE(isCastAllowedByDefault("DateTime64(9)", "DateTime64(6)"));
    /// The range of scale 9 ends in 2262.
    EXPECT_FALSE(isCastAllowedByDefault("DateTime64(3)", "DateTime64(9)"));
    EXPECT_FALSE(isCastAllowedByDefault("DateTime64(6)", "DateTime"));
}

TEST(ExportTaskUtils, PartitionIdIsDerivedFromParts)
{
    EXPECT_EQ(ExportTaskUtils::getPartitionIdOfParts({"2020_1_1_0", "2020_2_5_1"}, MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING), "2020");
    EXPECT_EQ(ExportTaskUtils::getPartitionIdOfParts({"all_3_3_0"}, MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING), "all");
}

TEST(ExportTaskRetryClassification, MissingPartIsFatalOnlyOnPlainPath)
{
    EXPECT_FALSE(ExportTaskUtils::isNonRetryableExportError(ErrorCodes::NO_SUCH_DATA_PART));
    EXPECT_TRUE(ExportTaskUtils::isNonRetryablePlainExportError(ErrorCodes::NO_SUCH_DATA_PART));

    EXPECT_FALSE(ExportTaskUtils::isNonRetryableExportError(ErrorCodes::UNKNOWN_TABLE));
    EXPECT_TRUE(ExportTaskUtils::isNonRetryablePlainExportError(ErrorCodes::UNKNOWN_TABLE));

    EXPECT_TRUE(ExportTaskUtils::isNonRetryableExportError(ErrorCodes::BAD_ARGUMENTS));
    EXPECT_TRUE(ExportTaskUtils::isNonRetryablePlainExportError(ErrorCodes::BAD_ARGUMENTS));

    EXPECT_FALSE(ExportTaskUtils::isNonRetryableExportError(ErrorCodes::NETWORK_ERROR));
    EXPECT_FALSE(ExportTaskUtils::isNonRetryablePlainExportError(ErrorCodes::NETWORK_ERROR));
}

}
