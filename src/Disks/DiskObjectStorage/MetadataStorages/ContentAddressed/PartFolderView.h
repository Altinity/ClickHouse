#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <map>
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
/// manifest cache holds). No I/O; never mutated after construction — a mutable-files refresh
/// builds a NEW view sharing the same manifest pointer. All answers are pure functions of the
/// members; wiring-reserved `.ca_*` mutable names are filtered here (folder-view semantics).
class PartFolderView
{
public:
    PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                   uint64_t published_at_ms_, std::map<String, String> mutable_files_,
                   std::shared_ptr<const Cas::PartManifest> manifest_);

    /// Convenience join of a fresh `Resolved` + its validated shared decode.
    static std::shared_ptr<const PartFolderView> make(
        PartRefKey key, const Cas::Resolved & resolved,
        std::shared_ptr<const Cas::PartManifest> manifest);

    /// Wiring-reserved RefPayload.mutable_files keys (dot-prefixed `.ca_*`) — never user-visible.
    static bool isReservedMutableName(std::string_view name) { return name.starts_with(".ca_"); }

    /// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the
    /// PoC recognizer (B64, also matches the nested detached-staging shape). `file` is the ROUTED
    /// in-tree file path. Returns the "<file>/" prefix, or nullopt when not a projection dir.
    static std::optional<std::string> projectionDirPrefix(const std::string & file);

    const PartRefKey & refKey() const { return key; }
    const Cas::ManifestId & manifestId() const { return manifest_id; }
    uint64_t manifestSize() const { return manifest_size; }
    uint64_t publishedAtMs() const { return published_at_ms; }
    const std::map<String, String> & mutableFiles() const { return mutable_files; }
    const std::shared_ptr<const Cas::PartManifest> & manifest() const { return manifest_body; }

    const Cas::ManifestEntry * findFile(const String & path) const;
    bool hasFile(const String & path) const;                      /// entry OR non-reserved mutable
    std::optional<uint64_t> fileSize(const String & path) const;  /// inline / blob
    std::optional<String> inlineBytes(const String & path) const; /// Inline entries only
    std::optional<String> mutableBytes(const String & path) const;
    std::vector<String> listChildren(const String & dir_prefix) const;
    bool hasDirectory(const String & dir_prefix) const;
    size_t estimatedBytes() const;

private:
    PartRefKey key;
    Cas::ManifestId manifest_id;
    uint64_t manifest_size = 0;
    uint64_t published_at_ms = 0;
    std::map<String, String> mutable_files;
    std::shared_ptr<const Cas::PartManifest> manifest_body;
};

}
