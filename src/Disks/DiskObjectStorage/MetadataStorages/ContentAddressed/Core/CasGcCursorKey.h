#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>
#include <charconv>
#include <string_view>
#include <utility>

namespace DB::Cas
{

/// Format a GC cursor key from a root namespace and shard index.
///
/// The canonical format is "<ns>/<shard>" where <ns> is the namespace string
/// (which may itself contain '/', e.g. "srv1/tbl") and <shard> is the decimal
/// shard index.  The namespace and shard are separated by the LAST '/' — callers
/// that parse the key back MUST use rfind('/') to locate the boundary.
///
/// This function MUST produce the IDENTICAL string as the legacy inline expression
///   ns.string() + "/" + std::to_string(shard)
/// so that all cursor keys written by the new code match what was written before and
/// what is already stored in durable GC state.
inline String cursorKey(const RootNamespace & ns, uint64_t shard)
{
    return ns.string() + "/" + std::to_string(shard);
}

/// Parse a GC cursor key back into (RootNamespace, shard).
///
/// Splitting rule: the boundary is the LAST '/' in the key — i.e. rfind('/') — so
/// that namespaces that contain '/' (e.g. "srv1/tbl") are handled correctly.
/// The behaviour MUST be identical to the legacy inline parse at CasGc.cpp:310-316:
///   size_t slash = cursor_key.rfind('/');
///   RootNamespace ns{cursor_key.substr(0, slash)};
///   uint64_t shard; std::from_chars(cursor_key.data() + slash + 1, ..., shard);
///
/// The caller is responsible for ensuring `key` is a valid cursor key produced by
/// `cursorKey` — passing an invalid string results in undefined behaviour (rfind
/// returns npos, from_chars fails).
inline std::pair<RootNamespace, uint64_t> parseCursorKey(std::string_view key)
{
    const size_t slash = key.rfind('/');
    const RootNamespace ns{String(key.substr(0, slash))};
    uint64_t shard = 0;
    std::from_chars(key.data() + slash + 1, key.data() + key.size(), shard);
    return {ns, shard};
}

}
