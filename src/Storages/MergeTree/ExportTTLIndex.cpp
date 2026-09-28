#include <Storages/MergeTree/ExportTTLIndex.h>

#include <Common/Exception.h>
#include <Common/escapeForFileName.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <algorithm>
#include <sstream>

namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
}

namespace
{

Poco::JSON::Array::Ptr rangesToJSON(const std::vector<MergeTreePartInfo> & ranges)
{
    Poco::JSON::Array::Ptr array = new Poco::JSON::Array();
    for (const auto & range : ranges)
    {
        Poco::JSON::Array::Ptr pair = new Poco::JSON::Array();
        pair->add(range.min_block);
        pair->add(range.max_block);
        array->add(pair);
    }
    return array;
}

std::vector<MergeTreePartInfo> rangesFromJSON(const String & partition_id, const Poco::JSON::Array::Ptr & array)
{
    std::vector<MergeTreePartInfo> ranges;
    if (!array)
        return ranges;

    ranges.reserve(array->size());
    for (size_t i = 0; i < array->size(); ++i)
    {
        const auto pair = array->getArray(static_cast<unsigned int>(i));
        if (!pair || pair->size() != 2)
            throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid block range in the export index of partition {}", partition_id);
        ranges.emplace_back(partition_id, pair->getElement<Int64>(0), pair->getElement<Int64>(1), /* level */ 0, /* mutation */ 0);
    }
    return ranges;
}

}

Int64 ExportTTLIndexEntry::maxBlock() const
{
    Int64 result = 0;
    for (const auto & range : exported)
        result = std::max(result, range.max_block);
    for (const auto & [_, ranges] : claimed)
        for (const auto & range : ranges)
            result = std::max(result, range.max_block);
    return result;
}

std::vector<MergeTreePartInfo> ExportTTLIndexEntry::allClaimed() const
{
    std::vector<MergeTreePartInfo> result;
    for (const auto & [_, ranges] : claimed)
        result.insert(result.end(), ranges.begin(), ranges.end());
    return ExportFenceUtils::compactRanges(std::move(result));
}

PartExportState ExportTTLIndexEntry::classify(const MergeTreePartInfo & part) const
{
    for (const auto & [_, ranges] : claimed)
        if (ExportFenceUtils::intersectsAny(part, ranges))
            return PartExportState::CLAIMED;
    if (ExportFenceUtils::intersectsAny(part, exported))
        return PartExportState::EXPORTED;
    return PartExportState::NONE;
}

ExportFenceEntry ExportTTLIndexEntry::toFenceEntry(const String & destination) const
{
    ExportFenceEntry entry;
    entry.destination = destination;
    entry.exported = exported;
    entry.claimed = allClaimed();
    return entry;
}

void ExportTTLIndexEntry::claim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts)
{
    auto & ranges = claimed[transaction_id];
    for (const auto & part : parts)
        ranges.emplace_back(partition_id, part.min_block, part.max_block, 0, 0);
    ranges = ExportFenceUtils::compactRanges(std::move(ranges));
}

void ExportTTLIndexEntry::moveClaims(const std::vector<String> & from, const String & to)
{
    auto & target = claimed[to];
    for (const auto & transaction_id : from)
    {
        const auto it = claimed.find(transaction_id);
        if (it == claimed.end() || transaction_id == to)
            continue;
        target.insert(target.end(), it->second.begin(), it->second.end());
        claimed.erase(it);
    }
    target = ExportFenceUtils::compactRanges(std::move(target));
}

void ExportTTLIndexEntry::commitClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts)
{
    if (const auto it = claimed.find(transaction_id); it != claimed.end())
    {
        exported.insert(exported.end(), it->second.begin(), it->second.end());
        claimed.erase(it);
    }
    for (const auto & part : parts)
        exported.emplace_back(partition_id, part.min_block, part.max_block, 0, 0);
    exported = ExportFenceUtils::compactRanges(std::move(exported));
}

void ExportTTLIndexEntry::releaseClaim(const String & transaction_id)
{
    claimed.erase(transaction_id);
}

String ExportTTLIndexEntry::toJSONString() const
{
    Poco::JSON::Object json;
    json.set("exported", rangesToJSON(exported));

    Poco::JSON::Object::Ptr claimed_object = new Poco::JSON::Object();
    for (const auto & [transaction_id, ranges] : claimed)
        claimed_object->set(transaction_id, rangesToJSON(ranges));
    json.set("claimed", claimed_object);

    std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);
    return oss.str();
}

ExportTTLIndexEntry ExportTTLIndexEntry::fromJSONString(const String & partition_id, const String & json_string)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = partition_id;
    if (json_string.empty())
        return entry;

    Poco::JSON::Parser parser;
    const auto json = parser.parse(json_string).extract<Poco::JSON::Object::Ptr>();
    if (!json)
        throw Exception(ErrorCodes::INCORRECT_DATA, "The export index of partition {} is not a JSON object", partition_id);

    entry.exported = ExportFenceUtils::compactRanges(rangesFromJSON(partition_id, json->getArray("exported")));

    if (const auto claimed_object = json->getObject("claimed"))
    {
        for (const auto & transaction_id : claimed_object->getNames())
            entry.claimed[transaction_id] = ExportFenceUtils::compactRanges(
                rangesFromJSON(partition_id, claimed_object->getArray(transaction_id)));
    }

    return entry;
}

std::unique_ptr<const ExportTTLIndexSnapshot> ExportTTLIndexSnapshot::build(
    int32_t version, std::map<String, std::map<String, ExportTTLVersionedEntry>> entries)
{
    auto fence = std::make_shared<ExportFence>();
    for (const auto & [destination_key, by_partition] : entries)
        for (const auto & [partition_id, versioned] : by_partition)
            if (!versioned.entry.empty())
                fence->entries_by_partition[partition_id].push_back(versioned.entry.toFenceEntry(destination_key));

    auto snapshot = std::make_unique<ExportTTLIndexSnapshot>();
    snapshot->version = version;
    snapshot->entries = std::move(entries);
    snapshot->fence = std::move(fence);
    return snapshot;
}

String ExportTTLSchedulerState::toJSONString() const
{
    Poco::JSON::Object json;
    json.set("scheduler_replica", scheduler_replica);

    Poco::JSON::Object::Ptr partitions_object = new Poco::JSON::Object();
    for (const auto & [partition_id, partition] : partitions)
    {
        Poco::JSON::Object::Ptr partition_object = new Poco::JSON::Object();
        partition_object->set("last_error", partition.last_error);
        partition_object->set("first_eligible_time", static_cast<Int64>(partition.first_eligible_time));
        partition_object->set("last_new_part_time", static_cast<Int64>(partition.last_new_part_time));
        partitions_object->set(partition_id, partition_object);
    }
    json.set("partitions", partitions_object);

    std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);
    return oss.str();
}

ExportTTLSchedulerState ExportTTLSchedulerState::fromJSONString(const String & json_string)
{
    ExportTTLSchedulerState state;
    if (json_string.empty())
        return state;

    Poco::JSON::Parser parser;
    const auto json = parser.parse(json_string).extract<Poco::JSON::Object::Ptr>();
    if (!json)
        throw Exception(ErrorCodes::INCORRECT_DATA, "The state of the TTL export scheduler is not a JSON object");

    if (json->has("scheduler_replica"))
        state.scheduler_replica = json->getValue<String>("scheduler_replica");

    if (const auto partitions_object = json->getObject("partitions"))
    {
        for (const auto & partition_id : partitions_object->getNames())
        {
            const auto partition_object = partitions_object->getObject(partition_id);
            if (!partition_object)
                continue;
            auto & partition = state.partitions[partition_id];
            partition.last_error = partition_object->optValue<String>("last_error", "");
            partition.first_eligible_time = static_cast<time_t>(partition_object->optValue<Int64>("first_eligible_time", 0));
            partition.last_new_part_time = static_cast<time_t>(partition_object->optValue<Int64>("last_new_part_time", 0));
        }
    }

    return state;
}

namespace ExportTTLUtils
{

String destinationKey(const String & database, const String & table, const String & uuid)
{
    return escapeForFileName(database) + "." + escapeForFileName(table) + "." + (uuid.empty() ? String("none") : escapeForFileName(uuid));
}

std::vector<MergeTreePartInfo> rangesOfParts(const std::vector<String> & part_names, MergeTreeDataFormatVersion format_version)
{
    std::vector<MergeTreePartInfo> ranges;
    ranges.reserve(part_names.size());
    for (const auto & name : part_names)
    {
        const auto info = MergeTreePartInfo::fromPartName(name, format_version);
        ranges.emplace_back(info.getPartitionId(), info.min_block, info.max_block, 0, 0);
    }
    return ExportFenceUtils::compactRanges(std::move(ranges));
}

}

}
