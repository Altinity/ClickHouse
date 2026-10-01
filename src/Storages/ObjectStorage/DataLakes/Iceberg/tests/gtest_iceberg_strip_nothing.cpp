#include <gtest/gtest.h>

#include <Columns/ColumnConst.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeFactory.h>
#include <Processors/Chunk.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/DataFileStatistics.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/MultipleFileWriter.h>

using namespace DB;

namespace
{

DataTypePtr type(const String & name)
{
    return DataTypeFactory::instance().get(name);
}

String strippedName(const String & name)
{
    auto stripped = Iceberg::stripNothing(type(name));
    return stripped ? stripped->getName() : "nullptr";
}

}

TEST(IcebergStripNothing, TypeWithoutNothingIsReturnedAsIs)
{
    for (const auto * name : {"Int64", "Nullable(String)", "Tuple(a Int64, b Nullable(String))", "Array(Int32)", "Map(String, Int64)"})
    {
        auto original = type(name);
        EXPECT_EQ(Iceberg::stripNothing(original), original) << name;
    }
}

TEST(IcebergStripNothing, NothingLeavesAreRemoved)
{
    EXPECT_EQ(strippedName("Nothing"), "nullptr");
    EXPECT_EQ(strippedName("Nullable(Nothing)"), "nullptr");
    EXPECT_EQ(strippedName("Tuple(a Nullable(Int64), u Nullable(Nothing))"), "Tuple(a Nullable(Int64))");
    EXPECT_EQ(strippedName("Tuple(u Nullable(Nothing), v Nullable(Nothing))"), "nullptr");
    EXPECT_EQ(
        strippedName("Tuple(a Int64, s Tuple(b Nullable(String), u Nullable(Nothing)), t Tuple(u Nullable(Nothing)))"),
        "Tuple(a Int64, s Tuple(b Nullable(String)))");
    EXPECT_EQ(strippedName("Array(Tuple(a Nullable(Int64), u Nullable(Nothing)))"), "Array(Tuple(a Nullable(Int64)))");
    EXPECT_EQ(strippedName("Map(String, Tuple(a Nullable(Int64), u Nullable(Nothing)))"), "Map(String, Tuple(a Nullable(Int64)))");
}

TEST(IcebergStripNothing, ContainerOfOnlyNothingStripsToNothing)
{
    EXPECT_EQ(strippedName("Array(Nullable(Nothing))"), "nullptr");
    EXPECT_EQ(strippedName("Array(Tuple(u Nullable(Nothing)))"), "nullptr");
    EXPECT_EQ(strippedName("Map(String, Nullable(Nothing))"), "nullptr");
}

TEST(IcebergStripNothing, TupleColumnKeepsSiblingValues)
{
    auto original = type("Tuple(a Nullable(Int64), u Nullable(Nothing))");
    auto stripped = Iceberg::stripNothing(original);

    auto column = original->createColumn();
    column->insert(Tuple{Field(Int64(42)), Field()});
    column->insert(Tuple{Field(), Field()});

    auto result = Iceberg::stripNothingColumn(std::move(column), original, stripped);
    EXPECT_EQ(result->getName(), stripped->createColumn()->getName());
    ASSERT_EQ(result->size(), 2);
    EXPECT_EQ((*result)[0], Field(Tuple{Field(Int64(42))}));
    EXPECT_EQ((*result)[1], Field(Tuple{Field()}));
}

TEST(IcebergStripNothing, ConstTupleColumnIsMaterialized)
{
    auto original = type("Tuple(a Nullable(Int64), u Nullable(Nothing))");
    auto stripped = Iceberg::stripNothing(original);

    auto single = original->createColumn();
    single->insert(Tuple{Field(Int64(7)), Field()});
    ColumnPtr column = ColumnConst::create(std::move(single), 3);

    auto result = Iceberg::stripNothingColumn(column, original, stripped);
    EXPECT_EQ(result->getName(), stripped->createColumn()->getName());
    ASSERT_EQ(result->size(), 3);
    for (size_t row = 0; row < 3; ++row)
        EXPECT_EQ((*result)[row], Field(Tuple{Field(Int64(7))}));
}

TEST(IcebergStripNothing, ArrayAndMapColumnsKeepOffsetsAndKeys)
{
    {
        auto original = type("Array(Tuple(a Nullable(Int64), u Nullable(Nothing)))");
        auto stripped = Iceberg::stripNothing(original);

        auto column = original->createColumn();
        column->insert(Array{Field(Tuple{Field(Int64(1)), Field()}), Field(Tuple{Field(Int64(2)), Field()})});
        column->insert(Array{});

        auto result = Iceberg::stripNothingColumn(std::move(column), original, stripped);
        EXPECT_EQ(result->getName(), stripped->createColumn()->getName());
        ASSERT_EQ(result->size(), 2);
        EXPECT_EQ((*result)[0], Field(Array{Field(Tuple{Field(Int64(1))}), Field(Tuple{Field(Int64(2))})}));
        EXPECT_EQ((*result)[1], Field(Array{}));
    }
    {
        auto original = type("Map(String, Tuple(a Nullable(Int64), u Nullable(Nothing)))");
        auto stripped = Iceberg::stripNothing(original);

        auto column = original->createColumn();
        column->insert(Map{Field(Tuple{Field("k"), Field(Tuple{Field(Int64(5)), Field()})})});

        auto result = Iceberg::stripNothingColumn(std::move(column), original, stripped);
        EXPECT_EQ(result->getName(), stripped->createColumn()->getName());
        ASSERT_EQ(result->size(), 1);
        EXPECT_EQ((*result)[0], Field(Map{Field(Tuple{Field("k"), Field(Tuple{Field(Int64(5))})})}));
    }
}

#if USE_AVRO

namespace
{

/// Schema fields with ids 1, 2, 3 for (id Int64, name Nullable(String), u Nullable(Nothing)).
Poco::JSON::Array::Ptr statisticsSchema()
{
    Poco::JSON::Array::Ptr schema = new Poco::JSON::Array;
    for (Int32 id = 1; id <= 3; ++id)
    {
        Poco::JSON::Object::Ptr field = new Poco::JSON::Object;
        field->set(Iceberg::f_id, id);
        schema->add(field);
    }
    return schema;
}

Chunk statisticsChunk()
{
    auto id = type("Int64")->createColumn();
    id->insert(Field(Int64(10)));
    id->insert(Field(Int64(11)));

    auto name = type("Nullable(String)")->createColumn();
    name->insert(Field("x"));
    name->insert(Field("y"));

    auto unknown = type("Nullable(Nothing)")->createColumn();
    unknown->insertDefault();
    unknown->insertDefault();

    Columns columns;
    columns.push_back(std::move(id));
    columns.push_back(std::move(name));
    columns.push_back(std::move(unknown));
    return Chunk(std::move(columns), 2);
}

template <typename T>
std::vector<size_t> fieldIdsOf(const std::vector<std::pair<size_t, T>> & stats)
{
    std::vector<size_t> ids;
    for (const auto & [id, _] : stats)
        ids.push_back(id);
    return ids;
}

}

TEST(IcebergStripNothing, ExcludedColumnsHaveNoStatistics)
{
    DataFileStatistics stats(statisticsSchema());
    stats.excludeColumns({false, false, true});
    stats.update(statisticsChunk());

    const std::vector<size_t> written = {1, 2};
    EXPECT_EQ(fieldIdsOf(stats.getColumnSizes()), written);
    EXPECT_EQ(fieldIdsOf(stats.getNullCounts()), written);
    EXPECT_EQ(fieldIdsOf(stats.getLowerBounds()), written);
    EXPECT_EQ(fieldIdsOf(stats.getUpperBounds()), written);
    EXPECT_EQ(stats.getLowerBounds()[0].second, Field(Int64(10)));
    EXPECT_EQ(stats.getUpperBounds()[0].second, Field(Int64(11)));

    DataFileStatistics merged(statisticsSchema());
    merged.merge(stats);
    EXPECT_EQ(fieldIdsOf(merged.getColumnSizes()), written);
    EXPECT_EQ(fieldIdsOf(merged.getLowerBounds()), written);
}

TEST(IcebergStripNothing, StatisticsWithoutExclusionCoverEveryColumn)
{
    DataFileStatistics stats(statisticsSchema());
    stats.update(statisticsChunk());

    const std::vector<size_t> all = {1, 2, 3};
    EXPECT_EQ(fieldIdsOf(stats.getColumnSizes()), all);
    EXPECT_EQ(fieldIdsOf(stats.getLowerBounds()), all);
}

#endif
