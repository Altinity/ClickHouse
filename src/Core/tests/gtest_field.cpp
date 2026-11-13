#include <gtest/gtest.h>
#include <Common/Exception.h>
#include <Core/Field.h>

namespace DB::ErrorCodes
{
    extern const int ARGUMENT_OUT_OF_BOUND;
}

using namespace DB;

GTEST_TEST(Field, FromBool)
{
    {
        Field f{false};
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.get<UInt64>(), 0);
        ASSERT_EQ(f.get<bool>(), false);
    }

    {
        Field f{true};
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.get<UInt64>(), 1);
        ASSERT_EQ(f.get<bool>(), true);
    }

    {
        Field f;
        f = false;
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.get<UInt64>(), 0);
        ASSERT_EQ(f.get<bool>(), false);
    }

    {
        Field f;
        f = true;
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.get<UInt64>(), 1);
        ASSERT_EQ(f.get<bool>(), true);
    }
}


GTEST_TEST(Field, Move)
{
    Field f;

    f = Field{String{"Hello, world (1)"}};
    ASSERT_EQ(f.get<String>(), "Hello, world (1)");
    f = Field{String{"Hello, world (2)"}};
    ASSERT_EQ(f.get<String>(), "Hello, world (2)");
    f = Field{Array{Field{String{"Hello, world (3)"}}}};
    ASSERT_EQ(f.get<Array>()[0].get<String>(), "Hello, world (3)");
    f = String{"Hello, world (4)"};
    ASSERT_EQ(f.get<String>(), "Hello, world (4)");
    f = Array{Field{String{"Hello, world (5)"}}};
    ASSERT_EQ(f.get<Array>()[0].get<String>(), "Hello, world (5)");
    f = Array{String{"Hello, world (6)"}};
    ASSERT_EQ(f.get<Array>()[0].get<String>(), "Hello, world (6)");
}


/// The exponent form `Decimal64_'1e-1000'` builds a `DecimalField` with scale 1000 (the
/// `e`/`E` branch of `readDigits` bypasses the closing-quote check the plain-fraction form hits),
/// which an unfixed reader would hand to the decimal text writer. The scale must be rejected.
GTEST_TEST(Field, RestoreFromDumpDecimalOutOfRangeScaleThrows)
{
    try
    {
        Field::restoreFromDump("Decimal64_'1e-1000'");
        FAIL() << "expected ARGUMENT_OUT_OF_BOUND";
    }
    catch (const Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::ARGUMENT_OUT_OF_BOUND);
    }

    /// An in-range exponent-form scale still round-trips.
    Field restored = Field::restoreFromDump("Decimal64_'1e-5'");
    ASSERT_EQ(restored.safeGet<DecimalField<Decimal64>>().getScale(), 5u);
}
