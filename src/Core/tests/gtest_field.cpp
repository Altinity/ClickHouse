#include <gtest/gtest.h>
#include <Common/Exception.h>
#include <Core/Field.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

namespace DB::ErrorCodes
{
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
    writeBinary(size_t(1), wb);
    wb.finalize();

    ReadBufferFromString rb(wb.str());
    try
    {
        Array value;
        readBinary(value, rb);
        FAIL() << "expected TOO_DEEP_RECURSION";
    }
    catch (const Exception & e)
    {
        ASSERT_EQ(e.code(), ErrorCodes::TOO_DEEP_RECURSION);
    }

    /// A shallow array still round-trips.
    WriteBufferFromOwnString ok_wb;
    writeBinary(Array{Field(UInt64(1)), Field(UInt64(2))}, ok_wb);
    ok_wb.finalize();
    ReadBufferFromString ok_rb(ok_wb.str());
    Array roundtrip;
    readBinary(roundtrip, ok_rb);
    ASSERT_EQ(roundtrip.size(), 2u);
}
