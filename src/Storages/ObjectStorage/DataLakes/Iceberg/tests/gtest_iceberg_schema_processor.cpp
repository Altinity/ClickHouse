#include <gtest/gtest.h>

#include <Common/tests/gtest_global_context.h>
#include <Common/tests/gtest_global_register.h>
#include <DataTypes/IDataType.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/SchemaProcessor.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include "config.h"
#if USE_AVRO
#include <Storages/ObjectStorage/DataLakes/Iceberg/IcebergMetadata.h>
#endif

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

using namespace DB::Iceberg;

namespace DB::ErrorCodes
{
extern const int ICEBERG_SPECIFICATION_VIOLATION;
extern const int SUPPORT_IS_DISABLED;
}

namespace
{
Poco::JSON::Object::Ptr parseSchema(const std::string & json)
{
    Poco::JSON::Parser parser;
    return parser.parse(json).extract<Poco::JSON::Object::Ptr>();
}

/// The gate is read from the context the schema processor is called with, not one captured at
/// construction, so a test sets it on a copy of the global context and passes that copy in.
DB::ContextMutablePtr contextWithAggregateFunctionStates(bool allow)
{
    auto context = DB::Context::createCopy(getContext().context);
    context->setSetting("allow_experimental_aggregate_function_states_in_iceberg", DB::Field(allow));
    return context;
}

/// An annotation that does not describe the field it sits on must be rejected as malformed metadata,
/// and by that check rather than by some unrelated failure.
void expectAnnotatedSchemaRejected(const Poco::JSON::Object::Ptr & schema)
{
    auto context = contextWithAggregateFunctionStates(true);
    IcebergSchemaProcessor processor(context);
    try
    {
        processor.addIcebergTableSchema(schema, context);
        FAIL() << "The annotation does not describe the field type and must be rejected";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ICEBERG_SPECIFICATION_VIOLATION) << e.message();
    }
}
}

TEST(IcebergSchemaProcessor, GetSimpleTypeBoolean)
{
    auto type = IcebergSchemaProcessor::getSimpleType("boolean", getContext().context);
    EXPECT_EQ(type->getName(), "Bool");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeInt)
{
    auto type = IcebergSchemaProcessor::getSimpleType("int", getContext().context);
    EXPECT_EQ(type->getName(), "Int32");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeLong)
{
    auto type = IcebergSchemaProcessor::getSimpleType("long", getContext().context);
    EXPECT_EQ(type->getName(), "Int64");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeBigint)
{
    auto type = IcebergSchemaProcessor::getSimpleType("bigint", getContext().context);
    EXPECT_EQ(type->getName(), "Int64");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeFloat)
{
    auto type = IcebergSchemaProcessor::getSimpleType("float", getContext().context);
    EXPECT_EQ(type->getName(), "Float32");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeDouble)
{
    auto type = IcebergSchemaProcessor::getSimpleType("double", getContext().context);
    EXPECT_EQ(type->getName(), "Float64");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeDate)
{
    auto type = IcebergSchemaProcessor::getSimpleType("date", getContext().context);
    EXPECT_EQ(type->getName(), "Date32");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeTime)
{
    auto type = IcebergSchemaProcessor::getSimpleType("time", getContext().context);
    EXPECT_EQ(type->getName(), "Time64(6)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeTimestamp)
{
    auto type = IcebergSchemaProcessor::getSimpleType("timestamp", getContext().context);
    EXPECT_EQ(type->getName(), "DateTime64(6)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeTimestamptz)
{
    auto type = IcebergSchemaProcessor::getSimpleType("timestamptz", getContext().context);
    EXPECT_EQ(type->getName(), "DateTime64(6, 'UTC')");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeTimestampNs)
{
    auto type = IcebergSchemaProcessor::getSimpleType("timestamp_ns", getContext().context);
    EXPECT_EQ(type->getName(), "DateTime64(9)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeTimestamptzNs)
{
    auto type = IcebergSchemaProcessor::getSimpleType("timestamptz_ns", getContext().context);
    EXPECT_EQ(type->getName(), "DateTime64(9, 'UTC')");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeString)
{
    auto type = IcebergSchemaProcessor::getSimpleType("string", getContext().context);
    EXPECT_EQ(type->getName(), "String");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeBinary)
{
    auto type = IcebergSchemaProcessor::getSimpleType("binary", getContext().context);
    EXPECT_EQ(type->getName(), "String");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeUuid)
{
    auto type = IcebergSchemaProcessor::getSimpleType("uuid", getContext().context);
    EXPECT_EQ(type->getName(), "UUID");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeFixed)
{
    auto type = IcebergSchemaProcessor::getSimpleType("fixed[16]", getContext().context);
    EXPECT_EQ(type->getName(), "FixedString(16)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeDecimal)
{
    auto type = IcebergSchemaProcessor::getSimpleType("decimal(10, 2)", getContext().context);
    EXPECT_EQ(type->getName(), "Decimal(10, 2)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeUnknownThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("unknown_type", getContext().context), DB::Exception);
}

/// The Iceberg primitive type grammar is a closed set: scalars, decimal(P, S) and fixed[N] whose
/// only parameters are integers, geography/geometry whose parameters are bare identifiers, and the
/// list/map/struct wrappers. None of them carries a quoted string literal. A spelling that embeds
/// one (e.g. "MyType('Hello ( world )')") matches no branch of getSimpleType and is rejected before
/// any comparison runs, so canonicalizeTypeSpacing never sees whitespace inside a quoted literal.
TEST(IcebergSchemaProcessor, GetSimpleTypeWithStringLiteralArgumentThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("MyType('Hello ( world )')", getContext().context), DB::Exception);
}

/// The same string-literal-bearing spelling must be rejected as an initial schema type, i.e. the
/// parser guards the entry point so a quoted literal never reaches the whitespace canonicalization.
TEST(IcebergSchemaProcessor, InitialSchemaTypeWithStringLiteralArgumentThrows)
{
    auto schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"MyType('Hello ( world )')"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(schema, getContext().context), DB::Exception);
}

/// The primitive parser must accept the same inner-whitespace spellings that the
/// whitespace-insensitive comparison treats as equivalent. Without canonicalizing the type string
/// before parsing, readIntText does not skip the leading space, so "decimal( 20, 0 )" and
/// "fixed[ 16 ]" fail to parse even though they denote decimal(20, 0) / fixed[16].
TEST(IcebergSchemaProcessor, GetSimpleTypeDecimalInnerWhitespace)
{
    auto type = IcebergSchemaProcessor::getSimpleType("decimal( 20, 0 )", getContext().context);
    EXPECT_EQ(type->getName(), "Decimal(20, 0)");
}

TEST(IcebergSchemaProcessor, GetSimpleTypeFixedInnerWhitespace)
{
    auto type = IcebergSchemaProcessor::getSimpleType("fixed[ 16 ]", getContext().context);
    EXPECT_EQ(type->getName(), "FixedString(16)");
}

/// Regression test for https://github.com/ClickHouse/ClickHouse/issues/109642
/// The same schema-id can be serialized by different Iceberg writers with different
/// whitespace in parameterized primitive type strings, e.g. the table metadata JSON
/// emits "decimal(20,0)" while the manifest Avro metadata emits "decimal(20, 0)".
/// Both denote the identical type per the Iceberg spec, so re-adding the schema-id
/// must NOT be rejected as a rebinding to a different schema.
TEST(IcebergSchemaProcessor, DecimalTypeWhitespaceIsInsensitive)
{
    auto first = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20,0)"}]})json");
    auto second = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20, 0)"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(second, getContext().context));
}

/// A genuinely different type bound to the same schema-id must still be rejected.
TEST(IcebergSchemaProcessor, RebindingSchemaIdToDifferentTypeStillRejected)
{
    auto first = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20,0)"}]})json");
    auto second = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20,2)"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(second, getContext().context), DB::Exception);
}

/// A renamed field bound to the same schema-id must still be rejected (issue #107316).
TEST(IcebergSchemaProcessor, RebindingSchemaIdToRenamedFieldStillRejected)
{
    auto first = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"long"}]})json");
    auto second = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c9","required":false,"type":"long"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(second, getContext().context), DB::Exception);
}

/// The whitespace-insensitive comparison must reach into list/map wrappers: the nested
/// element/key/value primitive types (here list<decimal>) can also be serialized with
/// different spacing across metadata files.
TEST(IcebergSchemaProcessor, ListElementDecimalWhitespaceIsInsensitive)
{
    auto first = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"list","element-id":2,"element-required":false,"element":"decimal(20,0)"}}]})json");
    auto second = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"list","element-id":2,"element-required":false,"element":"decimal(20, 0)"}}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(second, getContext().context));
}

/// Same for map key/value primitive types (here map<decimal, decimal>).
TEST(IcebergSchemaProcessor, MapKeyValueDecimalWhitespaceIsInsensitive)
{
    auto first = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"map","key-id":2,"key":"decimal(20,0)","value-id":3,"value-required":false,"value":"decimal(10,2)"}}]})json");
    auto second = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"map","key-id":2,"key":"decimal(20, 0)","value-id":3,"value-required":false,"value":"decimal(10, 2)"}}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(second, getContext().context));
}

/// The Iceberg geography/geometry primitives carry parameters too, e.g.
/// "geography(crs, algorithm)", so their serialization can also differ by whitespace
/// across metadata files. With the geo parser enabled, re-adding the same schema-id with
/// different spacing must not be rejected.
TEST(IcebergSchemaProcessor, GeographyTypeWhitespaceIsInsensitive)
{
    auto first = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"geography(C,A)"}]})json");
    auto second = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"geography(C, A)"}]})json");
    IcebergSchemaProcessor processor(getContext().context, /*allow_geo_parser_=*/true);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(second, getContext().context));
}

/// A geo type string carrying leading/trailing whitespace must map to its alias just like the
/// space-free spelling. The alias prefix match (geography -> binary) runs on the canonicalized
/// spelling, so " geography(C,A)" and "geography(C, A)" under the same schema-id compare equal
/// instead of one skipping aliasing (staying "geography") and the other becoming "binary".
TEST(IcebergSchemaProcessor, GeographyTypeEdgeWhitespaceIsInsensitive)
{
    auto first = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":" geography(C,A) "}]})json");
    auto second = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"geography(C, A)"}]})json");
    IcebergSchemaProcessor processor(getContext().context, /*allow_geo_parser_=*/true);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(second, getContext().context));
}

/// Schema-evolution path: renaming a geo field across two schema-ids while only changing the
/// whitespace of its parameterized type string must resolve to a rename, so the transform DAG
/// exposes the NEW column name. Without whitespace-insensitive comparison the old node is kept
/// unchanged and the DAG would still expose the old name.
TEST(IcebergSchemaProcessor, RenameGeoFieldAcrossSchemaIdsWithWhitespaceIsRename)
{
    auto old_schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"a","required":false,"type":"geography(C,A)"}]})json");
    auto new_schema = parseSchema(R"json({"schema-id":1,"fields":[{"id":1,"name":"b","required":false,"type":"geography(C, A)"}]})json");
    IcebergSchemaProcessor processor(getContext().context, /*allow_geo_parser_=*/true);
    processor.addIcebergTableSchema(old_schema, getContext().context);
    processor.addIcebergTableSchema(new_schema, getContext().context);

    auto dag = processor.getSchemaTransformationDagByIds(getContext().context, 0, 1);
    ASSERT_TRUE(dag);
    const auto & outputs = dag->getOutputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0]->result_name, "b");
}

/// A whitespace-heavy type string must be accepted in the INITIAL/current schema (not just the
/// repeated-same-schema-id path): the parser runs before any comparison, so it has to tolerate the
/// same spellings on its own.
TEST(IcebergSchemaProcessor, InitialSchemaDecimalInnerWhitespaceAccepted)
{
    auto schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal( 20, 0 )"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    EXPECT_NO_THROW(processor.addIcebergTableSchema(schema, getContext().context));
}

/// Schema-evolution across two schema-ids where a decimal widens (allowed conversion) while its
/// type string also carries inner whitespace. allowPrimitiveTypeConversion must canonicalize the
/// spacing so the widening is still recognized and the DAG casts to the new type under the new name.
TEST(IcebergSchemaProcessor, WidenDecimalAcrossSchemaIdsWithInnerWhitespace)
{
    auto old_schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(10,2)"}]})json");
    auto new_schema = parseSchema(R"json({"schema-id":1,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal( 20, 2 )"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(old_schema, getContext().context);
    processor.addIcebergTableSchema(new_schema, getContext().context);

    auto dag = processor.getSchemaTransformationDagByIds(getContext().context, 0, 1);
    ASSERT_TRUE(dag);
    const auto & outputs = dag->getOutputs();
    ASSERT_EQ(outputs.size(), 1u);
    EXPECT_EQ(outputs[0]->result_type->getName(), "Nullable(Decimal(20, 2))");
}

/// A genuinely different nested type inside a list wrapper must still be rejected.
TEST(IcebergSchemaProcessor, RebindingListElementToDifferentTypeStillRejected)
{
    auto first = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"list","element-id":2,"element-required":false,"element":"decimal(20,0)"}}]})json");
    auto second = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":{"type":"list","element-id":2,"element-required":false,"element":"decimal(20,2)"}}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(first, getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(second, getContext().context), DB::Exception);
}

/// Spacing normalization only removes whitespace adjacent to the delimiters '(', ')', '[', ']', ','.
/// Whitespace embedded inside a numeric token is not formatting, so malformed spellings such as
/// "decimal(2 0,0)" or "fixed[1 6]" must NOT canonicalize to a valid type and must still be rejected.
TEST(IcebergSchemaProcessor, GetSimpleTypeDecimalMalformedInnerTokenWhitespaceThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("decimal(2 0,0)", getContext().context), DB::Exception);
}

TEST(IcebergSchemaProcessor, GetSimpleTypeFixedMalformedInnerTokenWhitespaceThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("fixed[1 6]", getContext().context), DB::Exception);
}

/// The same malformed spelling must be rejected when it appears as an initial schema type, i.e. the
/// broadened normalization must not let invalid metadata pass through addIcebergTableSchema.
TEST(IcebergSchemaProcessor, InitialSchemaDecimalMalformedInnerTokenWhitespaceThrows)
{
    auto schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(2 0,0)"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(schema, getContext().context), DB::Exception);
}

/// Trailing garbage after the scale token must be rejected. Canonicalizing spacing does not remove
/// whitespace between two digits, so "decimal(20,0 0)" keeps the embedded space; the parser must not
/// stop after reading the scale and silently ignore the rest. This mirrors the fixed[N] handling.
TEST(IcebergSchemaProcessor, GetSimpleTypeDecimalTrailingGarbageInScaleThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("decimal(20,0 0)", getContext().context), DB::Exception);
}

/// The same malformed scale spelling must be rejected as an initial schema type.
TEST(IcebergSchemaProcessor, InitialSchemaDecimalTrailingGarbageInScaleThrows)
{
    auto schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20,0 0)"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(schema, getContext().context), DB::Exception);
}

/// A new schema-id introduced during evolution is parsed at add time (getSimpleType runs on every
/// field), so a malformed scale in the new schema is rejected when the new schema is added and never
/// reaches the evolution DAG. The old, valid schema-id remains added.
TEST(IcebergSchemaProcessor, SchemaEvolutionDecimalTrailingGarbageInScaleThrows)
{
    auto old_schema = parseSchema(R"json({"schema-id":0,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(10,2)"}]})json");
    auto new_schema = parseSchema(R"json({"schema-id":1,"fields":[{"id":1,"name":"c0","required":false,"type":"decimal(20,2 2)"}]})json");
    IcebergSchemaProcessor processor(getContext().context);
    processor.addIcebergTableSchema(old_schema, getContext().context);
    EXPECT_THROW(processor.addIcebergTableSchema(new_schema, getContext().context), DB::Exception);
}

/// A missing scale ("decimal(20,)") or a sign-only scale ("decimal(20,+)") is malformed metadata and
/// must be rejected, not silently read as scale 0. The scale is parsed with readIntText, which throws
/// at end of buffer or on a non-digit, matching how the precision is parsed.
TEST(IcebergSchemaProcessor, GetSimpleTypeDecimalEmptyScaleThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("decimal(20,)", getContext().context), DB::Exception);
}

TEST(IcebergSchemaProcessor, GetSimpleTypeDecimalSignOnlyScaleThrows)
{
    EXPECT_THROW(IcebergSchemaProcessor::getSimpleType("decimal(20,+)", getContext().context), DB::Exception);
}

/// An optional field derives as `Nullable(String)`, and `AggregateFunction` cannot be inside
/// `Nullable`, so the annotation is returned exactly as it stands.
TEST(IcebergSchemaProcessor, AnnotatedAggregateFunctionFieldIsHonoured)
{
    tryRegisterAggregateFunctions();
    auto schema = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"u","required":false,"type":"binary","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json");
    auto context = contextWithAggregateFunctionStates(true);
    IcebergSchemaProcessor processor(context);
    processor.addIcebergTableSchema(schema, context);

    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 1u);
    EXPECT_EQ(columns->front().name, "u");
    EXPECT_EQ(columns->front().type->getName(), "AggregateFunction(uniq, UInt64)");
}

/// Without the opt-in the same annotation must be refused, rather than fall back to the derived
/// `Nullable(String)`: reading the states as strings would be a wrong result, not an error.
TEST(IcebergSchemaProcessor, AnnotatedAggregateFunctionFieldIsRefusedWithoutTheSetting)
{
    tryRegisterAggregateFunctions();
    auto schema = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"u","required":false,"type":"binary","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json");
    auto context = contextWithAggregateFunctionStates(false);
    IcebergSchemaProcessor processor(context);
    try
    {
        processor.addIcebergTableSchema(schema, context);
        FAIL() << "The annotation names an aggregate function and must not be honoured without the setting";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::SUPPORT_IS_DISABLED) << e.message();
    }
}

/// `SimpleAggregateFunction` stays ungated: the field holds ordinary values and no state
/// deserializer is involved.
TEST(IcebergSchemaProcessor, AnnotatedSimpleAggregateFunctionFieldNeedsNoSetting)
{
    tryRegisterAggregateFunctions();
    auto schema = parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"s","required":true,"type":"long","clickhouse.type":"SimpleAggregateFunction(sum, Int64)"}]})json");
    auto context = contextWithAggregateFunctionStates(false);
    IcebergSchemaProcessor processor(context);
    processor.addIcebergTableSchema(schema, context);

    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 1u);
    EXPECT_EQ(columns->front().name, "s");
    EXPECT_EQ(columns->front().type->getName(), "SimpleAggregateFunction(sum, Int64)");
}

/// The value in force is the one on the context of the call, not one frozen into the processor. A
/// table engine builds its processor at `ATTACH`, so only a per-call read lets a query opt in at all.
TEST(IcebergSchemaProcessor, AnnotatedAggregateFunctionFieldFollowsTheCallingContext)
{
    tryRegisterAggregateFunctions();
    IcebergSchemaProcessor processor(getContext().context);

    auto allowed_context = contextWithAggregateFunctionStates(true);
    processor.addIcebergTableSchema(
        parseSchema(
            R"json({"schema-id":0,"fields":[{"id":1,"name":"u","required":false,"type":"binary","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json"),
        allowed_context);
    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 1u);
    EXPECT_EQ(columns->front().type->getName(), "AggregateFunction(uniq, UInt64)");

    auto refused_context = contextWithAggregateFunctionStates(false);
    try
    {
        processor.addIcebergTableSchema(
            parseSchema(
                R"json({"schema-id":1,"fields":[{"id":2,"name":"v","required":false,"type":"binary","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json"),
            refused_context);
        FAIL() << "The setting is off on the context of this call and the annotation must be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::SUPPORT_IS_DISABLED) << e.message();
    }
}

/// A stale or hand-edited annotation must be rejected: a `long` field holds ordinary integers, not
/// serialized states.
TEST(IcebergSchemaProcessor, AnnotationNotMatchingTheFieldTypeIsRejected)
{
    tryRegisterAggregateFunctions();
    expectAnnotatedSchemaRejected(parseSchema(
        R"json({"schema-id":0,"fields":[{"id":1,"name":"u","required":true,"type":"long","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json"));
}

/// `addIcebergTableSchema` publishes a schema-id only once all of its fields have parsed, and drops
/// whatever it wrote if parsing throws. Otherwise the next call for that schema-id would take the
/// "already added" branch and hand out a schema that was never built. The retries below reuse one
/// processor, as a table engine does - it builds its processor once, at `ATTACH`.

/// The aggregate-state gate is the everyday way in: a read is refused, the user enables the setting,
/// and the retry runs against the processor the refused read left behind.
TEST(IcebergSchemaProcessor, RetryAfterRefusedAggregateFunctionStateSchemaSucceeds)
{
    tryRegisterAggregateFunctions();
    const std::string json
        = R"json({"schema-id":0,"fields":[{"id":1,"name":"u","required":false,"type":"binary","clickhouse.type":"AggregateFunction(uniq, UInt64)"}]})json";
    IcebergSchemaProcessor processor(getContext().context);

    auto refused_context = contextWithAggregateFunctionStates(false);
    try
    {
        processor.addIcebergTableSchema(parseSchema(json), refused_context);
        FAIL() << "The annotation names an aggregate function and must be refused without the setting";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::SUPPORT_IS_DISABLED) << e.message();
    }

    /// Nothing of the refused schema may survive the throw.
    EXPECT_FALSE(processor.hasClickhouseTableSchemaById(0));
    EXPECT_FALSE(processor.tryGetFieldCharacteristics(0, 1).has_value());
    EXPECT_FALSE(processor.tryGetColumnIDByName(0, "u").has_value());

    auto allowed_context = contextWithAggregateFunctionStates(true);
    ASSERT_NO_THROW(processor.addIcebergTableSchema(parseSchema(json), allowed_context));
    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 1u);
    EXPECT_EQ(columns->front().name, "u");
    EXPECT_EQ(columns->front().type->getName(), "AggregateFunction(uniq, UInt64)");
}

/// The same invariant without an aggregate state: an Iceberg type string the parser rejects. The
/// first field does parse, so this also pins that the per-field characteristics recorded before the
/// throw are dropped rather than left to shadow the retry, which renames that field.
TEST(IcebergSchemaProcessor, RetryAfterUnparsableTypeUnderTheSameSchemaIdSucceeds)
{
    IcebergSchemaProcessor processor(getContext().context);
    EXPECT_THROW(
        processor.addIcebergTableSchema(
            parseSchema(
                R"json({"schema-id":0,"fields":[{"id":1,"name":"a","required":false,"type":"long"},{"id":2,"name":"b","required":false,"type":"decimal(20,)"}]})json"),
            getContext().context),
        DB::Exception);

    EXPECT_FALSE(processor.hasClickhouseTableSchemaById(0));
    EXPECT_FALSE(processor.tryGetFieldCharacteristics(0, 1).has_value());
    EXPECT_FALSE(processor.tryGetColumnIDByName(0, "a").has_value());

    ASSERT_NO_THROW(processor.addIcebergTableSchema(
        parseSchema(
            R"json({"schema-id":0,"fields":[{"id":1,"name":"a2","required":false,"type":"int"},{"id":2,"name":"b","required":false,"type":"decimal(20,0)"}]})json"),
        getContext().context));

    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 2u);
    EXPECT_EQ(columns->front().name, "a2");
    auto field = processor.getFieldCharacteristics(0, 1);
    EXPECT_EQ(field.name, "a2");
    EXPECT_EQ(field.type->getName(), "Nullable(Int32)");
    EXPECT_FALSE(processor.tryGetColumnIDByName(0, "a").has_value());
}

#if USE_AVRO
/// `IcebergMetadata::backgroundMetadataPrefetcherThread` warms the metadata files cache on a timer,
/// with no query behind it, so it asks for the schema-id with `SchemaParsing::Skip`. That must neither
/// apply the gates `IcebergSchemaProcessor::getFieldType` reads from the query - a background task
/// carries none, so an `AggregateFunction` column would be refused on every period - nor publish a
/// parsed schema, which the processor would then serve to queries that never enabled the setting.
TEST(IcebergSchemaProcessor, SkippingTheSchemaNeitherAppliesTheGateNorPublishesTheSchema)
{
    tryRegisterAggregateFunctions();
    Poco::JSON::Parser parser;
    auto metadata = parser
                        .parse(
                            R"json({"format-version":2,"current-schema-id":0,"schemas":[{"schema-id":0,)json"
                            R"json("fields":[{"id":1,"name":"u","required":false,"type":"binary",)json"
                            R"json("clickhouse.type":"AggregateFunction(uniq, UInt64)"}]}]})json")
                        .extract<Poco::JSON::Object::Ptr>();

    auto log = getLogger("IcebergSchemaProcessorTest");
    auto context = contextWithAggregateFunctionStates(false);
    DB::Iceberg::IcebergSchemaProcessor processor(context);

    /// A query without the setting is refused, which is what the gate is for.
    try
    {
        DB::IcebergMetadata::parseTableSchema(
            metadata, processor, context, log, DB::IcebergMetadata::SchemaParsing::Parse);
        FAIL() << "The annotation names an aggregate function and must be refused without the setting";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::SUPPORT_IS_DISABLED) << e.message();
    }

    /// The prefetcher, asking for the schema-id alone, is not.
    Int32 schema_id = -1;
    ASSERT_NO_THROW(
        schema_id = DB::IcebergMetadata::parseTableSchema(
            metadata, processor, context, log, DB::IcebergMetadata::SchemaParsing::Skip));
    EXPECT_EQ(schema_id, 0);

    /// And it published nothing, so the gate still decides for the queries that follow.
    EXPECT_FALSE(processor.hasClickhouseTableSchemaById(0));
    EXPECT_FALSE(processor.tryGetFieldCharacteristics(0, 1).has_value());
    EXPECT_FALSE(processor.tryGetColumnIDByName(0, "u").has_value());

    try
    {
        DB::IcebergMetadata::parseTableSchema(
            metadata, processor, context, log, DB::IcebergMetadata::SchemaParsing::Parse);
        FAIL() << "Skipping the schema must not have made the annotation acceptable without the setting";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::SUPPORT_IS_DISABLED) << e.message();
    }

    auto allowed_context = contextWithAggregateFunctionStates(true);
    ASSERT_NO_THROW(DB::IcebergMetadata::parseTableSchema(
        metadata, processor, allowed_context, log, DB::IcebergMetadata::SchemaParsing::Parse));
    auto columns = processor.getClickhouseTableSchemaById(0);
    ASSERT_EQ(columns->size(), 1u);
    EXPECT_EQ(columns->front().name, "u");
    EXPECT_EQ(columns->front().type->getName(), "AggregateFunction(uniq, UInt64)");
}
#endif
