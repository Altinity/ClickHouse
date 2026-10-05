#include <Common/AntalyaErrorCodes.h>
#include <Common/ErrorCodes.h>
#include <Common/Exception.h>
#include <Columns/IColumn.h>
#include <DataTypes/IDataType.h>
#include <Interpreters/BackgroundSchedulePoolLog.h>
#include <Interpreters/PartLog.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <stdexcept>

namespace DB::ErrorCodes
{
extern const int CATALOG_NAMESPACE_DISABLED;
extern const int CAS_DELETE_MARKER;
extern const int BAD_ARGUMENTS;
}

namespace
{
struct ExpectedError
{
    int code;
    std::string_view name;
};

constexpr auto expected_antalya_errors = std::to_array<ExpectedError>({
#define M(ID, NAME) ExpectedError{DB::ErrorCodes::ANTALYA_ERROR_CODE_BASE + ID, #NAME},
    APPLY_FOR_ANTALYA_ERROR_CODES(M)
#undef M
});
}

TEST(ErrorCodes, AntalyaIdentity)
{
    EXPECT_EQ(DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED, 10001);
    EXPECT_EQ(DB::ErrorCodes::CAS_DELETE_MARKER, 10006);
    for (const auto & entry : expected_antalya_errors)
    {
        EXPECT_EQ(DB::ErrorCodes::getName(entry.code), entry.name);
        EXPECT_EQ(DB::ErrorCodes::getErrorCodeByName(entry.name), entry.code);
    }
    EXPECT_EQ(DB::ErrorCodes::getErrorCodeByName("BAD_ARGUMENTS"), DB::ErrorCodes::BAD_ARGUMENTS);
    for (int code : {-1, 10000, 10007, std::numeric_limits<int>::max()})
        EXPECT_TRUE(DB::ErrorCodes::getName(code).empty());
    EXPECT_THROW(DB::ErrorCodes::getErrorCodeByName("NOT_A_REGISTERED_ERROR"), DB::Exception);
}

TEST(ErrorCodes, ExceptionWireIdentity)
{
    for (int code : {10001, 10006, 10007, -1, std::numeric_limits<int>::max()})
    {
        auto original = DB::Exception::createDeprecated("wire identity", code);
        DB::WriteBufferFromOwnString output;
        DB::writeException(original, output, false);
        DB::ReadBufferFromString input(output.str());
        auto decoded = DB::readException(input, "", true);
        EXPECT_EQ(decoded.code(), code);
        EXPECT_NE(decoded.message().find("wire identity"), std::string::npos);
    }
}

TEST(ErrorCodes, CompactEnumerationAndCounters)
{
    ASSERT_EQ(DB::ErrorCodes::size(), DB::ErrorCodes::end() + expected_antalya_errors.size());
    EXPECT_LT(DB::ErrorCodes::size(), DB::ErrorCodes::CATALOG_NAMESPACE_DISABLED);
    EXPECT_TRUE(DB::ErrorCodes::getName(DB::ErrorCodes::end() - 1).empty());
    EXPECT_THROW(DB::ErrorCodes::getCode(DB::ErrorCodes::size()), std::out_of_range);
    EXPECT_THROW(DB::ErrorCodes::getValue(DB::ErrorCodes::size()), std::out_of_range);

    for (size_t slot = 0; slot < DB::ErrorCodes::size(); ++slot)
    {
        const auto code = DB::ErrorCodes::getCode(slot);
        const auto name = DB::ErrorCodes::getName(code);
        if (!name.empty())
            EXPECT_EQ(DB::ErrorCodes::getErrorCodeByName(name), code);
        if (slot < static_cast<size_t>(DB::ErrorCodes::end()))
            EXPECT_EQ(code, slot);
        else
        {
            const auto & expected = expected_antalya_errors.at(slot - DB::ErrorCodes::end());
            EXPECT_EQ(code, expected.code);
            EXPECT_EQ(name, expected.name);
        }

        if (name.empty())
            continue;
        for (bool remote : {false, true})
        {
            const auto before = DB::ErrorCodes::getValue(slot).get();
            const auto index = DB::ErrorCodes::increment(code, remote, "slot test", "slot test", {});
            DB::ErrorCodes::extendedMessage(code, remote, index, "slot test extended");
            const auto after = DB::ErrorCodes::getValue(slot).get();
            EXPECT_EQ(after.local.count, before.local.count + !remote);
            EXPECT_EQ(after.remote.count, before.remote.count + remote);
            EXPECT_EQ((remote ? after.remote : after.local).message, "slot test extended");
        }
    }
}

TEST(ErrorCodes, UnknownAccountingDoesNotPolluteAntalya)
{
    const auto vendor_slot = DB::ErrorCodes::size() - 1;
    const auto before = DB::ErrorCodes::getValue(vendor_slot).get();
    const auto sentinel = DB::ErrorCodes::end() - 1;
    for (int code : {-1, DB::ErrorCodes::end(), 10000, 10007, std::numeric_limits<int>::max()})
    {
        const auto count = DB::ErrorCodes::getValue(sentinel).get().local.count;
        const auto index = DB::ErrorCodes::increment(code, false, "unknown", "unknown", {});
        DB::ErrorCodes::extendedMessage(code, false, index, "unknown extended");
        const auto value = DB::ErrorCodes::getValue(sentinel).get().local;
        EXPECT_EQ(value.count, count + 1);
        EXPECT_EQ(value.message, "unknown extended");
    }
    for (int code : {779, DB::ErrorCodes::end() - 2})
    {
        ASSERT_GE(code, 0);
        ASSERT_LT(code, DB::ErrorCodes::end());
        const auto ordinary_before = DB::ErrorCodes::getValue(code).get().local.count;
        DB::ErrorCodes::increment(code, false, "ordinary", "ordinary", {});
        EXPECT_EQ(DB::ErrorCodes::getValue(code).get().local.count, ordinary_before + 1);
    }

    const auto after = DB::ErrorCodes::getValue(vendor_slot).get();
    EXPECT_EQ(before.local.count, after.local.count);
    EXPECT_EQ(before.remote.count, after.remote.count);
}

TEST(ErrorCodes, AntalyaCodesSurviveNarrowLogColumns)
{
    auto check_log = [](auto element, int code)
    {
        element.error = static_cast<UInt16>(code);
        DB::MutableColumns columns;
        size_t error_column = 0;
        for (const auto & column : element.getColumnsDescription().getAllPhysical())
        {
            if (column.name == "error")
                error_column = columns.size();
            columns.push_back(column.type->createColumn());
        }
        element.appendToBlock(columns);
        EXPECT_EQ(columns[error_column]->getUInt(0), code);
        EXPECT_EQ(DB::ErrorCodes::getName(static_cast<int>(columns[error_column]->getUInt(0))), DB::ErrorCodes::getName(code));
    };
    for (size_t slot = DB::ErrorCodes::end(); slot < DB::ErrorCodes::size(); ++slot)
    {
        const auto code = DB::ErrorCodes::getCode(slot);
        check_log(DB::PartLogElement{}, code);
        check_log(DB::BackgroundSchedulePoolLogElement{}, code);
    }
}
