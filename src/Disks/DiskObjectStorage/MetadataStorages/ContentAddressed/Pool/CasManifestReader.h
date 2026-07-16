#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Common/CacheBase.h>
#include <memory>
#include <optional>

namespace DB::Cas
{

/// Blob placement inside a content object: the object key plus the ranged window (payload offset +
/// length) that `locate` derives from a manifest entry.
struct BlobLocation
{
    String key;
    uint64_t offset = 0;                      /// payload start within the object
    uint64_t length = 0;
};

/// The manifest read path, extracted from `Pool` (spec §Decomposition): the mandatory-HEAD +
/// fail-closed-validation + decode of a part manifest, plus the token-gated byte-weighted decode
/// cache and blob `locate`. Environment is injected by reference (no `Pool` back-reference): the
/// backend, the layout, the pool meta (for the fixed `blob_header_len`), and the event sink.
///
/// The decode cache is a `CacheBase` LRU — its synchronization is INTERNAL to `CacheBase`; this
/// component owns no `Pool`-level mutex. `nullptr` <=> caching disabled
/// (`manifest_decode_cache_bytes == 0`).
class CasManifestReader
{
public:
    CasManifestReader(
        Backend & backend_, const Layout & layout_, const PoolMeta & meta_,
        const CasEventSink & event_sink_, size_t manifest_decode_cache_bytes);

    /// Fail-closed manifest read: mandatory HEAD (a missing body is INV-NO-DANGLE), (id, token) cache
    /// hit reuses the immutable decode, else GET + decode + body/namespace validation, then cache set.
    PartManifest readManifest(const ManifestId & id);
    /// Identical to `readManifest` but returns the shared decoded manifest (the cache's own value),
    /// avoiding a copy on the part-folder read path.
    std::shared_ptr<const PartManifest> readManifestShared(const ManifestId & id);

    /// Blob placement only (no read): the object key + payload window for a Blob-placed entry.
    BlobLocation locate(const ManifestEntry & entry) const;

    /// Test seam: retained bytes of the manifest decode cache (0 when disabled).
    size_t manifestDecodeCacheBytes() const { return manifest_cache ? manifest_cache->sizeInBytes() : 0; }

private:
    struct ManifestCacheKey
    {
        ManifestId manifest_id;
        Token token;
        bool operator==(const ManifestCacheKey &) const = default;
    };
    struct ManifestCacheKeyHash
    {
        size_t operator()(const ManifestCacheKey & k) const;
    };
    /// Byte-weighted so a server that reads very many parts (each decode carrying megabytes of inline
    /// bytes) has an honest memory ceiling instead of a count-only bound. Same key, same fail-closed
    /// token semantics.
    struct PartManifestWeight
    {
        size_t operator()(const PartManifest & m) const
        {
            size_t bytes = 256;
            for (const auto & e : m.entries)
                bytes += e.path.size() + e.inline_bytes.size() + 96;
            return bytes;
        }
    };
    using ManifestDecodeCache = CacheBase<ManifestCacheKey, PartManifest, ManifestCacheKeyHash, PartManifestWeight>;

    Backend & backend;
    const Layout & layout;
    const PoolMeta & meta;
    const CasEventSink & event_sink;
    std::unique_ptr<ManifestDecodeCache> manifest_cache;
};

}
