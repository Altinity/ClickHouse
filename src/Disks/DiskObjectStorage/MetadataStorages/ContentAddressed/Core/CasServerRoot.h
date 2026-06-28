#pragma once
#include <Common/Exception.h>
#include <base/types.h>

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

}
