#pragma once

#include <Storages/MergeTree/MergeTreeDataPartTTLInfo.h>
#include <Storages/MergeTree/ExportedBlocks.h>
#include <base/types.h>

#include <ctime>
#include <optional>

#include <fmt/format.h>

namespace DB
{

/// Keeps TTL that deletes or rewrites rows from applying to parts that the `EXPORT` TTL of the
/// table has not exported entirely yet, so no row is lost before it reaches the destination. A view
/// of the exported blocks older than the index only holds more parts, but for blocks that
/// `FORGET PARTITION` forgot meanwhile.
struct ExportTTLDeleteGate
{
    bool enabled = false;
    /// Key of the destination in the export index. While it is unknown, every part with due TTL is held.
    String destination_key;
    time_t now = 0;

    /// Returns the reason why the part must not be merged, or nothing if it may be.
    std::optional<String> check(
        const String & part_name, const MergeTreePartInfo & info, const MergeTreeDataPartTTLInfos & ttl_infos,
        const ExportedBlocks * exported_blocks) const
    {
        if (!enabled || !ttl_infos.part_min_ttl || ttl_infos.part_min_ttl > now)
            return std::nullopt;

        if (exported_blocks && !destination_key.empty() && exported_blocks->isExported(info, destination_key))
            return std::nullopt;

        return fmt::format("Part {} has TTL that is due, but the EXPORT TTL has not exported it yet", part_name);
    }
};

}
