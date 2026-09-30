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
    return ranges;
}

}

Int64 ExportTTLIndexEntry::maxBlock() const
{
    Int64 result = 0;
    for (const auto & range : exported)
        result = std::max(result, range.max_block);
    if (claim)
        for (const auto & range : claim->ranges)
            result = std::max(result, range.max_block);
    return result;
}

PartExportState ExportTTLIndexEntry::classify(const MergeTreePartInfo & part) const
{
    if (claim && ExportFenceUtils::intersectsAny(part, claim->ranges))
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
    if (claim)
        entry.claimed = claim->ranges;
    return entry;
}

void ExportTTLIndexEntry::startClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts)
{
    if (claim)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Export task {} cannot claim parts of partition {}, which export task {} claimed",
            transaction_id, partition_id, claim->transaction_id);

    std::vector<MergeTreePartInfo> ranges;
    ranges.reserve(parts.size());
    for (const auto & part : parts)
        ranges.emplace_back(partition_id, part.min_block, part.max_block, 0, 0);
    claim = Claim{.transaction_id = transaction_id, .ranges = ExportFenceUtils::compactRanges(std::move(ranges))};
}

void ExportTTLIndexEntry::commitClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts)
{
    if (claim && claim->transaction_id == transaction_id)
    {
        exported.insert(exported.end(), claim->ranges.begin(), claim->ranges.end());
        claim.reset();
    }
    for (const auto & part : parts)
        exported.emplace_back(partition_id, part.min_block, part.max_block, 0, 0);
    exported = ExportFenceUtils::compactRanges(std::move(exported));
}

String ExportTTLIndexEntry::toJSONString() const
{
    Poco::JSON::Object json;
    json.set("exported", rangesToJSON(exported));

    if (claim)
    {
        Poco::JSON::Object::Ptr claim_object = new Poco::JSON::Object();
        claim_object->set("transaction_id", claim->transaction_id);
        claim_object->set("ranges", rangesToJSON(claim->ranges));
        json.set("claim", claim_object);
    }

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

    if (const auto claim_object = json->getObject("claim"))
    {
        entry.claim = Claim{
            .transaction_id = claim_object->getValue<String>("transaction_id"),
            .ranges = ExportFenceUtils::compactRanges(rangesFromJSON(partition_id, claim_object->getArray("ranges"))),
        };
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

namespace ExportTTLUtils
{

void allowLossyCasts(Context & context)
{
    context.setSetting("export_merge_tree_part_allow_lossy_cast", true);
}

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

std::vector<std::pair<Int64, Int64>> toBlockRanges(const std::vector<MergeTreePartInfo> & ranges)
{
    std::vector<std::pair<Int64, Int64>> result;
    result.reserve(ranges.size());
    for (const auto & range : ranges)
        result.emplace_back(range.min_block, range.max_block);
    return result;
}

std::vector<MergeTreePartInfo> fromBlockRanges(const String & partition_id, const std::vector<std::pair<Int64, Int64>> & block_ranges)
{
    std::vector<MergeTreePartInfo> ranges;
    ranges.reserve(block_ranges.size());
    for (const auto & [min_block, max_block] : block_ranges)
        ranges.emplace_back(partition_id, min_block, max_block, /* level */ 0, /* mutation */ 0);
    return ExportFenceUtils::compactRanges(std::move(ranges));
}

}

}
