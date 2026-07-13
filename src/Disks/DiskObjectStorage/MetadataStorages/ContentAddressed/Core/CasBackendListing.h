#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <functional>
#include <string_view>

namespace DB::Cas
{

/// Walk every key under `prefix` exactly once, resuming by the backend's explicit last-returned-key
/// cursor (ListPage::next_cursor, empty => done). The one paginated LIST/cursor loop that GC, fsck,
/// and the sweeps all re-implemented (~10 sites).
///
/// `on_page_fetched`, if set, fires exactly once per physical `backend.list` call (including an
/// empty/undersized final page) — a GC-owned caller's hook for a page-level ProfileEvents counter,
/// without misattributing a non-GC caller (e.g. fsck) that leaves it unset. Trails `page_limit`
/// (rather than sitting before it) so the two existing callers that override `page_limit`
/// (`Gc::fold`, `CasFsck.cpp`'s `listAll`) needed no change.
inline void forEachListedKey(Backend & backend, const String & prefix,
                             const std::function<void(const ListedKey &)> & cb,
                             size_t page_limit = 1000,
                             const std::function<void()> & on_page_fetched = {})
{
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(prefix, cursor, page_limit);
        if (on_page_fetched)
            on_page_fetched();
        for (const ListedKey & k : page.keys)
            cb(k);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

/// The normalized verdict of a token-exact delete, unifying the DeleteOutcome::Kind three-way that GC
/// (blob + manifest delete) and the orphan-manifest sweep each mapped by hand.
enum class DeleteClass : uint8_t { Deleted, Absent, Replaced };

inline DeleteClass classifyDeleteOutcome(const DeleteOutcome & d)
{
    switch (d.kind)
    {
        case DeleteOutcome::Kind::Deleted:       return DeleteClass::Deleted;
        case DeleteOutcome::Kind::NotFound:      return DeleteClass::Absent;
        case DeleteOutcome::Kind::TokenMismatch: return DeleteClass::Replaced;
    }
    return DeleteClass::Replaced;   /// unreachable; fail-safe toward "leave it" (never a false Deleted)
}

inline std::string_view deleteClassName(DeleteClass c)
{
    switch (c)
    {
        case DeleteClass::Deleted:  return "deleted";
        case DeleteClass::Absent:   return "absent";
        case DeleteClass::Replaced: return "replaced";
    }
    return "replaced";
}

}
