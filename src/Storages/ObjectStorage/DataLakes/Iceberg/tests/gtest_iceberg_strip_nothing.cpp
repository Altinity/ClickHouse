#include <gtest/gtest.h>

#include <Columns/ColumnConst.h>
#include <Core/Field.h>
#include <DataTypes/DataTypeFactory.h>
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
