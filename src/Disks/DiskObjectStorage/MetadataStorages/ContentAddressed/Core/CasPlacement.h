#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <base/defines.h>

namespace DB::Cas
{

/// Exhaustive dispatch over `Placement`: invokes exactly the handler for the matching arm and returns
/// its result. No `default` — a new `Placement` enumerator forces every caller to handle it
/// (compile error otherwise). Mirrors the hand-written `switch (placement)` used across the
/// codecs, walk, build, and fsck — but centralises exhaustiveness enforcement in one place.
template <typename OnInline, typename OnBlob, typename OnSubtree>
decltype(auto) visitPlacement(Placement p, OnInline && on_inline, OnBlob && on_blob,
                              OnSubtree && on_sub)
{
    switch (p)
    {
        case Placement::Inline:    return on_inline();
        case Placement::Blob:      return on_blob();
        case Placement::Subtree:   return on_sub();
    }
    UNREACHABLE();
}

}
