#include <Storages/MergeTree/ExportTTLIndex.h>

#include <Interpreters/Context.h>
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
    extern const int LOGICAL_ERROR;
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
    return ExportBlockRanges::compact(std::move(ranges));
}

String stringify(const Poco::Dynamic::Var & json)
{
    std::ostringstream oss;     // STYLE_CHECK_ALLOW_STD_STRING_STREAM
    oss.exceptions(std::ios::failbit);
    Poco::JSON::Stringifier::stringify(json, oss);
    return oss.str();
}

}

Int64 ExportTTLIndexEntry::maxBlock() const
{
    Int64 result = 0;
    for (const auto & range : exported)
        result = std::max(result, range.max_block);
    return result;
}

PartExportState ExportTTLIndexEntry::classify(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & current_task_ranges) const
{
    if (ExportBlockRanges::intersectsAny(part, current_task_ranges))
        return PartExportState::CLAIMED;
    if (ExportBlockRanges::isCoveredByUnion(part, exported))
        return PartExportState::EXPORTED;
    return PartExportState::NONE;
}

std::vector<MergeTreePartInfo> ExportTTLIndexEntry::notExported(const MergeTreePartInfo & part) const
{
    return ExportBlockRanges::subtract(part, exported);
}

void ExportTTLIndexEntry::startClaim(const String & transaction_id)
{
    if (!current_task.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Export task {} cannot start for partition {}, which export task {} exports",
            transaction_id, partition_id, current_task);
    current_task = transaction_id;
}

void ExportTTLIndexEntry::retryClaim(const String & transaction_id)
{
    if (current_task.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Export task {} cannot retry the task of partition {}, which has none",
            transaction_id, partition_id);
    current_task = transaction_id;
}

void ExportTTLIndexEntry::commitClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & ranges)
{
    if (current_task == transaction_id)
        current_task.clear();

    for (const auto & range : ranges)
        exported.emplace_back(partition_id, range.min_block, range.max_block, /* level */ 0, /* mutation */ 0);
    exported = ExportBlockRanges::compact(std::move(exported));
}

String ExportTTLIndexEntry::toJSONString() const
{
    Poco::JSON::Object json;
    json.set("exported", rangesToJSON(exported));
    if (!current_task.empty())
        json.set("current_task", current_task);
    return stringify(json);
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

    entry.exported = rangesFromJSON(partition_id, json->getArray("exported"));
    entry.current_task = json->optValue<String>("current_task", "");
    return entry;
}

std::unique_ptr<const ExportTTLIndexSnapshot> ExportTTLIndexSnapshot::build(
    int32_t version, std::map<String, std::map<String, ExportTTLVersionedEntry>> entries)
{
    auto exported_blocks = std::make_shared<ExportedBlocks>();
    for (const auto & [destination_key, by_partition] : entries)
        for (const auto & [partition_id, versioned] : by_partition)
            if (!versioned.entry.exported.empty())
                exported_blocks->ranges_by_partition[partition_id][destination_key] = versioned.entry.exported;

    auto snapshot = std::make_unique<ExportTTLIndexSnapshot>();
    snapshot->version = version;
    snapshot->entries = std::move(entries);
    snapshot->exported_blocks = std::move(exported_blocks);
    return snapshot;
}

namespace ExportTTLUtils
{

void allowLossyCasts(Context & context)
{
    context.setSetting("export_merge_tree_part_allow_lossy_cast", true);
}

String destinationKey(const String & database, const String & table)
{
    return escapeForFileName(database) + "." + escapeForFileName(table);
}

std::vector<MergeTreePartInfo> toPartInfos(const String & partition_id, const ExportBlockNumberRanges & ranges)
{
    std::vector<MergeTreePartInfo> result;
    result.reserve(ranges.size());
    for (const auto & [min_block, max_block] : ranges)
        result.emplace_back(partition_id, min_block, max_block, /* level */ 0, /* mutation */ 0);
    return ExportBlockRanges::compact(std::move(result));
}

ExportBlockNumberRanges toBlockNumberRanges(const std::vector<MergeTreePartInfo> & ranges)
{
    ExportBlockNumberRanges result;
    result.reserve(ranges.size());
    for (const auto & range : ranges)
        result.emplace_back(range.min_block, range.max_block);
    return result;
}

}

}
