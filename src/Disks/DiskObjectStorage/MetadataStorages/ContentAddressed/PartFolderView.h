#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// Immutable snapshot of one resolved committed part/projection folder (spec
/// 2026-07-08-cas-part-folder-cache §PartFolderView). Index-free: the decoder guarantees strictly
/// ascending canonical path order, so file lookup is a binary search and directory listing is a
/// contiguous range scan over the SHARED decode (`manifest` is the same object the Store's
/// manifest cache holds). No I/O; never mutated after construction. All answers are pure functions
/// of the members. All-tree-part-files Task 9: the mutable-files serving this view used to provide
/// (a separate out-of-band payload, refreshed independently of the manifest) is gone — every
/// per-part file is an ordinary manifest tree entry now, served by `findFile` like anything else; a
/// content change is always a manifest change (`repointRef`), so the existing manifest-id staleness
/// check is the only freshness check this view needs.
class PartFolderView
{
public:
    PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                   uint64_t published_at_ms_,
                   std::shared_ptr<const Cas::PartManifest> manifest_, uint64_t validated_at_ms_);

    /// Convenience join of a fresh `Resolved` + its validated shared decode. `validated_at_ms` is the
    /// caller's "now" (spec §3 part_folder_validate): construction is only reached after
    /// `readManifestShared`'s mandatory HEAD, so it IS a body-proven moment. The caller supplies it
    /// (rather than this reading a clock itself) so `CachedPartFolderAccess`'s single injectable clock
    /// drives both this stamp and its own age-window comparison — the same seam tests use.
    static std::shared_ptr<const PartFolderView> make(
        PartRefKey key, const Cas::Resolved & resolved,
        std::shared_ptr<const Cas::PartManifest> manifest, uint64_t validated_at_ms);

    /// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the
    /// PoC recognizer (B64, also matches the nested detached-staging shape). `file` is the ROUTED
    /// in-tree file path. Returns the "<file>/" prefix, or nullopt when not a projection dir.
    static std::optional<std::string> projectionDirPrefix(const std::string & file);

    const PartRefKey & refKey() const { return key; }
    const Cas::ManifestId & manifestId() const { return manifest_id; }
    uint64_t manifestSize() const { return manifest_size; }
    uint64_t publishedAtMs() const { return published_at_ms; }
    const std::shared_ptr<const Cas::PartManifest> & manifest() const { return manifest_body; }
    /// The wall-clock ms at which this view's manifest body was last HEAD-proven live (spec §3
    /// part_folder_validate). A mutable-only refresh clone carries the ORIGINAL view's stamp forward
    /// (a mutable drift did not re-prove the body).
    uint64_t validatedAtMs() const { return validated_at_ms; }

    const Cas::ManifestEntry * findFile(const String & path) const;
    bool hasFile(const String & path) const;
    std::optional<uint64_t> fileSize(const String & path) const;  /// inline / blob
    std::optional<String> inlineBytes(const String & path) const; /// Inline entries only
    std::vector<String> listChildren(const String & dir_prefix) const;
    bool hasDirectory(const String & dir_prefix) const;
    size_t estimatedBytes() const;

private:
    PartRefKey key;
    Cas::ManifestId manifest_id;
    uint64_t manifest_size = 0;
    uint64_t published_at_ms = 0;
    std::shared_ptr<const Cas::PartManifest> manifest_body;
    uint64_t validated_at_ms = 0;
};

}
