#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/tests/cas_test_helpers.h>
#include <gtest/gtest.h>

using namespace DB::Cas;

namespace
{

enum class Fruit : uint8_t
{
    Apple = 0,
    Pear = 1,
    Plum = 2,
};

constexpr EnumWireTable<Fruit, 3> fruits{{{
    {Fruit::Apple, "apple"},
    {Fruit::Pear, "pear"},
    {Fruit::Plum, "plum"},
}}};

static_assert(fruits.denseAndOrdered());
static_assert(fruits.wordsUnique());
static_assert(casEnumTableCoversEnum<fruits, Fruit>());

/// A one-based dense enum exercises the index arithmetic from the first entry's value.
enum class Grade : uint8_t
{
    Low = 1,
    Mid = 2,
    High = 3,
};

constexpr EnumWireTable<Grade, 3> grades{{{
    {Grade::Low, "low"},
    {Grade::Mid, "mid"},
    {Grade::High, "high"},
}}};

static_assert(grades.denseAndOrdered());
static_assert(casEnumTableCoversEnum<grades, Grade>());

}

TEST(CASEnumWireTable, RoundTripsEveryEntryBothWays)
{
    for (const auto & e : fruits.entries)
    {
        EXPECT_EQ(fruits.toWord(e.value, "fruits"), e.word);
        EXPECT_EQ(fruits.fromWord(e.word, "fruits"), e.value);
    }
    for (const auto & e : grades.entries)
        EXPECT_EQ(grades.fromWord(grades.toWord(e.value, "grades"), "grades"), e.value);
}

TEST(CASEnumWireTable, FailsClosedBothWays)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { fruits.fromWord("banana", "fruits"); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { fruits.toWord(static_cast<Fruit>(99), "fruits"); });
}
