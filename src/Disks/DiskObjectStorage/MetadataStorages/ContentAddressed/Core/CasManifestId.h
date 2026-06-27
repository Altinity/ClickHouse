#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <tuple>

namespace DB::Cas
{

/// The compact reference a root journal stores for a part manifest (spec §Part Manifest Reference
/// And Identity). It is NOT a string key: `CasLayout::manifestKey` derives the object key from this
/// ref plus the owning root namespace. `root_namespace_id` is deliberately NOT a field here - it
/// comes from the owning root context and must never be serialized into the journal ref.
///
///   writer_instance_id   - writer-incarnation token, formatted "<server_id_hex>:<process_epoch>"; a
///                          new process epoch must not reuse the same build prefix. It is a String,
///                          used verbatim as the `<writer_instance_id>` path segment of `manifestKey`.
///   build_sequence       - monotone build sequence inside one writer incarnation; part of identity
///                          and of the build-scoped debris prefix.
///   manifest_instance_id - random 128-bit; gives collision-safety and the never-reused guarantee.
struct ManifestRef
{
    String writer_instance_id;
    uint64_t build_sequence = 0;
    UInt128 manifest_instance_id{};

    bool operator==(const ManifestRef & o) const = default;

    /// Total order for std::map / std::set keys. Field order is arbitrary but stable.
    bool operator<(const ManifestRef & o) const
    {
        return std::tie(writer_instance_id, build_sequence, manifest_instance_id)
             < std::tie(o.writer_instance_id, o.build_sequence, o.manifest_instance_id);
    }
};

/// The protocol identity GC uses (spec §Object Identity And Ownership): namespace-qualified
/// `ManifestId = (root_namespace_id, ManifestRef)`. It keys source edges / blob deltas / cleanup work
/// and addressing. `ManifestId` is the protocol identity; the TLA+ model abstracts it to
/// `ManifestSafetyId = (root_namespace, manifest_instance_id)`, a Phase-0 model-only term that never
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

/// The 2-char fanout segment of a manifest key: the first 2 lowercase-hex chars of
/// `manifest_instance_id` (spec §S3 Layout: the `<aa>` fanout is derived from `manifest_instance_id`,
/// NOT from the payload digest). 32-char hex always has >= 2 chars, so this never underflows.
inline String manifestAa(const ManifestRef & ref)
{
    return u128ToHex(ref.manifest_instance_id).substr(0, 2);
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
        const size_t h1 = std::hash<::String>{}(r.writer_instance_id);
        const size_t h2 = std::hash<uint64_t>{}(r.build_sequence);
        const size_t h3 = std::hash<::UInt128>{}(r.manifest_instance_id);
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
