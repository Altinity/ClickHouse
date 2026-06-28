#pragma once
#include <Common/Exception.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// Validate a `server_root_id` — the explicit, configured identity of the content-addressed layout
/// subtree a server owns (spec §mount-safety). It is a clean relative path: it composes into the
/// object-key tree (`gc/server-roots/<srid>/...`, `roots/<srid>/...`), so the same hygiene the layout
/// applies to a namespace applies here (mirrors `CasLayout.h::checkNamespace`):
///   - non-empty;
///   - no leading/trailing '/', no empty segment ("//");
///   - no '.' or '..' segment;
///   - total length <= 255;
///   - no segment equal to the reserved "_files" / "_manifests".
/// Throws `ErrorCodes::BAD_ARGUMENTS` on any violation. Fail closed — there is no sanitizing fallback.
inline void validateServerRootId(const String & id)
{
    if (id.empty())
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "server_root_id must be non-empty");

    if (id.size() > 255)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "server_root_id '{}' is too long ({} > 255 bytes)", id, id.size());

    size_t start = 0;
    while (true)
    {
        size_t end = id.find('/', start);
        const String segment = id.substr(start, end == String::npos ? String::npos : end - start);
        if (segment.empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' has an empty segment (leading/trailing or doubled '/')", id);
        if (segment == "." || segment == "..")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses a relative segment ('.' or '..')", id);
        if (segment == "_files")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses the reserved segment '_files'", id);
        if (segment == "_manifests")
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "server_root_id '{}' uses the reserved segment '_manifests'", id);
        if (end == String::npos)
            break;
        start = end + 1;
    }
}

/// Phase 0 (mount safety) per-server-root control objects. Each is a pure-protobuf mutable object
/// carrying a CasHeader; the codecs mirror `CasWatermark.cpp` exactly (magic + checkCompatibility,
/// UInt128 in big-endian 16-byte form). Decode fails closed with CORRUPTED_DATA on bad bytes.

/// Owner anchor (`gc/server-roots/<srid>/owner`): binds the configured `server_root_id` to the
/// server UUID that owns the subtree.
struct OwnerObject
{
    UInt128 server_uuid{};
};

/// Writer-epoch fence (`gc/server-roots/<srid>/epoch`): the monotone next writer epoch to hand out.
struct ServerEpoch
{
    uint64_t next_writer_epoch = 0;
};

/// Mount lease (`gc/server-roots/<srid>/mount`): the current live mount holder of the subtree.
struct MountLease
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    String hostname;
    uint64_t pid = 0;
    uint64_t started_at_ms = 0;
    uint64_t seq = 0;
    uint64_t expires_at_ms = 0;
};

String encodeOwner(const OwnerObject & o);
OwnerObject decodeOwner(std::string_view data);

String encodeServerEpoch(const ServerEpoch & e);
ServerEpoch decodeServerEpoch(std::string_view data);

String encodeMountLease(const MountLease & m);
MountLease decodeMountLease(std::string_view data);

}
