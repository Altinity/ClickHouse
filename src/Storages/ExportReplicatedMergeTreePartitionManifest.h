#pragma once

#include <base/types.h>
#include <Interpreters/StorageID.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>

namespace DB
{

struct ExportReplicatedMergeTreePartitionManifest
{
    String transaction_id;
    String partition_id;
    String destination_database;
    String destination_table;
    String source_replica;
    size_t number_of_parts;
    std::vector<String> parts;
    time_t create_time;

    std::string toJsonString() const
    {
        Poco::JSON::Object json;
        json.set("transaction_id", transaction_id);
        json.set("partition_id", partition_id);
        json.set("destination_database", destination_database);
        json.set("destination_table", destination_table);
        json.set("source_replica", source_replica);
        json.set("number_of_parts", number_of_parts);
        
        Poco::JSON::Array::Ptr parts_array = new Poco::JSON::Array();
        for (const auto & part : parts)
            parts_array->add(part);
        json.set("parts", parts_array);
        
        json.set("create_time", create_time);
        std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        oss.exceptions(std::ios::failbit);
        Poco::JSON::Stringifier::stringify(json, oss);
        return oss.str();
    }

    static ExportReplicatedMergeTreePartitionManifest fromJsonString(const std::string & json_string)
    {
        Poco::JSON::Parser parser;
        auto json = parser.parse(json_string).extract<Poco::JSON::Object::Ptr>();
        chassert(json);

        ExportReplicatedMergeTreePartitionManifest manifest;
        manifest.transaction_id = json->getValue<String>("transaction_id");
        manifest.partition_id = json->getValue<String>("partition_id");
        manifest.destination_database = json->getValue<String>("destination_database");
        manifest.destination_table = json->getValue<String>("destination_table");
        manifest.source_replica = json->getValue<String>("source_replica");
        manifest.number_of_parts = json->getValue<size_t>("number_of_parts");
        
        auto parts_array = json->getArray("parts");
        for (size_t i = 0; i < parts_array->size(); ++i)
            manifest.parts.push_back(parts_array->getElement<String>(static_cast<unsigned int>(i)));
        
        manifest.create_time = json->getValue<time_t>("create_time");
        return manifest;
    }
};

}
