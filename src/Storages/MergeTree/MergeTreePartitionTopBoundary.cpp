#include <Storages/MergeTree/MergeTreePartitionTopBoundary.h>

#include <Common/DateLUT.h>
#include <Common/DateLUTImpl.h>
#include <Common/Exception.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDate32.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <Parsers/ASTExpressionList.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Storages/KeyDescription.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_TTL_EXPRESSION;
    extern const int LOGICAL_ERROR;
}

namespace MergeTreePartitionTopBoundary
{

namespace
{

enum class PartitionKind : uint8_t
{
    IdentityDate,        /// PARTITION BY <Date>             ; partition_id = yyyymmdd
    IdentityDate32,      /// PARTITION BY <Date32>           ; partition_id = yyyymmdd
    IdentityDateTime,    /// PARTITION BY <DateTime>         ; partition_id = unix seconds
    IdentityDateTime64,  /// PARTITION BY <DateTime64(p)>    ; partition_id = raw underlying (rare)
    ToYear,              /// partition_id = YYYY
    ToYYYYMM,            /// partition_id = YYYYMM
    ToYYYYMMDD,          /// partition_id = YYYYMMDD
    ToStartOfYear,       /// output Date(start of year)       -> yyyymmdd
    ToStartOfQuarter,    /// output Date(start of quarter)    -> yyyymmdd
    ToStartOfMonth,      /// output Date(start of month)      -> yyyymmdd
    ToStartOfWeek,       /// output Date(Sunday)              -> yyyymmdd
    ToMonday,            /// output Date(Monday)              -> yyyymmdd
    ToStartOfDay,        /// output DateTime(start of day)    -> unix seconds
    ToStartOfHour,       /// output DateTime(start of hour)   -> unix seconds
    ToStartOfMinute,     /// output DateTime(start of minute) -> unix seconds
};

struct Classification
{
    PartitionKind kind;
};

std::optional<Classification> classify(const KeyDescription & partition_key)
{
    if (!partition_key.expression_list_ast)
        return {};

    const auto & children = partition_key.expression_list_ast->children;
    if (children.size() != 1)
        return {};

    if (partition_key.sample_block.columns() != 1)
        return {};

    const auto & sample_type = partition_key.sample_block.getByPosition(0).type;
    const auto & node = children[0];

    /// Identity partition keys: a bare column name.
    if (const auto * ident = node->as<ASTIdentifier>())
    {
        (void)ident;
        if (typeid_cast<const DataTypeDate *>(sample_type.get()))
            return Classification{PartitionKind::IdentityDate};
        if (typeid_cast<const DataTypeDate32 *>(sample_type.get()))
            return Classification{PartitionKind::IdentityDate32};
        if (typeid_cast<const DataTypeDateTime *>(sample_type.get()))
            return Classification{PartitionKind::IdentityDateTime};
        if (typeid_cast<const DataTypeDateTime64 *>(sample_type.get()))
            return Classification{PartitionKind::IdentityDateTime64};
        return {};
    }

    /// Function-based partition keys: must be a single-argument call on a column.
    if (const auto * func = node->as<ASTFunction>())
    {
        if (!func->arguments || func->arguments->children.size() != 1)
            return {};
        if (!func->arguments->children[0]->as<ASTIdentifier>())
            return {};

        const String & name = func->name;
        if (name == "toYear")
            return Classification{PartitionKind::ToYear};
        if (name == "toYYYYMM")
            return Classification{PartitionKind::ToYYYYMM};
        if (name == "toYYYYMMDD")
            return Classification{PartitionKind::ToYYYYMMDD};
        if (name == "toStartOfYear")
            return Classification{PartitionKind::ToStartOfYear};
        if (name == "toStartOfQuarter")
            return Classification{PartitionKind::ToStartOfQuarter};
        if (name == "toStartOfMonth")
            return Classification{PartitionKind::ToStartOfMonth};
        if (name == "toStartOfWeek")
            return Classification{PartitionKind::ToStartOfWeek};
        if (name == "toMonday")
            return Classification{PartitionKind::ToMonday};
        if (name == "toStartOfDay")
            return Classification{PartitionKind::ToStartOfDay};
        if (name == "toStartOfHour")
            return Classification{PartitionKind::ToStartOfHour};
        if (name == "toStartOfMinute")
            return Classification{PartitionKind::ToStartOfMinute};
    }

    return {};
}

UInt64 parseUnsigned(const String & partition_id)
{
    ReadBufferFromString buf(partition_id);
    UInt64 value;
    readText(value, buf);
    assertEOF(buf);
    return value;
}

/// Convert yyyymmdd to (year, month, day) tuple.
struct YMD { Int16 year; UInt8 month; UInt8 day; };

YMD parseYYYYMMDD(UInt64 yyyymmdd)
{
    return YMD{
        .year = static_cast<Int16>(yyyymmdd / 10000),
        .month = static_cast<UInt8>((yyyymmdd / 100) % 100),
        .day = static_cast<UInt8>(yyyymmdd % 100),
    };
}

time_t makeDateTimeAtEndOfDay(Int16 year, UInt8 month, UInt8 day)
{
    const auto & lut = DateLUT::serverTimezoneInstance();
    time_t start_of_day = lut.makeDateTime(year, month, day, 0, 0, 0);
    return start_of_day + 86400 - 1;
}

}

bool isPartitionExpressionSupported(const KeyDescription & partition_key)
{
    return classify(partition_key).has_value();
}

void checkPartitionExpressionSupported(const KeyDescription & partition_key)
{
    if (!isPartitionExpressionSupported(partition_key))
    {
        throw Exception(ErrorCodes::BAD_TTL_EXPRESSION,
            "TTL EXPORT requires the PARTITION BY expression to be one of: identity Date/Date32/DateTime/DateTime64 column, "
            "toYear, toYYYYMM, toYYYYMMDD, toMonday, toStartOf{{Year,Quarter,Month,Week,Day,Hour,Minute}}.");
    }
}

time_t computeTopBoundary(const KeyDescription & partition_key, const String & partition_id)
{
    const auto cls = classify(partition_key);
    if (!cls)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "computeTopBoundary called for unsupported partition expression");

    const auto & lut = DateLUT::serverTimezoneInstance();
    const UInt64 raw = parseUnsigned(partition_id);

    switch (cls->kind)
    {
        case PartitionKind::IdentityDate:
        case PartitionKind::IdentityDate32:
        {
            const auto ymd = parseYYYYMMDD(raw);
            return makeDateTimeAtEndOfDay(ymd.year, ymd.month, ymd.day);
        }
        case PartitionKind::IdentityDateTime:
        case PartitionKind::IdentityDateTime64:
        {
            /// DateTime / DateTime64 identity partitioning groups all rows with the same value into a single
            /// partition. The supremum of that range is the value itself.
            return static_cast<time_t>(raw);
        }
        case PartitionKind::ToYear:
        {
            const Int16 year = static_cast<Int16>(raw);
            return makeDateTimeAtEndOfDay(year, 12, 31);
        }
        case PartitionKind::ToYYYYMM:
        {
            const Int16 year = static_cast<Int16>(raw / 100);
            const UInt8 month = static_cast<UInt8>(raw % 100);
            const UInt8 last_day = lut.daysInMonth(year, month);
            return makeDateTimeAtEndOfDay(year, month, last_day);
        }
        case PartitionKind::ToYYYYMMDD:
        {
            const auto ymd = parseYYYYMMDD(raw);
            return makeDateTimeAtEndOfDay(ymd.year, ymd.month, ymd.day);
        }
        case PartitionKind::ToStartOfYear:
        {
            const auto ymd = parseYYYYMMDD(raw);
            /// Range is the whole year.
            return makeDateTimeAtEndOfDay(ymd.year, 12, 31);
        }
        case PartitionKind::ToStartOfQuarter:
        {
            const auto ymd = parseYYYYMMDD(raw);
            /// Range is the whole quarter (3 months starting from the start month).
            const UInt8 last_month_in_quarter = static_cast<UInt8>(ymd.month + 2);
            const UInt8 last_day = lut.daysInMonth(ymd.year, last_month_in_quarter);
            return makeDateTimeAtEndOfDay(ymd.year, last_month_in_quarter, last_day);
        }
        case PartitionKind::ToStartOfMonth:
        {
            const auto ymd = parseYYYYMMDD(raw);
            const UInt8 last_day = lut.daysInMonth(ymd.year, ymd.month);
            return makeDateTimeAtEndOfDay(ymd.year, ymd.month, last_day);
        }
        case PartitionKind::ToStartOfWeek:
        case PartitionKind::ToMonday:
        {
            const auto ymd = parseYYYYMMDD(raw);
            const time_t start_of_day = lut.makeDateTime(ymd.year, ymd.month, ymd.day, 0, 0, 0);
            /// One full week = 7 days.
            return start_of_day + 7 * 86400 - 1;
        }
        case PartitionKind::ToStartOfDay:
        {
            return static_cast<time_t>(raw) + 86400 - 1;
        }
        case PartitionKind::ToStartOfHour:
        {
            return static_cast<time_t>(raw) + 3600 - 1;
        }
        case PartitionKind::ToStartOfMinute:
        {
            return static_cast<time_t>(raw) + 60 - 1;
        }
    }

    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unreachable partition kind in computeTopBoundary");
}

time_t addInterval(time_t top_boundary, IntervalKind kind, Int64 count)
{
    const auto & lut = DateLUT::serverTimezoneInstance();
    switch (kind.kind)
    {
        case IntervalKind::Kind::Nanosecond:
        case IntervalKind::Kind::Microsecond:
        case IntervalKind::Kind::Millisecond:
            /// Sub-second intervals round to 0 seconds; this is fine for export TTL which works in seconds.
            return top_boundary;
        case IntervalKind::Kind::Second:
            return top_boundary + count;
        case IntervalKind::Kind::Minute:
            return top_boundary + count * 60;
        case IntervalKind::Kind::Hour:
            return top_boundary + count * 3600;
        case IntervalKind::Kind::Day:
            return lut.addDays(top_boundary, count);
        case IntervalKind::Kind::Week:
            return lut.addWeeks(top_boundary, count);
        case IntervalKind::Kind::Month:
            return lut.addMonths(static_cast<time_t>(top_boundary), count);
        case IntervalKind::Kind::Quarter:
            return lut.addMonths(static_cast<time_t>(top_boundary), count * 3);
        case IntervalKind::Kind::Year:
            return lut.addYears(static_cast<time_t>(top_boundary), count);
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Unknown IntervalKind {}", static_cast<int>(kind.kind));
}

int comparePartitionIds(const KeyDescription & partition_key, const String & a, const String & b)
{
    const auto cls = classify(partition_key);
    if (!cls)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "comparePartitionIds called for unsupported partition expression");

    const UInt64 va = parseUnsigned(a);
    const UInt64 vb = parseUnsigned(b);
    if (va < vb) return -1;
    if (va > vb) return 1;
    return 0;
}

}

}
