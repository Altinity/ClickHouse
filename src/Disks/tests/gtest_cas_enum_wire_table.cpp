#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/tests/cas_test_helpers.h>
#include <base/defines.h>
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

TEST(CASEnumWireTable, FromWordFailsClosed)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { fruits.fromWord("banana", "fruits"); });
}

/// `LOGICAL_ERROR` aborts the process in debug/sanitizer builds (`handle_error_code`), so the
/// defensive toWord branch needs the death-test split this test directory already uses (see
/// `gtest_cas_gc_state_format.cpp`'s `RejectsZeroGcShardsOnEncode` pair) — a bare EXPECT_THROW
/// would SIGABRT the whole gate binary on those lanes.
#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASEnumWireTableDeathTest, ToWordAbortsOnOutOfRangeValue)
{
    EXPECT_DEATH(fruits.toWord(static_cast<Fruit>(99), "fruits"), "outside the wire vocabulary");
}
#else
TEST(CASEnumWireTable, ToWordThrowsLogicalErrorOnOutOfRangeValue)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { fruits.toWord(static_cast<Fruit>(99), "fruits"); });
}
#endif

/// The compile-time proofs must also be exercised in the direction where they can fail — a
/// predicate rewritten to `return true;` must break this file. All three are constexpr, so the
/// negative cases are plain static_asserts over deliberately bad tables:
namespace bad_tables
{

enum class Sparse : uint8_t { A = 0, B = 2 };
constexpr EnumWireTable<Sparse, 2> sparse{{{{Sparse::A, "a"}, {Sparse::B, "b"}}}};
static_assert(!sparse.denseAndOrdered());

constexpr EnumWireTable<Fruit, 3> dup_words{{{
    {Fruit::Apple, "apple"}, {Fruit::Pear, "apple"}, {Fruit::Plum, "plum"}}}};
static_assert(!dup_words.wordsUnique());

constexpr EnumWireTable<Fruit, 3> dup_value{{{
    {Fruit::Apple, "apple"}, {Fruit::Apple, "pear"}, {Fruit::Plum, "plum"}}}};
static_assert(!casEnumTableCoversEnum<dup_value, Fruit>());

constexpr EnumWireTable<Fruit, 3> invalid_value{{{
    {Fruit::Apple, "apple"}, {Fruit::Pear, "pear"}, {static_cast<Fruit>(99), "plum"}}}};
static_assert(!casEnumTableCoversEnum<invalid_value, Fruit>());

}
