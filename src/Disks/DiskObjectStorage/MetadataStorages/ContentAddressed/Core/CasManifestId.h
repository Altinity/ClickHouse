#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <fmt/format.h>
#include <cstdint>
#include <tuple>

namespace DB::Cas
{

/// The compact reference a root journal stores for a part manifest (spec §Part Manifest Reference
/// And Identity). It is NOT a string key: `CasLayout::manifestKey` derives the object key from this
/// ref plus the owning root namespace. `root_namespace_id` is deliberately NOT a field here - it
/// comes from the owning root context and must never be serialized into the journal ref.
///
///   writer_epoch         - durable monotone writer epoch allocated under the mounted `server_root_id`;
///                          never reused for that server root.
///   build_sequence       - monotone build sequence inside one writer incarnation; part of identity
///                          and of the build-scoped debris prefix.
///   manifest_ordinal     - monotone ordinal inside one build, rendered as `000001.zst` in the key.
struct ManifestRef
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    uint32_t manifest_ordinal = 0;

    bool operator==(const ManifestRef & o) const = default;

    /// Total order for std::map / std::set keys. Field order is arbitrary but stable.
    bool operator<(const ManifestRef & o) const
    {
        return std::tie(writer_epoch, build_sequence, manifest_ordinal)
             < std::tie(o.writer_epoch, o.build_sequence, o.manifest_ordinal);
    }
};

/// The protocol identity GC uses (spec §Object Identity And Ownership): namespace-qualified
/// `ManifestId = (root_namespace_id, ManifestRef)`. It keys source edges / blob deltas / cleanup work
/// and addressing. `ManifestId` is the protocol identity; the TLA+ model abstracts it to
/// `ManifestSafetyId = (root_namespace, manifest ref)`, a Phase-0 model-only term that never
/// appears in this code.
/// Two namespaces may legally carry the same `ManifestRef` tuple without addressing the same object;
/// keying source edges / blob deltas / cleanup work by `ManifestRef` alone is the modeled
/// `SabotageKeyByRefNotId` hazard. Always key by `ManifestId`.
struct ManifestId
{
    RootNamespace root_namespace;   /// owning namespace; NOT part of the journal ref
    ManifestRef ref;

    bool operator==(const ManifestId & o) const = default;

    bool operator<(const ManifestId & o) const
    {
        if (root_namespace.string() != o.root_namespace.string())
            return root_namespace.string() < o.root_namespace.string();
        return ref < o.ref;
    }
};

/// A `{writer_epoch, build_sequence}` pair identifying the incarnation of a ref-shard (legacy
/// mutable-shard model). `{0, 0}` is the unstamped sentinel -- a valid value, not an error. Relocated
/// here from the removed legacy ref-shard codec (dropped with the snapshot+log ref model): it survives as
/// the type of `ShardCoverage::incarnation` in the fold seal, which the snapshot+log model leaves unstamped.
struct ShardIncarnation
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    bool operator==(const ShardIncarnation &) const = default;
    bool operator<(const ShardIncarnation & o) const
    {
        return std::tie(writer_epoch, build_sequence) < std::tie(o.writer_epoch, o.build_sequence);
    }
};

inline constexpr uint32_t kMaxManifestOrdinal = 999999;

/// Six-digit filename for a per-build part-manifest ordinal: `000001.zst` through `999999.zst`
/// (the registered v3 stored suffix for `FormatId::PartManifest`). `0` is reserved as an invalid
/// sentinel and is never emitted.
inline String manifestOrdinalFileName(uint32_t manifest_ordinal)
{
    if (manifest_ordinal == 0 || manifest_ordinal > kMaxManifestOrdinal)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "Manifest ordinal must be in [1, {}], got {}", kMaxManifestOrdinal, manifest_ordinal);
    return fmt::format("{:06}{}", manifest_ordinal, storedSuffix(FormatId::PartManifest));
}

inline String manifestRefDebugString(const ManifestRef & ref)
{
    return fmt::format("{}:{}:{}", ref.writer_epoch, ref.build_sequence, ref.manifest_ordinal);
}

}

/// std::hash specializations so ManifestRef / ManifestId can key unordered containers (the read-path
/// `(ManifestId, Token)` cache in Phase 1c, plus any GC-side unordered map). Equality is the
/// `operator==` above; these hashes must agree with it (equal values => equal hash).
namespace std
{

template <>
struct hash<DB::Cas::ManifestRef>
{
    size_t operator()(const DB::Cas::ManifestRef & r) const
    {
        const size_t h1 = std::hash<uint64_t>{}(r.writer_epoch);
        const size_t h2 = std::hash<uint64_t>{}(r.build_sequence);
        const size_t h3 = std::hash<uint32_t>{}(r.manifest_ordinal);
        size_t h = h1;
        h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct hash<DB::Cas::ManifestId>
{
    size_t operator()(const DB::Cas::ManifestId & id) const
    {
        const size_t h1 = std::hash<::String>{}(id.root_namespace.string());
        const size_t h2 = std::hash<DB::Cas::ManifestRef>{}(id.ref);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

}
