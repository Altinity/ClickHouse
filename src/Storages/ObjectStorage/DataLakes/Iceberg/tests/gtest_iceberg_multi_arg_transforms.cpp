#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Common/tests/gtest_global_context.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeString.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Utils.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

using namespace DB;
using namespace DB::Iceberg;

namespace DB::ErrorCodes
{
extern const int ICEBERG_SPECIFICATION_VIOLATION;
}

namespace
{

/// Build metadata with three columns (a: int id=1, b: int id=2, c: int id=3)
/// and a configurable sort order.
Poco::JSON::Object::Ptr makeMetadataWithSortOrder(const Poco::JSON::Array::Ptr & sort_fields)
{
    auto metadata = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    metadata->set(f_format_version, 3);
    metadata->set(f_current_schema_id, 0);
    metadata->set(f_last_column_id, 3);

    auto schemas = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto schema = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    schema->set(f_schema_id, 0);
    schema->set(f_type, "struct");

    auto fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    for (Int32 id = 1; id <= 3; ++id)
    {
        auto field = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
        field->set(f_id, id);
        field->set(f_name, String(1, static_cast<char>('a' + id - 1)));
        field->set(f_required, true);
        field->set(f_type, "int");
        fields->add(field);
    }
    schema->set(f_fields, fields);
    schemas->add(schema);
    metadata->set(f_schemas, schemas);

    auto sort_order = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    sort_order->set(f_order_id, static_cast<Int64>(1));
    sort_order->set(f_fields, sort_fields);

    auto sort_orders = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_orders->add(sort_order);
    metadata->set(f_sort_orders, sort_orders);
    metadata->set(f_default_sort_order_id, static_cast<Int64>(1));

    return metadata;
}

/// Make an identity sort field referencing a single column by source-id.
Poco::JSON::Object::Ptr makeIdentitySortField(Int64 source_id, const String & direction = "asc")
{
    auto sf = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    sf->set(f_source_id, source_id);
    sf->set("transform", "identity");
    sf->set("direction", direction);
    sf->set("null-order", "nulls-first");
    return sf;
}

/// Make a multi-arg sort field (e.g. bucket[16]) referencing multiple source-ids.
Poco::JSON::Object::Ptr makeMultiArgSortField(
    const std::vector<Int64> & source_ids, const String & transform = "bucket[16]", const String & direction = "asc")
{
    auto sf = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    auto ids = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    for (auto id : source_ids)
        ids->add(id);
    sf->set(f_source_ids, ids);
    sf->set("transform", transform);
    sf->set("direction", direction);
    sf->set("null-order", "nulls-first");
    return sf;
}

NamesAndTypesList threeIntColumns()
{
    return NamesAndTypesList{
        NameAndTypePair("a", std::make_shared<DataTypeInt32>()),
        NameAndTypePair("b", std::make_shared<DataTypeInt32>()),
        NameAndTypePair("c", std::make_shared<DataTypeInt32>()),
    };
}

}

/// --- Sorting key prefix tests ---

TEST(IcebergMultiArgTransforms, SortKeyMultiArgFirstFieldReturnsEmpty)
{
    /// Sort order: [bucket[16](a, b) ASC, c ASC]
    /// The first field is multi-arg and unrepresentable, so the entire key must be empty
    /// (cannot skip to c — that is not a valid prefix of the sort order).
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_fields->add(makeMultiArgSortField({1, 2}));
    sort_fields->add(makeIdentitySortField(3));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    EXPECT_EQ(key.definition_ast, nullptr);
}


TEST(IcebergMultiArgTransforms, SortKeyMultiArgLastFieldKeepsPrefix)
{
    /// Sort order: [c ASC, bucket[16](a, b) ASC]
    /// The prefix before the multi-arg field is (c ASC) — that must be kept.
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_fields->add(makeIdentitySortField(3));
    sort_fields->add(makeMultiArgSortField({1, 2}));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    ASSERT_NE(key.definition_ast, nullptr);
    EXPECT_EQ(key.column_names.size(), 1);
    EXPECT_EQ(key.column_names[0], "c");
}


TEST(IcebergMultiArgTransforms, SortKeyMultiArgMiddleFieldKeepsOnlyPrefix)
{
    /// Sort order: [a ASC, bucket[16](a, b) ASC, c ASC]
    /// Only (a ASC) is a valid prefix.
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_fields->add(makeIdentitySortField(1));
    sort_fields->add(makeMultiArgSortField({1, 2}));
    sort_fields->add(makeIdentitySortField(3));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    ASSERT_NE(key.definition_ast, nullptr);
    EXPECT_EQ(key.column_names.size(), 1);
    EXPECT_EQ(key.column_names[0], "a");
}


TEST(IcebergMultiArgTransforms, SortKeyTwoNormalFieldsPrefix)
{
    /// Sort order: [a ASC, c ASC, bucket[16](a, b) ASC]
    /// Prefix is (a ASC, c ASC).
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_fields->add(makeIdentitySortField(1));
    sort_fields->add(makeIdentitySortField(3));
    sort_fields->add(makeMultiArgSortField({1, 2}));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    ASSERT_NE(key.definition_ast, nullptr);
    EXPECT_EQ(key.column_names.size(), 2);
    EXPECT_EQ(key.column_names[0], "a");
    EXPECT_EQ(key.column_names[1], "c");
}


TEST(IcebergMultiArgTransforms, SortKeyAllNormalFieldsUnchanged)
{
    /// Sort order: [a ASC, b ASC, c ASC] — no multi-arg at all.
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    sort_fields->add(makeIdentitySortField(1));
    sort_fields->add(makeIdentitySortField(2));
    sort_fields->add(makeIdentitySortField(3));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    ASSERT_NE(key.definition_ast, nullptr);
    EXPECT_EQ(key.column_names.size(), 3);
}


/// --- Empty source-ids validation ---

TEST(IcebergMultiArgTransforms, SortFieldMissingSourceIdBreaksSortKey)
{
    /// A sort field with neither source-id nor source-ids: break stops processing.
    auto sort_fields = Poco::JSON::Array::Ptr(new Poco::JSON::Array);
    auto bad_field = Poco::JSON::Object::Ptr(new Poco::JSON::Object);
    bad_field->set("transform", "identity");
    bad_field->set("direction", "asc");
    bad_field->set("null-order", "nulls-first");
    sort_fields->add(bad_field);
    sort_fields->add(makeIdentitySortField(3));

    auto metadata = makeMetadataWithSortOrder(sort_fields);
    auto key = getSortingKeyDescriptionFromMetadata(metadata, threeIntColumns(), getContext().context);
    /// The first field has no source-id/source-ids -> break -> empty key.
    EXPECT_EQ(key.definition_ast, nullptr);
}

#endif
