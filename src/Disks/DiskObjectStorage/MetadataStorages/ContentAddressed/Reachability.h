#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <functional>
#include <set>
#include <string>

namespace DB::ContentAddressed
{

using FooterResolver = std::function<Footer(const std::string & part_id)>;

/// Reachable blob-key set from the live roots (refs -> part footers -> blob keys).
std::set<std::string> markReachableBlobs(const std::set<std::string> & live_part_ids, const FooterResolver & resolve);

}
