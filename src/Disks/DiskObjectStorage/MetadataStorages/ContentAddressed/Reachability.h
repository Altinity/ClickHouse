#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace DB::ContentAddressed
{

using FooterResolver = std::function<Footer(const std::string & part_id)>;

/// Reachable blob-key set from the live roots (refs -> part footers -> blob keys).
std::set<std::string> markReachableBlobs(const std::set<std::string> & live_part_ids, const FooterResolver & resolve);

struct SweepResult
{
    std::vector<std::string> to_delete;
    std::unordered_map<std::string, int64_t> first_unreachable; /// updated timers (cleared for reachable-again)
};

/// `grace` is measured from first loss of reachability (NOT object age). Reachable-again clears the timer.
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace);

}
