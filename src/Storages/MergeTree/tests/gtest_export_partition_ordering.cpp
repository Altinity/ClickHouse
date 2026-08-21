#include <gtest/gtest.h>
#include <sstream>
#include <Storages/ExportReplicatedMergeTreePartitionTaskEntry.h>
#include <Storages/MergeTree/ExportPartitionUtils.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/Exception.h>
#include <Core/Settings.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCOMPATIBLE_COLUMNS;
    extern const int THERE_IS_NO_COLUMN;
    extern const int NUMBER_OF_COLUMNS_DOESNT_MATCH;
}

namespace Setting
{
    extern const SettingsMergeTreePartExportSchemaMatchMode export_merge_tree_part_schema_match_mode;
    extern const SettingsBool ignore_extra_source_columns;
}

namespace
{
    template <typename DataType>
    ColumnWithTypeAndName makeColumn(const String & name)
    {
        auto type = std::make_shared<DataType>();
        return {type->createColumn(), type, name};
    }

    template <typename Function>
    void expectExceptionCode(Function && function, int expected_code)
    {
        try
        {
            function();
            FAIL() << "Expected exception code " << expected_code;
        }
        catch (const Exception & exception)
        {
            EXPECT_EQ(exception.code(), expected_code) << exception.message();
        }
    }

    ExportReplicatedMergeTreePartitionManifest makeValidManifest()
    {
        ExportReplicatedMergeTreePartitionManifest manifest;
        manifest.transaction_id = "tx1";
        manifest.query_id = "query1";
        manifest.partition_id = "2020";
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

class ExportPartitionOrderingTest : public ::testing::Test
{
protected:
    ExportPartitionTaskEntriesContainer container;
    ExportPartitionTaskEntriesContainer::index<ExportPartitionTaskEntryTagByCompositeKey>::type & by_key;
    ExportPartitionTaskEntriesContainer::index<ExportPartitionTaskEntryTagByCreateTime>::type & by_create_time;

    ExportPartitionOrderingTest()
        : by_key(container.get<ExportPartitionTaskEntryTagByCompositeKey>())
        , by_create_time(container.get<ExportPartitionTaskEntryTagByCreateTime>())
    {
    }
};

class ExportPartitionManifestBackCompatTest : public ::testing::Test
{
};

TEST_F(ExportPartitionOrderingTest, IterationOrderMatchesCreateTime)
{
    time_t base_time = 1000;
    
    ExportReplicatedMergeTreePartitionManifest manifest1;
    manifest1.partition_id = "2020";
    manifest1.destination_database = "db1";
    manifest1.destination_table = "table1";
    manifest1.transaction_id = "tx1";
    manifest1.create_time = base_time + 300; // Latest
    
    ExportReplicatedMergeTreePartitionManifest manifest2;
    manifest2.partition_id = "2021";
    manifest2.destination_database = "db1";
    manifest2.destination_table = "table1";
    manifest2.transaction_id = "tx2";
    manifest2.create_time = base_time + 100; // Middle
    
    ExportReplicatedMergeTreePartitionManifest manifest3;
    manifest3.partition_id = "2022";
    manifest3.destination_database = "db1";
    manifest3.destination_table = "table1";
    manifest3.transaction_id = "tx3";
    manifest3.create_time = base_time; // Oldest

    ExportReplicatedMergeTreePartitionTaskEntry entry1{manifest1, ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING, {}, {}, {}, {}};
    ExportReplicatedMergeTreePartitionTaskEntry entry2{manifest2, ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING, {}, {}, {}, {}};
    ExportReplicatedMergeTreePartitionTaskEntry entry3{manifest3, ExportReplicatedMergeTreePartitionTaskEntry::Status::PENDING, {}, {}, {}, {}};

    // Insert in reverse order
    by_key.insert(entry1);
    by_key.insert(entry2);
    by_key.insert(entry3);

    // Verify iteration order matches create_time (ascending)
    auto it = by_create_time.begin();
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.partition_id, "2022"); // Oldest first
    EXPECT_EQ(it->manifest.create_time, base_time);
    
    ++it;
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.partition_id, "2021");
    EXPECT_EQ(it->manifest.create_time, base_time + 100);
    
    ++it;
    ASSERT_NE(it, by_create_time.end());
    EXPECT_EQ(it->manifest.partition_id, "2020");
    EXPECT_EQ(it->manifest.create_time, base_time + 300);
    
    ++it;
    EXPECT_EQ(it, by_create_time.end());
}


TEST_F(ExportPartitionManifestBackCompatTest, MissingSchemaMatchModeParsesAsNullopt)
{
    auto manifest = makeValidManifest();
    manifest.schema_match_mode = MergeTreePartExportSchemaMatchMode::match_by_name;

    Poco::JSON::Parser parser;
    auto json = parser.parse(manifest.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("schema_match_mode");
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    auto parsed = ExportReplicatedMergeTreePartitionManifest::fromJsonString(oss.str());
    EXPECT_FALSE(parsed.schema_match_mode.has_value());
}

TEST_F(ExportPartitionManifestBackCompatTest, SchemaMatchModeRoundTripsForEveryValue)
{
    for (const auto value : magic_enum::enum_values<MergeTreePartExportSchemaMatchMode>())
    {
        auto manifest = makeValidManifest();
        manifest.schema_match_mode = value;

        auto parsed = ExportReplicatedMergeTreePartitionManifest::fromJsonString(manifest.toJsonString());

        ASSERT_TRUE(parsed.schema_match_mode.has_value()) << "value=" << magic_enum::enum_name(value);
        EXPECT_EQ(*parsed.schema_match_mode, value) << "value=" << magic_enum::enum_name(value);
    }
}

TEST_F(ExportPartitionManifestBackCompatTest, MissingIgnoreExtraSourceColumnsParsesAsNullopt)
{
    auto manifest = makeValidManifest();
    manifest.ignore_extra_source_columns = true;

    Poco::JSON::Parser parser;
    auto json = parser.parse(manifest.toJsonString()).extract<Poco::JSON::Object::Ptr>();
    json->remove("ignore_extra_source_columns");
    std::ostringstream oss;
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);

    auto parsed = ExportReplicatedMergeTreePartitionManifest::fromJsonString(oss.str());
    EXPECT_FALSE(parsed.ignore_extra_source_columns.has_value());
}

TEST_F(ExportPartitionManifestBackCompatTest, IgnoreExtraSourceColumnsRoundTripsForEveryValue)
{
    for (const bool value : {false, true})
    {
        auto manifest = makeValidManifest();
        manifest.ignore_extra_source_columns = value;

        auto parsed = ExportReplicatedMergeTreePartitionManifest::fromJsonString(manifest.toJsonString());

        ASSERT_TRUE(parsed.ignore_extra_source_columns.has_value()) << "value=" << value;
        EXPECT_EQ(*parsed.ignore_extra_source_columns, value) << "value=" << value;
    }
}

TEST_F(ExportPartitionManifestBackCompatTest, MissingSchemaMatchSettingsFallBackToDefaultsInWorkerContext)
{
    auto manifest = makeValidManifest();
    ASSERT_FALSE(manifest.schema_match_mode.has_value());
    ASSERT_FALSE(manifest.ignore_extra_source_columns.has_value());

    auto worker_context = ExportPartitionUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

    EXPECT_EQ(
        worker_context->getSettingsRef()[Setting::export_merge_tree_part_schema_match_mode].value,
        MergeTreePartExportSchemaMatchMode::match_by_position);
    EXPECT_EQ(
        worker_context->getSettingsRef()[Setting::ignore_extra_source_columns].value,
        false);
}

TEST_F(ExportPartitionManifestBackCompatTest, SchemaMatchModeAppliedToWorkerContextForEveryValue)
{
    for (const auto value : magic_enum::enum_values<MergeTreePartExportSchemaMatchMode>())
    {
        auto manifest = makeValidManifest();
        manifest.schema_match_mode = value;

        auto worker_context = ExportPartitionUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

        EXPECT_EQ(
            worker_context->getSettingsRef()[Setting::export_merge_tree_part_schema_match_mode].value,
            value) << "value=" << magic_enum::enum_name(value);
    }
}

TEST_F(ExportPartitionManifestBackCompatTest, IgnoreExtraSourceColumnsAppliedToWorkerContextForEveryValue)
{
    for (const bool value : {false, true})
    {
        auto manifest = makeValidManifest();
        manifest.ignore_extra_source_columns = value;

        auto worker_context = ExportPartitionUtils::getContextCopyWithTaskSettings(getContext().context, manifest);

        EXPECT_EQ(
            worker_context->getSettingsRef()[Setting::ignore_extra_source_columns].value,
            value) << "value=" << value;
    }
}

TEST(ExportColumnCastsTest, UsesSelectedMatchingMode)
{
    const ColumnsWithTypeAndName source_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
        makeColumn<DataTypeString>("payload"),
        makeColumn<DataTypeString>("extra"),
    };
    const ColumnsWithTypeAndName destination_columns = {
        makeColumn<DataTypeString>("payload"),
        makeColumn<DataTypeInt64>("year"),
        makeColumn<DataTypeInt64>("id"),
    };
    const StorageID destination_storage_id{"test", "destination"};

    expectExceptionCode(
        [&]
        {
            ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                source_columns,
                destination_columns,
                MergeTreePartExportSchemaMatchMode::match_by_position,
                /*ignore_extra_source_columns=*/ true,
                destination_storage_id);
        },
        ErrorCodes::INCOMPATIBLE_COLUMNS);

    EXPECT_NO_THROW(ExportPartitionUtils::verifyExportColumnCastsAreSafe(
        source_columns,
        destination_columns,
        MergeTreePartExportSchemaMatchMode::match_by_name,
        /*ignore_extra_source_columns=*/ true,
        destination_storage_id));
}

TEST(ExportColumnCastsTest, MatchByNameAcceptsReorderedColumnsWithEqualColumnCount)
{
    const ColumnsWithTypeAndName source_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
        makeColumn<DataTypeString>("payload"),
    };
    const ColumnsWithTypeAndName reordered_destination_columns = {
        makeColumn<DataTypeString>("payload"),
        makeColumn<DataTypeInt64>("year"),
        makeColumn<DataTypeInt64>("id"),
    };
    const StorageID destination_storage_id{"test", "destination"};

    for (const bool ignore_extra_source_columns : {false, true})
    {
        expectExceptionCode(
            [&]
            {
                ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                    source_columns,
                    reordered_destination_columns,
                    MergeTreePartExportSchemaMatchMode::match_by_position,
                    ignore_extra_source_columns,
                    destination_storage_id);
            },
            ErrorCodes::INCOMPATIBLE_COLUMNS);

        EXPECT_NO_THROW(ExportPartitionUtils::verifyExportColumnCastsAreSafe(
            source_columns,
            reordered_destination_columns,
            MergeTreePartExportSchemaMatchMode::match_by_name,
            ignore_extra_source_columns,
            destination_storage_id));
    }

    const ColumnsWithTypeAndName same_order_destination_columns = {
        makeColumn<DataTypeInt64>("id"),
        makeColumn<DataTypeInt64>("year"),
        makeColumn<DataTypeString>("payload"),
    };

    for (const auto mode : {MergeTreePartExportSchemaMatchMode::match_by_position, MergeTreePartExportSchemaMatchMode::match_by_name})
        for (const bool ignore_extra_source_columns : {false, true})
            EXPECT_NO_THROW(ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                source_columns, same_order_destination_columns, mode, ignore_extra_source_columns, destination_storage_id));
}

TEST(ExportColumnCastsTest, MatchByNameRejectsUnmatchedSourceColumnUnlessIgnored)
{
    const ColumnsWithTypeAndName source_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
        makeColumn<DataTypeString>("extra"),
    };
    const ColumnsWithTypeAndName destination_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
    };
    const StorageID destination_storage_id{"test", "destination"};

    expectExceptionCode(
        [&]
        {
            ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                source_columns,
                destination_columns,
                MergeTreePartExportSchemaMatchMode::match_by_name,
                /*ignore_extra_source_columns=*/ false,
                destination_storage_id);
        },
        ErrorCodes::NUMBER_OF_COLUMNS_DOESNT_MATCH);

    EXPECT_NO_THROW(ExportPartitionUtils::verifyExportColumnCastsAreSafe(
        source_columns,
        destination_columns,
        MergeTreePartExportSchemaMatchMode::match_by_name,
        /*ignore_extra_source_columns=*/ true,
        destination_storage_id));
}

TEST(ExportColumnCastsTest, RejectsLossyCastAfterMatchingByName)
{
    const ColumnsWithTypeAndName source_columns = {
        makeColumn<DataTypeInt64>("id"),
        makeColumn<DataTypeInt32>("year"),
        makeColumn<DataTypeString>("extra"),
    };
    const ColumnsWithTypeAndName destination_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
    };

    expectExceptionCode(
        [&]
        {
            ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                source_columns,
                destination_columns,
                MergeTreePartExportSchemaMatchMode::match_by_name,
                /*ignore_extra_source_columns=*/ true,
                StorageID{"test", "destination"});
        },
        ErrorCodes::INCOMPATIBLE_COLUMNS);
}

TEST(ExportColumnCastsTest, RejectsMissingDestinationColumnAfterMatchingByName)
{
    const ColumnsWithTypeAndName source_columns = {
        makeColumn<DataTypeInt32>("id"),
        makeColumn<DataTypeInt32>("year"),
        makeColumn<DataTypeString>("extra"),
    };
    const ColumnsWithTypeAndName destination_columns = {
        makeColumn<DataTypeInt32>("renamed_id"),
        makeColumn<DataTypeInt32>("year"),
    };

    expectExceptionCode(
        [&]
        {
            ExportPartitionUtils::verifyExportColumnCastsAreSafe(
                source_columns,
                destination_columns,
                MergeTreePartExportSchemaMatchMode::match_by_name,
                /*ignore_extra_source_columns=*/ true,
                StorageID{"test", "destination"});
        },
        ErrorCodes::THERE_IS_NO_COLUMN);
}

}
