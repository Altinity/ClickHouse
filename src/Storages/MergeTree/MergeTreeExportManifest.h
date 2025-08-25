#pragma once

#include <Core/QualifiedTableName.h>
#include <Interpreters/StorageID.h>
#include <Disks/IDisk.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Common/Exception.h>
#include "Storages/MergeTree/RangesInDataPart.h"
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Dynamic/Var.h>
#include <memory>
#include <sstream>

namespace DB
{

/**
 * JSON manifest for exporting a set of parts to object storage.
 * Layout on disk (pretty-printed JSON):
 * {
 *   "transaction_id": "<id>",
 *   "partition_id": "<partition_id>",
 *   "destination": "<database>.<table>",
 *   "parts": [ {"part_name": "name", "remote_path": "path-or-empty"}, ... ]
 * }
 */
struct MergeTreeExportManifest
{
    MergeTreeExportManifest()
        : destination_storage_id(StorageID::createEmpty())
    {}

    struct Item
    {
        String part_name;
        String remote_path; // empty until uploaded
    };

    String transaction_id;
    String partition_id;
    StorageID destination_storage_id;
    std::vector<Item> items;
    bool completed = false;

    std::filesystem::path file_path;
    DiskPtr disk;

    static std::shared_ptr<MergeTreeExportManifest> create(
        const DiskPtr & disk_,
        const String & path_prefix,
        const String & commit_id_,
        const String & partition_id_,
        const StorageID & destination_storage_id_,
        const std::vector<DataPartPtr> & data_parts)
    {
        auto manifest = std::make_shared<MergeTreeExportManifest>();
        manifest->disk = disk_;
        manifest->transaction_id = commit_id_;
        manifest->partition_id = partition_id_;
        manifest->destination_storage_id = destination_storage_id_;
        manifest->file_path = std::filesystem::path(path_prefix) / ("export_partition_" + partition_id_ + "_commit_" + commit_id_ + ".json");
        manifest->items.reserve(data_parts.size());
        for (const auto & data_part : data_parts)
            manifest->items.push_back({data_part->name, ""});
        manifest->write();
        return manifest;
    }

    static std::shared_ptr<MergeTreeExportManifest> read(const DiskPtr & disk_, const String & file_path_)
    {
        auto manifest = std::make_shared<MergeTreeExportManifest>();
        manifest->disk = disk_;
        manifest->file_path = file_path_;

        auto in = disk_->readFile(file_path_, ReadSettings{});

        String json_str;
        readStringUntilEOF(json_str, *in);

        Poco::JSON::Parser parser;
        Poco::Dynamic::Var json = parser.parse(json_str);
        const Poco::JSON::Object::Ptr & root = json.extract<Poco::JSON::Object::Ptr>();
        if (!root)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Invalid export manifest JSON: {}", file_path_);

        manifest->transaction_id = root->getValue<String>("transaction_id");
        manifest->partition_id = root->getValue<String>("partition_id");
        const auto destination = root->getValue<String>("destination");
        manifest->destination_storage_id = StorageID(QualifiedTableName::parseFromString(destination));
        manifest->completed = root->getValue<bool>("completed");

        manifest->items.clear();
        auto parts = root->get("parts").extract<Poco::JSON::Array::Ptr>();
        for (unsigned int i = 0; i < parts->size(); ++i)
        {
            const auto part_obj = parts->getObject(i);
            Item item;
            item.part_name = part_obj->getValue<String>("part_name");
            item.remote_path = part_obj->getValue<String>("remote_path");
            manifest->items.push_back(std::move(item));
        }

        return manifest;
    }

    void write() const
    {
        auto out = disk->writeFile(file_path, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite);

        Poco::JSON::Object::Ptr root(new Poco::JSON::Object());
        root->set("transaction_id", transaction_id);
        root->set("partition_id", partition_id);
        root->set("destination", destination_storage_id.getQualifiedName().getFullName());
        root->set("completed", completed);

        Poco::JSON::Array::Ptr parts(new Poco::JSON::Array());
        for (const auto & i : items)
        {
            Poco::JSON::Object::Ptr obj(new Poco::JSON::Object());
            obj->set("part_name", i.part_name);
            obj->set("remote_path", i.remote_path);
            parts->add(obj);
        }
        root->set("parts", parts);

        std::ostringstream oss;
        Poco::JSON::Stringifier::stringify(root, oss, 2);
        const std::string s = oss.str();
        out->write(s.data(), s.size());
        out->finalize();
        out->sync();
    }

    void updateRemotePathAndWrite(const String & part_name, const String & remote_path)
    {
        for (auto & i : items)
        {
            if (i.part_name == part_name)
            {
                i.remote_path = remote_path;
                break;
            }
        }
        write();
    }

    std::vector<String> pendingParts() const
    {
        std::vector<String> res;
        for (const auto & i : items)
            if (i.remote_path.empty())
                res.push_back(i.part_name);
        return res;
    }

    std::vector<String> exportedPaths() const
    {
        std::vector<String> res;
        res.reserve(items.size());

        for (const auto & i : items)
        {
            if (!i.remote_path.empty())
            {
                res.push_back(i.remote_path);
            }
        }

        return res;
    }
};

}


