#pragma once
#include <base/types.h>
#include <cstdint>
#include <set>
#include <string_view>

namespace DB::Cas
{

/// The namespace registry (`gc/registry`, design §5.3; formerly `roots/_registry`) — the
/// authoritative namespace universe, a mutable CAS object like a root-shard manifest. A writer's
/// FIRST publish into a namespace CAS-appends it here BEFORE creating any shard manifest
/// (`W-REGISTER`), and GC discovers namespaces FROM the registry (never LIST) and fences it like
/// a shard. This is what orders namespace CREATION against the GC fence: without it, a first
/// publish into a brand-new namespace is invisible to both horns of the no-return argument (the
/// fresh manifest carries `fence_round` 0, so the writer gate never refreshes and the recheck
/// never folds its journal) — a stale-view writer could republish a condemned hash past the
/// recheck, and the exact-token delete would dangle a live ref.
///
/// Non-hashed metadata => strict JSON:
///   {"format":"cas_roots_registry","version":1,"registry_version":3,"fence_round":2,
///    "namespaces":["srv1/tbl","srv2/tbl2"]}
struct RootsRegistry
{
    uint64_t registry_version = 0;   /// monotone, CAS-carried (like shard_version)
    uint64_t fence_round = 0;        /// written only by GC; monotone (like a manifest's)
    std::set<String> namespaces;     /// sorted => deterministic encode
};

String encodeRootsRegistry(const RootsRegistry & registry);
RootsRegistry decodeRootsRegistry(std::string_view data);

}
