#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>

#include <Columns/ColumnAggregateFunction.h>

#include <Common/SipHash.h>
#include <Common/AlignedBuffer.h>
#include <Common/quoteString.h>
#include <Common/FieldVisitorToString.h>

#include <Formats/FormatSettings.h>
#include <DataTypes/DataTypeAggregateFunction.h>
#include <DataTypes/DataTypeCustomSimpleAggregateFunction.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeFixedString.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeTime64.h>
#include <DataTypes/DataTypeTuple.h>
#include <DataTypes/Serializations/SerializationAggregateFunction.h>
#include <DataTypes/DataTypeFactory.h>
#include <DataTypes/transformTypesRecursively.h>
#include <Common/FieldVisitorToCastedLiteral.h>
#include <Parsers/parseFieldFromCastedLiteral.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <IO/Operators.h>

#include <AggregateFunctions/AggregateFunctionFactory.h>
#include <AggregateFunctions/IAggregateFunction.h>
#include <Parsers/ASTDataType.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/ASTIdentifier_fwd.h>
#include <Parsers/ASTLiteral.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int SYNTAX_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int LOGICAL_ERROR;
}


DataTypeAggregateFunction::DataTypeAggregateFunction(AggregateFunctionPtr function_, const DataTypes & argument_types_,
                            const Array & parameters_, std::optional<size_t> version_)
    : function(std::move(function_))
    , argument_types(argument_types_)
    , parameters(parameters_)
    , version(version_)
{
}

String DataTypeAggregateFunction::getFunctionName() const
{
    return function->getName();
}


String DataTypeAggregateFunction::doGetName() const
{
    return getNameImpl(true, false);
}


String DataTypeAggregateFunction::getNameWithoutVersion() const
{
    return getNameImpl(false, false);
}


String DataTypeAggregateFunction::getNameForAnnotation() const
{
    return getNameImpl(true, true);
}


size_t DataTypeAggregateFunction::getVersion() const
{
    if (version)
        return *version;
    return function->getDefaultVersion();
}

DataTypePtr DataTypeAggregateFunction::getReturnType() const
{
    return function->getResultType();
}

DataTypePtr DataTypeAggregateFunction::getReturnTypeToPredict() const
{
    return function->getReturnTypeToPredict();
}

bool DataTypeAggregateFunction::isVersioned() const
{
    return function->isVersioned();
}

void DataTypeAggregateFunction::updateVersionFromRevision(size_t revision, bool if_empty) const
{
    setVersion(function->getVersionFromRevision(revision), if_empty);
}

String DataTypeAggregateFunction::getNameImpl(bool with_version, bool always_emit_version) const
{
    WriteBufferFromOwnString stream;
    stream << "AggregateFunction(";

    /// If aggregate function does not support versioning its version is 0 and is not printed.
    /// always_emit_version keeps an explicit 0 for a versioned function, which getName() would drop,
    /// making it indistinguishable from the default version. Non-versioned functions still omit it.
    auto data_type_version = getVersion();
    if (with_version && (data_type_version || (always_emit_version && isVersioned())))
        stream << data_type_version << ", ";
    stream << function->getName();

    if (!parameters.empty())
    {
        stream << '(';
        if (function->shouldPrintParametersWithTypes())
        {
            FieldVisitorToCastedLiteral visitor;
            for (size_t i = 0, size = parameters.size(); i < size; ++i)
            {
                if (i)
                    stream << ", ";
                stream << applyVisitor(visitor, parameters[i]);
            }
        }
        else
        {
            FieldVisitorToString visitor;
            for (size_t i = 0, size = parameters.size(); i < size; ++i)
            {
                if (i)
                    stream << ", ";
                stream << applyVisitor(visitor, parameters[i]);
            }
        }
        stream << ')';
    }

    for (const auto & argument_type : argument_types)
        stream << ", " << argument_type->getName();

    stream << ')';
    return stream.str();
}


MutableColumnPtr DataTypeAggregateFunction::createColumn() const
{
    return ColumnAggregateFunction::create(function, getVersion());
}


/// Create empty state
Field DataTypeAggregateFunction::getDefault() const
{
    Field field = AggregateFunctionStateData();
    field.safeGet<AggregateFunctionStateData>().name = getName();

    AlignedBuffer place_buffer(function->sizeOfData(), function->alignOfData());
    AggregateDataPtr place = place_buffer.data();

    function->create(place);

    try
    {
        WriteBufferFromString buffer_from_field(field.safeGet<AggregateFunctionStateData>().data);
        function->serialize(place, buffer_from_field, version);
    }
    catch (...)
    {
        function->destroy(place);
        throw;
    }

    function->destroy(place);

    return field;
}

bool DataTypeAggregateFunction::strictEquals(const DataTypePtr & lhs_state_type, const DataTypePtr & rhs_state_type, bool ignore_variant)
{
    const auto * lhs_state = typeid_cast<const DataTypeAggregateFunction *>(lhs_state_type.get());
    const auto * rhs_state = typeid_cast<const DataTypeAggregateFunction *>(rhs_state_type.get());

    if (!lhs_state || !rhs_state)
        return false;

    if (!ignore_variant && lhs_state->function->getStateVariant() != rhs_state->function->getStateVariant())
        return false;

    if (lhs_state->function->getName() != rhs_state->function->getName())
        return false;

    if (lhs_state->parameters.size() != rhs_state->parameters.size())
        return false;

    for (size_t i = 0; i < lhs_state->parameters.size(); ++i)
        if (lhs_state->parameters[i] != rhs_state->parameters[i])
            return false;

    if (lhs_state->argument_types.size() != rhs_state->argument_types.size())
        return false;

    for (size_t i = 0; i < lhs_state->argument_types.size(); ++i)
        if (!lhs_state->argument_types[i]->equals(*rhs_state->argument_types[i]))
            return false;

    return true;
}

void DataTypeAggregateFunction::updateHashImpl(SipHash & hash) const
{
    hash.update(getFunctionName());
    hash.update(parameters.size());
    for (const auto & param : parameters)
        hash.update(param.getType());
    hash.update(argument_types.size());
    for (const auto & arg_type : argument_types)
        arg_type->updateHash(hash);
    if (version)
        hash.update(*version);
    hash.update(static_cast<UInt8>(function->getStateVariant()));
}

bool DataTypeAggregateFunction::equalsIgnoringVariant(const IDataType & rhs) const
{
    if (typeid(rhs) != typeid(*this))
        return false;

    auto lhs_state_type = function->getNormalizedStateType();
    auto rhs_state_type = typeid_cast<const DataTypeAggregateFunction &>(rhs).function->getNormalizedStateType();

    return strictEquals(lhs_state_type, rhs_state_type, /*ignore_variant=*/ true);
}

bool DataTypeAggregateFunction::equals(const IDataType & rhs) const
{
    if (typeid(rhs) != typeid(*this))
        return false;

    auto lhs_state_type = function->getNormalizedStateType();
    auto rhs_state_type = typeid_cast<const DataTypeAggregateFunction &>(rhs).function->getNormalizedStateType();

    return strictEquals(lhs_state_type, rhs_state_type);
}


SerializationPtr DataTypeAggregateFunction::doGetSerialization(const SerializationInfoSettings &) const
{
    return SerializationAggregateFunction::create(function, getName(), getVersion());
}


namespace
{

/// Extract a single AggregateFunction parameter value from its AST node.
Field parseAggregateFunctionParameter(const ASTPtr & param_ast, const String & function_name)
{
    try
    {
        return parseFieldFromCastedLiteral(param_ast);
    }
    catch (Exception & e)
    {
        e.addMessage("while parsing aggregate function '{}'", function_name);
        throw;
    }
}

}

static DataTypePtr create(const ASTPtr & arguments)
{
    String function_name;
    DataTypes argument_types;
    Array params_row;
    std::optional<size_t> version;

    if (!arguments || arguments->children.empty())
        throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                        "Data type AggregateFunction requires parameters: "
                        "version(optionally), name of aggregate function and list of data types for arguments");

    ASTPtr data_type_ast = arguments->children[0];
    size_t argument_types_start_idx = 1;

    /* If aggregate function definition doesn't have version, it will have in AST children args [ASTFunction, types...] - in case
     * it is parametric, or [ASTIdentifier, types...] - otherwise. If aggregate function has version in AST, then it will be:
     * [ASTLiteral, ASTFunction (or ASTIdentifier), types...].
     */
    if (auto * version_ast = arguments->children[0]->as<ASTLiteral>())
    {
        if (arguments->children.size() < 2)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH,
                "Data type AggregateFunction has version, but it requires at least one more parameter - name of aggregate function");
        version = version_ast->value.safeGet<UInt64>();
        data_type_ast = arguments->children[1];
        argument_types_start_idx = 2;
    }

    auto action = NullsAction::EMPTY;
    if (const auto * parametric = data_type_ast->as<ASTFunction>())
    {
        if (parametric->parameters)
            throw Exception(ErrorCodes::SYNTAX_ERROR, "Unexpected level of parameters to aggregate function");

        function_name = parametric->name;
        action = parametric->getNullsAction();

        if (parametric->arguments)
        {
            const ASTs & parameters = parametric->arguments->children;
            params_row.resize(parameters.size());

            for (size_t i = 0; i < parameters.size(); ++i)
                params_row[i] = parseAggregateFunctionParameter(parameters[i], function_name);
        }
    }
    else if (auto opt_name = tryGetIdentifierName(data_type_ast))
    {
        function_name = *opt_name;
    }
    else if (data_type_ast->as<ASTLiteral>())
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Aggregate function name for data type AggregateFunction must "
                        "be passed as identifier (without quotes) or function");
    }
    else
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "Unexpected AST element {} passed as aggregate function name for data type AggregateFunction. "
                        "Must be identifier or function", data_type_ast->getID());

    for (size_t i = argument_types_start_idx; i < arguments->children.size(); ++i)
        argument_types.push_back(DataTypeFactory::instance().get(arguments->children[i]));

    if (function_name.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Empty name of aggregate function passed");

    AggregateFunctionProperties properties;
    AggregateFunctionPtr function = AggregateFunctionFactory::instance().get(function_name, action, argument_types, params_row, properties);
    return std::make_shared<DataTypeAggregateFunction>(function, argument_types, params_row, version);
}

void setVersionToAggregateFunctions(DataTypePtr & type, bool if_empty, std::optional<size_t> revision)
{
    auto callback = [revision, if_empty](DataTypePtr & column_type)
    {
        const auto * aggregate_function_type = typeid_cast<const DataTypeAggregateFunction *>(column_type.get());
        if (aggregate_function_type && aggregate_function_type->isVersioned())
        {
            if (revision)
                aggregate_function_type->updateVersionFromRevision(*revision, if_empty);
            else
                aggregate_function_type->setVersion(0, if_empty);
        }
    };

    callOnNestedSimpleTypes(type, callback);
}


void registerDataTypeAggregateFunction(DataTypeFactory & factory)
{
    factory.registerDataType("AggregateFunction", create, DataTypeFactory::Case::Sensitive, Documentation{
            .description = R"DOCS_MD(
## Description {#description}

All [Aggregate functions](/sql-reference/aggregate-functions) in ClickHouse have
an implementation-specific intermediate state that can be serialized to an
`AggregateFunction` data type and stored in a table. This is usually done by
means of a [materialized view](../../sql-reference/statements/create/view.md).

There are two aggregate function [combinators](/sql-reference/aggregate-functions/combinators)
commonly used with the `AggregateFunction` type:

- The [`-State`](/sql-reference/aggregate-functions/combinators#-state) aggregate function combinator, which when appended to an aggregate
function name, produces `AggregateFunction` intermediate states.
- The [`-Merge`](/sql-reference/aggregate-functions/combinators#-merge) aggregate
function combinator, which is used to get the final result of an aggregation
from the intermediate states.

## Syntax {#syntax}

```sql
AggregateFunction(aggregate_function_name, types_of_arguments...)
```

**Parameters**

- `aggregate_function_name` - The name of an aggregate function. If the function
is parametric, then its parameters should be specified too.
- `types_of_arguments` - The types of the aggregate function arguments.

for example:

```sql
CREATE TABLE t
(
    column1 AggregateFunction(uniq, UInt64),
    column2 AggregateFunction(anyIf, String, UInt8),
    column3 AggregateFunction(quantiles(0.5, 0.9), UInt64)
) ENGINE = ...
```

## Usage {#usage}

### Data Insertion {#data-insertion}

To insert data into a table with columns of type `AggregateFunction`, you can
use `INSERT SELECT` with aggregate functions and the
[`-State`](/sql-reference/aggregate-functions/combinators#-state) aggregate
function combinator.

For example, to insert into columns of type `AggregateFunction(uniq, UInt64)` and
`AggregateFunction(quantiles(0.5, 0.9), UInt64)` you would use the following
aggregate functions with combinators.

```sql
uniqState(UserID)
quantilesState(0.5, 0.9)(SendTiming)
```

In contrast to functions `uniq` and `quantiles`, `uniqState` and `quantilesState`
(with `-State` combinator appended) return the state, rather than the final value.
In other words, they return a value of `AggregateFunction` type.

In the results of the `SELECT` query, values of type `AggregateFunction` have
implementation-specific binary representations for all of the ClickHouse output
formats.

There is a special Session level setting `aggregate_function_input_format` that allows to build state from the input values.
It supports the following formats:

- `state` - binary string with the serialized state (the default).
If you dump data into, for example, the `TabSeparated` format with a `SELECT`
query, then this dump can be loaded back using the `INSERT` query.
- `value` - the format will expect a single value of the argument of the aggregate function, or in the case of multiple arguments, a tuple of them; that will be deserialized to form the relevant state
- `array` - the format will expect an Array of values, as described in the values option above; all the elements of the array will be aggregated to form the state

### Data Selection {#data-selection}

When selecting data from `AggregatingMergeTree` table, use the `GROUP BY` clause
and the same aggregate functions as for when you inserted the data, but use the
[`-Merge`](/sql-reference/aggregate-functions/combinators#-merge) combinator.

An aggregate function with the `-Merge` combinator appended to it takes a set of
states, combines them, and returns the result of the complete data aggregation.

For example, the following two queries return the same result:

```sql
SELECT uniq(UserID) FROM table

SELECT uniqMerge(state) FROM (SELECT uniqState(UserID) AS state FROM table GROUP BY RegionID)
```

## Usage Example {#usage-example}

See [AggregatingMergeTree](../../engines/table-engines/mergetree-family/aggregatingmergetree.md) engine description.

## Related Content {#related-content}

- Blog: [Using Aggregate Combinators in ClickHouse](https://clickhouse.com/blog/aggregate-functions-combinators-in-clickhouse-for-arrays-maps-and-states)
- [MergeState](/sql-reference/aggregate-functions/combinators#-mergestate)
combinator.
- [State](/sql-reference/aggregate-functions/combinators#-state) combinator.
)DOCS_MD",
            .syntax = "AggregateFunction(name, types...)",
            .examples = {},
            .related = {"SimpleAggregateFunction"},
        });
}

bool hasAggregateFunctionType(const DataTypePtr & type)
{
    auto result = false;
    auto check = [&](const IDataType & t)
    {
        result |= WhichDataType(t).isAggregateFunction();
    };

    check(*type);
    type->forEachChild(check);
    return result;
}

bool astHasAggregateFunctionType(const ASTPtr & ast)
{
    /// `ParserDataType` represents a type name with arguments as `ASTDataType` and a bare one as
    /// `ASTIdentifier`. The comparison is exact because `AggregateFunction` is registered
    /// case-sensitively and has no alias.
    std::string_view name;
    if (const auto * data_type = ast->as<ASTDataType>())
        name = data_type->name;
    else if (const auto * identifier = ast->as<ASTIdentifier>())
        name = identifier->name();

    if (name == "AggregateFunction")
        return true;

    for (const auto & child : ast->children)
        if (astHasAggregateFunctionType(child))
            return true;

    return false;
}

bool needsClickHouseTypeAnnotation(const DataTypePtr & type)
{
    auto result = false;
    auto check = [&](const IDataType & t)
    {
        if (WhichDataType(t).isAggregateFunction())
        {
            result = true;
            return;
        }
        /// `SimpleAggregateFunction(f, T)` is `T` carrying an `IDataTypeCustomName`.
        result |= typeid_cast<const DataTypeCustomSimpleAggregateFunction *>(t.getCustomName()) != nullptr;
    };

    check(*type);
    type->forEachChild(check);
    return result;
}

String getClickHouseTypeAnnotationName(const DataTypePtr & type)
{
    /// `SimpleAggregateFunction(f, T)` has no version and its `T` cannot hold an `AggregateFunction`,
    /// so getName() already round-trips it.
    if (type->getCustomName())
        return type->getName();

    switch (type->getTypeId())
    {
        case TypeIndex::AggregateFunction:
            return assert_cast<const DataTypeAggregateFunction &>(*type).getNameForAnnotation();
        case TypeIndex::Array:
            return "Array(" + getClickHouseTypeAnnotationName(assert_cast<const DataTypeArray &>(*type).getNestedType()) + ")";
        case TypeIndex::Map:
        {
            const auto & map_type = assert_cast<const DataTypeMap &>(*type);
            return "Map(" + getClickHouseTypeAnnotationName(map_type.getKeyType()) + ", "
                + getClickHouseTypeAnnotationName(map_type.getValueType()) + ")";
        }
        case TypeIndex::Tuple:
        {
            const auto & tuple_type = assert_cast<const DataTypeTuple &>(*type);
            const auto & elements = tuple_type.getElements();
            const auto & names = tuple_type.getElementNames();
            WriteBufferFromOwnString stream;
            stream << "Tuple(";
            for (size_t i = 0; i < elements.size(); ++i)
            {
                if (i)
                    stream << ", ";
                if (tuple_type.hasExplicitNames())
                    stream << backQuoteIfNeed(names[i]) << ' ';
                stream << getClickHouseTypeAnnotationName(elements[i]);
            }
            stream << ")";
            return stream.str();
        }
        default:
            return type->getName();
    }
}

namespace
{

/// Neither format's schema inference derives a `LowCardinality`, and either side may carry a
/// `Nullable` the other does not (an OPTIONAL parquet leaf always derives as one). Neither says
/// anything about the values, so both wrappers come off before the types are compared.
DataTypePtr removeNullableAndLowCardinality(const DataTypePtr & type)
{
    return removeNullable(removeLowCardinality(type));
}

bool isFixedStringOfSize(const DataTypePtr & type, size_t size)
{
    const auto * fixed_string = typeid_cast<const DataTypeFixedString *>(type.get());
    return fixed_string && fixed_string->getN() == size;
}

bool isDateTime64WithScale(const DataTypePtr & type, UInt32 scale)
{
    const auto * date_time64 = typeid_cast<const DataTypeDateTime64 *>(type.get());
    return date_time64 && date_time64->getScale() == scale;
}

/// True when `derived`, the type a parquet schema alone reads as, is one this server's parquet writer
/// can produce for a column of type `annotated`. Both arguments are free of `Nullable` and
/// `LowCardinality`.
///
/// A directed relation rather than an equality: the writer's mapping in `preparePrimitiveColumn` is
/// not injective, since parquet has no logical type for several ClickHouse types and a few
/// `output_format_parquet_*` settings, not recorded in the file, pick between representations. Each
/// case below mirrors one of those. Where the mapping is unclear the check is deliberately generous:
/// a pair wrongly accepted only honours an annotation that would have been honoured anyway, while a
/// pair wrongly rejected refuses a file that reads fine.
bool parquetWriterCouldProduce(const DataTypePtr & annotated, const DataTypePtr & derived)
{
    if (annotated->equals(*derived))
        return true;

    /// Parquet timestamps come in milli-, micro- and nanoseconds only, so a scale in between is
    /// written scaled up to the next unit and reads back with that unit's scale.
    auto next_timestamp_unit = [](UInt32 scale) -> UInt32
    {
        if (scale <= 3)
            return 3;
        if (scale <= 6)
            return 6;
        return 9;
    };

    switch (annotated->getTypeId())
    {
        /// Parquet has no 16-bit date, so `Date` is written as a DATE, deriving as `Date32` - or, with
        /// `output_format_parquet_date_as_uint16`, as a UINT_16.
        case TypeIndex::Date:
            return WhichDataType(derived).isDate32() || WhichDataType(derived).isUInt16();
        /// No second-resolution timestamp, so `DateTime` is written as TIMESTAMP_MILLIS, deriving as
        /// `DateTime64(3)` - or, with `output_format_parquet_datetime_as_uint32`, as a UINT_32.
        case TypeIndex::DateTime:
            return isDateTime64WithScale(derived, 3) || WhichDataType(derived).isUInt32();
        case TypeIndex::DateTime64:
            return isDateTime64WithScale(
                derived, next_timestamp_unit(assert_cast<const DataTypeDateTime64 &>(*annotated).getScale()));
        /// TIME is written with the timestamp units and derives as `DateTime64`, like TIMESTAMP.
        /// Second-resolution `Time` is written as micros.
        case TypeIndex::Time:
            return isDateTime64WithScale(derived, 6);
        case TypeIndex::Time64:
            return isDateTime64WithScale(
                derived, assert_cast<const DataTypeTime64 &>(*annotated).getScale() <= 6 ? 6 : 9);
        /// An enum is written as an ENUM byte array, deriving as `String` - or, with
        /// `output_format_parquet_enum_as_byte_array` off, as its underlying integer.
        case TypeIndex::Enum8:
            return isString(derived) || WhichDataType(derived).isInt8();
        case TypeIndex::Enum16:
            return isString(derived) || WhichDataType(derived).isInt16();
        /// No logical type for these, so they are written as raw bytes: `IPv4` as a plain UINT_32,
        /// the rest as a FIXED_LEN_BYTE_ARRAY of their width.
        case TypeIndex::IPv4:
            return WhichDataType(derived).isUInt32();
        case TypeIndex::IPv6:
        case TypeIndex::UInt128:
        case TypeIndex::Int128:
            return isFixedStringOfSize(derived, 16);
        case TypeIndex::UInt256:
        case TypeIndex::Int256:
            return isFixedStringOfSize(derived, 32);
        /// With `output_format_parquet_fixed_string_as_fixed_byte_array` off a `FixedString` is
        /// written as a plain BYTE_ARRAY, which derives as `String`.
        case TypeIndex::FixedString:
            return isString(derived);
        /// A JSON column read with `input_format_parquet_enable_json_parsing` off derives as `String`.
        case TypeIndex::Object:
            return isString(derived);
        default:
            return false;
    }
}

}

bool annotatedTypeMatchesDerived(const DataTypePtr & annotated_type, const DataTypePtr & derived_type, bool strict)
{
    auto annotated = removeNullableAndLowCardinality(annotated_type);
    auto derived = removeNullableAndLowCardinality(derived_type);

    if (annotated->getTypeId() == TypeIndex::AggregateFunction)
        return isString(derived);

    switch (annotated->getTypeId())
    {
        case TypeIndex::Array:
        {
            const auto * derived_array = typeid_cast<const DataTypeArray *>(derived.get());
            if (!derived_array)
                return false;
            return annotatedTypeMatchesDerived(
                assert_cast<const DataTypeArray &>(*annotated).getNestedType(), derived_array->getNestedType(), strict);
        }
        case TypeIndex::Tuple:
        {
            const auto & annotated_tuple = assert_cast<const DataTypeTuple &>(*annotated);
            const auto * derived_tuple = typeid_cast<const DataTypeTuple *>(derived.get());
            if (!derived_tuple || derived_tuple->getElements().size() != annotated_tuple.getElements().size())
                return false;
            for (size_t i = 0; i < annotated_tuple.getElements().size(); ++i)
                if (!annotatedTypeMatchesDerived(annotated_tuple.getElement(i), derived_tuple->getElement(i), strict))
                    return false;
            return true;
        }
        case TypeIndex::Map:
        {
            const auto & annotated_map = assert_cast<const DataTypeMap &>(*annotated);
            const auto * derived_map = typeid_cast<const DataTypeMap *>(derived.get());
            if (!derived_map)
                return false;
            return annotatedTypeMatchesDerived(annotated_map.getKeyType(), derived_map->getKeyType(), strict)
                && annotatedTypeMatchesDerived(annotated_map.getValueType(), derived_map->getValueType(), strict);
        }
        default:
            return !strict || parquetWriterCouldProduce(annotated, derived);
    }
}

}
