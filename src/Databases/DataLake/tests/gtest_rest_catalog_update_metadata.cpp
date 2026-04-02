#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>
#include <Databases/DataLake/RestCatalog.h>
#include <Storages/ObjectStorage/DataLakes/Iceberg/Constant.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <base/types.h>

using namespace DB;

namespace
{
Poco::JSON::Object::Ptr findUpdateByAction(const Poco::JSON::Array::Ptr & updates, const std::string & action)
{
    for (unsigned int i = 0; i < updates->size(); ++i)
    {
        auto o = updates->getObject(i);
        if (o->getValue<std::string>("action") == action)
            return o;
    }
    return nullptr;
}
}

TEST(RestCatalogUpdateMetadataBody, NullSnapshotSkips)
{
    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", nullptr);
    EXPECT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Skip);
    EXPECT_FALSE(r.request_body);
}

TEST(RestCatalogUpdateMetadataBody, SchemaUpdateValid)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    Poco::JSON::Array::Ptr schemas = new Poco::JSON::Array;
    Poco::JSON::Object::Ptr schema = new Poco::JSON::Object;
    schema->set(Iceberg::f_schema_id, 1);
    schema->set(Iceberg::f_type, "struct");
    schema->set(Iceberg::f_fields, Poco::JSON::Array::Ptr(new Poco::JSON::Array));
    schemas->add(schema);
    snapshot->set(Iceberg::f_schemas, schemas);
    snapshot->set(Iceberg::f_current_schema_id, 1);
    snapshot->set(Iceberg::f_last_column_id, 3);

    auto r = DataLake::buildUpdateMetadataRequestBody("my.ns", "tbl", snapshot);
    ASSERT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Ok);
    ASSERT_TRUE(r.request_body);

    auto id = r.request_body->getObject("identifier");
    EXPECT_EQ(id->getValue<std::string>("name"), "tbl");
    auto ns = id->getArray("namespace");
    ASSERT_EQ(ns->size(), 1u);
    EXPECT_EQ(ns->getElement<std::string>(0), "my.ns");

    ASSERT_TRUE(r.request_body->has("requirements"));
    auto req = r.request_body->getArray("requirements")->getObject(0);
    EXPECT_EQ(req->getValue<std::string>("type"), "assert-current-schema-id");
    EXPECT_EQ(req->getValue<Int32>("current-schema-id"), 0);

    auto updates = r.request_body->getArray("updates");
    auto add_schema = findUpdateByAction(updates, "add-schema");
    ASSERT_TRUE(add_schema);
    EXPECT_TRUE(add_schema->has("schema"));
    EXPECT_EQ(add_schema->getValue<Int32>("last-column-id"), 3);

    auto set_schema = findUpdateByAction(updates, "set-current-schema");
    ASSERT_TRUE(set_schema);
    EXPECT_EQ(set_schema->getValue<Int32>("schema-id"), 1);
}

TEST(RestCatalogUpdateMetadataBody, SchemaUpdateCurrentIdZeroNoRequirement)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    Poco::JSON::Array::Ptr schemas = new Poco::JSON::Array;
    Poco::JSON::Object::Ptr schema = new Poco::JSON::Object;
    schema->set(Iceberg::f_schema_id, 0);
    schema->set(Iceberg::f_type, "struct");
    schema->set(Iceberg::f_fields, Poco::JSON::Array::Ptr(new Poco::JSON::Array));
    schemas->add(schema);
    snapshot->set(Iceberg::f_schemas, schemas);
    snapshot->set(Iceberg::f_current_schema_id, 0);

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    ASSERT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Ok);
    EXPECT_FALSE(r.request_body->has("requirements"));
}

TEST(RestCatalogUpdateMetadataBody, SchemaUpdateMissingCurrentSchemaId)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    snapshot->set(Iceberg::f_schemas, Poco::JSON::Array::Ptr(new Poco::JSON::Array));

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    EXPECT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Error);
    EXPECT_FALSE(r.request_body);
}

TEST(RestCatalogUpdateMetadataBody, SchemaUpdateNoMatchingSchemaId)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    Poco::JSON::Array::Ptr schemas = new Poco::JSON::Array;
    Poco::JSON::Object::Ptr schema = new Poco::JSON::Object;
    schema->set(Iceberg::f_schema_id, 1);
    schema->set(Iceberg::f_type, "struct");
    schema->set(Iceberg::f_fields, Poco::JSON::Array::Ptr(new Poco::JSON::Array));
    schemas->add(schema);
    snapshot->set(Iceberg::f_schemas, schemas);
    snapshot->set(Iceberg::f_current_schema_id, 99);

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    EXPECT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Error);
    EXPECT_FALSE(r.request_body);
}

TEST(RestCatalogUpdateMetadataBody, SnapshotUpdateWithParent)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    snapshot->set("snapshot-id", static_cast<Int64>(12345));
    snapshot->set("parent-snapshot-id", static_cast<Int64>(12344));
    snapshot->set(Iceberg::f_timestamp_ms, static_cast<Int64>(1700000000000LL));

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    ASSERT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Ok);
    ASSERT_TRUE(r.request_body);

    ASSERT_TRUE(r.request_body->has("requirements"));
    auto req = r.request_body->getArray("requirements")->getObject(0);
    EXPECT_EQ(req->getValue<std::string>("type"), "assert-ref-snapshot-id");
    EXPECT_EQ(req->getValue<std::string>("ref"), "main");
    EXPECT_EQ(req->getValue<Int64>("snapshot-id"), 12344);

    auto updates = r.request_body->getArray("updates");
    auto add_snap = findUpdateByAction(updates, "add-snapshot");
    ASSERT_TRUE(add_snap);
    EXPECT_EQ(add_snap->getObject("snapshot")->getValue<Int64>("snapshot-id"), 12345);

    auto set_ref = findUpdateByAction(updates, "set-snapshot-ref");
    ASSERT_TRUE(set_ref);
    EXPECT_EQ(set_ref->getValue<Int64>("snapshot-id"), 12345);
}

TEST(RestCatalogUpdateMetadataBody, SnapshotUpdateWithoutParent)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    snapshot->set("snapshot-id", static_cast<Int64>(999));

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    ASSERT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Ok);
    EXPECT_FALSE(r.request_body->has("requirements"));

    auto updates = r.request_body->getArray("updates");
    ASSERT_TRUE(findUpdateByAction(updates, "add-snapshot"));
    ASSERT_TRUE(findUpdateByAction(updates, "set-snapshot-ref"));
}

TEST(RestCatalogUpdateMetadataBody, SnapshotUpdateParentMinusOneNoRequirement)
{
    Poco::JSON::Object::Ptr snapshot = new Poco::JSON::Object;
    snapshot->set("snapshot-id", static_cast<Int64>(1));
    snapshot->set("parent-snapshot-id", static_cast<Int64>(-1));

    auto r = DataLake::buildUpdateMetadataRequestBody("ns", "t", snapshot);
    ASSERT_EQ(r.status, DataLake::UpdateMetadataRequestBodyResult::Status::Ok);
    EXPECT_FALSE(r.request_body->has("requirements"));
}

#endif
