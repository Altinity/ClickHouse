#include <gtest/gtest.h>

#include <Common/tests/gtest_global_register.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeFactory.h>

using namespace DB;

/// `annotatedTypeMatchesDerived` decides whether a type name recorded next to the data may be
/// honoured for a column whose schema derives a different type.

namespace
{

DataTypePtr typeFromString(const String & name)
{
    tryRegisterAggregateFunctions();
    return DataTypeFactory::instance().get(name);
}

bool matches(const String & annotated, const String & derived)
{
    return annotatedTypeMatchesDerived(typeFromString(annotated), typeFromString(derived));
}

/// The mode the parquet reader uses: the annotation must also name a type the parquet writer maps to
/// what the file holds.
bool matchesStrictly(const String & annotated, const String & derived)
{
    return annotatedTypeMatchesDerived(typeFromString(annotated), typeFromString(derived), /*strict=*/ true);
}

}

TEST(AnnotatedTypeMatchesDerived, AggregateFunctionNeedsAStringPosition)
{
    /// A serialized state derives as `String`, or `Nullable(String)` for an optional Iceberg field.
    EXPECT_TRUE(matches("AggregateFunction(uniq, UInt64)", "String"));
    EXPECT_TRUE(matches("AggregateFunction(uniq, UInt64)", "Nullable(String)"));

    /// Anything else means the annotation does not describe these bytes.
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Int64"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "FixedString(16)"));
    EXPECT_FALSE(matches("AggregateFunction(uniq, UInt64)", "Array(String)"));

    /// Strictness elsewhere does not loosen an `AggregateFunction` position, and this pair is the
    /// check the parquet reader makes for a state column.
    EXPECT_TRUE(matchesStrictly("AggregateFunction(uniq, UInt64)", "String"));
    EXPECT_FALSE(matchesStrictly("AggregateFunction(uniq, UInt64)", "Int64"));
}

TEST(AnnotatedTypeMatchesDerived, ContainersAreMatchedElementwise)
{
    EXPECT_TRUE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(String)"));
    EXPECT_TRUE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(Nullable(String))"));
    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "Array(Int64)"));

    EXPECT_TRUE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, String)"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Map(String, Int64)"));

    /// Tuple elements are matched by position, so a state in one element does not excuse another.
    EXPECT_TRUE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a String, b Int64)"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a Int64, b String)"));
}

TEST(AnnotatedTypeMatchesDerived, TheNestingStructureItselfMustMatch)
{
    EXPECT_FALSE(matches("Array(AggregateFunction(uniq, UInt64))", "String"));
    EXPECT_FALSE(matches("Map(String, AggregateFunction(uniq, UInt64))", "Array(Tuple(String, String))"));
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "String"));
    /// A tuple that lost or gained an element is not the same column either.
    EXPECT_FALSE(matches("Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a String)"));
}

TEST(AnnotatedTypeMatchesDerived, PositionsThatDoNotHoldStatesAreLenient)
{
    /// `SimpleAggregateFunction` holds an ordinary value, so the annotation is expected to disagree
    /// with the derived type: Iceberg stores `UInt64` as `long`, which reads back as `Int64`.
    EXPECT_TRUE(matches("SimpleAggregateFunction(sum, UInt64)", "Int64"));
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, Nullable(String))", "Nullable(String)"));
    EXPECT_TRUE(matches("Array(SimpleAggregateFunction(sum, UInt64))", "Array(Int64)"));
}

TEST(AnnotatedTypeMatchesDerived, StrictModeRefusesAnAnnotationThatRetypesTheColumn)
{
    /// The reason strict mode exists: `castColumn` converts an integer to a timestamp silently, so a
    /// lenient check would let an annotation decide what a column of numbers means, with no way to
    /// opt out. Parquet writes `UInt64` as UINT_64 and `DateTime64(9)` as TIMESTAMP(NANOS), neither
    /// of which derives as `Int64`.
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(9))", "Int64"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "Int64"));
    /// Lenient mode still accepts both - it has to, see PositionsThatDoNotHoldStatesAreLenient.
    EXPECT_TRUE(matches("SimpleAggregateFunction(anyLast, DateTime64(9))", "Int64"));

    /// Same for the other reinterpretations parquet round-trips too exactly to excuse.
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "Int32"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv4)", "Int32"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Float64)", "Int64"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, String)", "FixedString(16)"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, Decimal(18, 4))", "Decimal(18, 2)"));
    /// A scale parquet cannot have is rounded up to the next unit, not down and not to any other.
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(3)"));
    EXPECT_FALSE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(9)"));

    /// A state in one position does not excuse a re-typing in another.
    EXPECT_FALSE(matchesStrictly(
        "Tuple(a AggregateFunction(uniq, UInt64), b DateTime64(9))", "Tuple(a String, b Int64)"));
    EXPECT_FALSE(matchesStrictly("Array(SimpleAggregateFunction(sum, UInt64))", "Array(Int64)"));
    EXPECT_FALSE(matchesStrictly(
        "Map(String, SimpleAggregateFunction(anyLast, DateTime64(9)))", "Map(String, Int64)"));
}

TEST(AnnotatedTypeMatchesDerived, StrictModeAcceptsWhatTheParquetWriterProduces)
{
    /// Every pair below is a type the parquet writer can be handed and the type the reader derives
    /// from what it wrote; 04673_parquet_aggregate_function_state.sh round-trips them through the real
    /// writer. The mapping is not injective, so this is a directed relation: `Date` may derive as
    /// `Date32`, but a `Date32` annotation over a UINT_16 may not.

    /// Types parquet stores exactly.
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "UInt64"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, String)", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, UUID)", "UUID"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Decimal(18, 4))", "Decimal(18, 4)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, FixedString(16))", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date32)", "Date32"));

    /// The annotation and the derived type do not carry `Nullable` the same way, and it says nothing
    /// about the values, so it is ignored on both sides.
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(sum, UInt64)", "Nullable(UInt64)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Nullable(String))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Nullable(String))", "Nullable(String)"));
    /// And at every level, not just the top: a list element is written OPTIONAL by default.
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Array(UInt64))", "Array(Nullable(UInt64))"));
    EXPECT_TRUE(matchesStrictly(
        "SimpleAggregateFunction(anyLast, Map(String, UInt64))", "Map(String, Nullable(UInt64))"));
    EXPECT_TRUE(matchesStrictly(
        "Tuple(a AggregateFunction(uniq, UInt64), b UInt64)", "Tuple(a Nullable(String), b Nullable(UInt64))"));

    /// A `LowCardinality` column is written through its dictionary type, and no parquet schema
    /// derives back as `LowCardinality`.
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, LowCardinality(String))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, LowCardinality(String))", "Nullable(String)"));
    EXPECT_TRUE(matchesStrictly(
        "SimpleAggregateFunction(anyLast, LowCardinality(Nullable(String)))", "Nullable(String)"));

    /// Types parquet has no logical type for, written as the nearest thing it does have.
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "Date32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Date)", "UInt16"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime)", "DateTime64(3, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime)", "UInt32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(0))", "DateTime64(3, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(4))", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, DateTime64(7))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time)", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time64(3))", "DateTime64(6, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Time64(9))", "DateTime64(9, 'UTC')"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum8('a' = 1))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum8('a' = 1))", "Int8"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "String"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Enum16('a' = 1))", "Int16"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv4)", "UInt32"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, IPv6)", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, Int128)", "FixedString(16)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, UInt256)", "FixedString(32)"));
    EXPECT_TRUE(matchesStrictly("SimpleAggregateFunction(anyLast, FixedString(16))", "String"));
}
