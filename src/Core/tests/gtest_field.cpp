#include <gtest/gtest.h>
#include <Common/Exception.h>
#include <Core/Field.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

namespace DB::ErrorCodes
{
    extern const int ARGUMENT_OUT_OF_BOUND;
    extern const int TOO_DEEP_RECURSION;
}

using namespace DB;

GTEST_TEST(Field, FromBool)
{
    {
        Field f{false};
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.safeGet<UInt64>(), 0);
        ASSERT_EQ(f.safeGet<bool>(), false);
    }

    {
        Field f{true};
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.safeGet<UInt64>(), 1);
        ASSERT_EQ(f.safeGet<bool>(), true);
    }

    {
        Field f;
        f = false;
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.safeGet<UInt64>(), 0);
        ASSERT_EQ(f.safeGet<bool>(), false);
    }

    {
        Field f;
        f = true;
        ASSERT_EQ(f.getType(), Field::Types::Bool);
        ASSERT_EQ(f.safeGet<UInt64>(), 1);
        ASSERT_EQ(f.safeGet<bool>(), true);
    }
}


GTEST_TEST(Field, Move)
{
    Field f;

    f = Field{String{"Hello, world (1)"}};
    ASSERT_EQ(f.safeGet<String>(), "Hello, world (1)");
    f = Field{String{"Hello, world (2)"}};
    ASSERT_EQ(f.safeGet<String>(), "Hello, world (2)");
    f = Field{Array{Field{String{"Hello, world (3)"}}}};
    ASSERT_EQ(f.safeGet<Array>()[0].safeGet<String>(), "Hello, world (3)");
    f = String{"Hello, world (4)"};
    ASSERT_EQ(f.safeGet<String>(), "Hello, world (4)");
    f = Array{Field{String{"Hello, world (5)"}}};
    ASSERT_EQ(f.safeGet<Array>()[0].safeGet<String>(), "Hello, world (5)");
    f = Array{String{"Hello, world (6)"}};
    ASSERT_EQ(f.safeGet<Array>()[0].safeGet<String>(), "Hello, world (6)");
}


/// The text `Field` dump reader `restoreFromDump` shares the out-of-range-scale problem with the
/// binary reader. The exponent form `Decimal64_'1e-1000'` builds a `DecimalField` with scale 1000 (the
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


/// Deeply nested binary Fields must raise TOO_DEEP_RECURSION.
GTEST_TEST(Field, ReadBinaryDeeplyNestedThrowsNotOverflow)
{
    WriteBufferFromOwnString wb;
    for (size_t i = 0; i < 500000; ++i)
    {
        writeBinary(UInt8(Field::Types::Array), wb);
        writeBinary(size_t(1), wb); /// one child
    }
    writeBinary(UInt8(Field::Types::Null), wb); /// leaf
    wb.finalize();

    ReadBufferFromString rb(wb.str());
    try
    {
        readFieldBinary(rb);
        FAIL() << "expected TOO_DEEP_RECURSION";
    }
    catch (const Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::TOO_DEEP_RECURSION);
    }

    /// A shallow array still round-trips.
    WriteBufferFromOwnString ok_wb;
    writeFieldBinary(Field(Array{Field(UInt64(1)), Field(UInt64(2))}), ok_wb);
    ok_wb.finalize();
    ReadBufferFromString ok_rb(ok_wb.str());
    Field roundtrip = readFieldBinary(ok_rb);
    ASSERT_EQ(roundtrip.safeGet<Array>().size(), 2u);
}
