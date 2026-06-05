#pragma once
#include <base/types.h>
#include <compare>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace DB::ContentAddressed
{

/// Strong typed identities for the content-addressed pool.
///
/// Each wraps a String but carries a distinct C++ type, so the compiler refuses to mix a bare
/// content hash with a full object key (the class of bug that caused live-blob data loss: the GC
/// reachable set held bare hashes while the sweep listed full object keys, and the std::string
/// types made the mismatched comparison silently compile). Construction from String is explicit,
/// there is NO implicit conversion to String, and the underlying string is reached only through
/// the explicit string accessor — at the object-storage boundary.
///
/// All five expose operator<=> and operator== (so they live in std::set / std::map) and a std::hash
/// specialization (so they live in std::unordered_* containers).

#define CONTENT_ADDRESSED_STRONG_STRING(NAME) \
    class NAME \
    { \
    public: \
        NAME() = default; \
        explicit NAME(String value_) : value(std::move(value_)) {} \
        const String & string() const { return value; } \
        auto operator<=>(const NAME &) const = default; \
        bool operator==(const NAME &) const = default; \
    private: \
        String value; \
    };

/// The bare content hash recorded in checksums / PartManifest::BlobEntry::key.
CONTENT_ADDRESSED_STRONG_STRING(BlobHash)
/// The full blob object key <prefix>/blobs/<h0>/<h1>/<hash>.
CONTENT_ADDRESSED_STRONG_STRING(BlobObjectKey)
/// The bare part id (the deterministic content-addressed identifier of a part).
CONTENT_ADDRESSED_STRONG_STRING(PartId)
/// The full part (manifest) object key <prefix>/parts/<p0>/<p1>/<part_id>.
CONTENT_ADDRESSED_STRONG_STRING(PartObjectKey)
/// The full ref object key <prefix>/store/<server>/<uuid>/refs/<part>.
CONTENT_ADDRESSED_STRONG_STRING(RefObjectKey)
/// The full per-ref sidecar object key <prefix>/store/<server>/<uuid>/refs/<part>.meta. It holds the
/// part's mutable per-part files (RefSidecar); ref-scoped, removed with the ref, never the manifest.
CONTENT_ADDRESSED_STRONG_STRING(RefMetaObjectKey)
/// The full GC delta-log object key <prefix>/gc/log/<epoch>.<shard>/<event_id> (CA GC S2). A single
/// coalesced log object holding `+`/`-` deltas; typed so it can never be confused with a blob/part/ref.
CONTENT_ADDRESSED_STRONG_STRING(GcLogObjectKey)
/// The full GC snapshot object key <prefix>/gc/snap/<padded-epoch>.<shard> (CA GC S2). The sorted
/// (H)->count run; typed so it can never be confused with a blob/part/ref/log key.
CONTENT_ADDRESSED_STRONG_STRING(GcSnapObjectKey)

#undef CONTENT_ADDRESSED_STRONG_STRING

}

#define CONTENT_ADDRESSED_STRONG_STRING_HASH(NAME) \
    template <> \
    struct std::hash<DB::ContentAddressed::NAME> \
    { \
        size_t operator()(const DB::ContentAddressed::NAME & v) const noexcept \
        { \
            return std::hash<std::string>{}(v.string()); \
        } \
    };

CONTENT_ADDRESSED_STRONG_STRING_HASH(BlobHash)
CONTENT_ADDRESSED_STRONG_STRING_HASH(BlobObjectKey)
CONTENT_ADDRESSED_STRONG_STRING_HASH(PartId)
CONTENT_ADDRESSED_STRONG_STRING_HASH(PartObjectKey)
CONTENT_ADDRESSED_STRONG_STRING_HASH(RefObjectKey)
CONTENT_ADDRESSED_STRONG_STRING_HASH(RefMetaObjectKey)
CONTENT_ADDRESSED_STRONG_STRING_HASH(GcLogObjectKey)
CONTENT_ADDRESSED_STRONG_STRING_HASH(GcSnapObjectKey)

#undef CONTENT_ADDRESSED_STRONG_STRING_HASH
