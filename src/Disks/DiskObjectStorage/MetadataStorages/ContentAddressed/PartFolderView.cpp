#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <base/defines.h>
#include <algorithm>
#include <unordered_set>

namespace DB::ContentAddressed
{

PartFolderView::PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                               uint64_t published_at_ms_,
                               std::shared_ptr<const Cas::PartManifest> manifest_, uint64_t validated_at_ms_)
    : key(std::move(key_))
    , manifest_id(std::move(manifest_id_))
    , manifest_size(manifest_size_)
    , published_at_ms(published_at_ms_)
    , manifest_body(std::move(manifest_))
    , validated_at_ms(validated_at_ms_)
{
    chassert(manifest_body);
    /// The binary-search contract: entries must be STRICTLY ascending by `path` (sorted AND unique) —
    /// `decodePartManifest` enforces exactly this for every decoded body, and `findEntry`'s binary
    /// search assumes uniqueness. `adjacent_find` with `!(a.path < b.path)` flags any adjacent pair
    /// that is out-of-order OR duplicate (stronger than `is_sorted`, which permits duplicates); a
    /// hand-constructed manifest (tests) must honor it too.
    chassert(std::adjacent_find(manifest_body->entries.begin(), manifest_body->entries.end(),
        [](const Cas::ManifestEntry & a, const Cas::ManifestEntry & b) { return !(a.path < b.path); })
        == manifest_body->entries.end());
}

std::shared_ptr<const PartFolderView> PartFolderView::make(
    PartRefKey key, const Cas::Resolved & resolved, std::shared_ptr<const Cas::PartManifest> manifest,
    uint64_t validated_at_ms)
{
    return std::make_shared<const PartFolderView>(
        std::move(key), resolved.manifest_id, resolved.manifest_size,
        resolved.published_at_ms, std::move(manifest), validated_at_ms);
}

std::optional<std::string> PartFolderView::projectionDirPrefix(const std::string & file)
{
    if (file.empty())
        return std::nullopt;
    const auto last_slash = file.find_last_of('/');
    const std::string_view last_component
        = last_slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(last_slash + 1);
    if (last_component.ends_with(".proj") || last_component.ends_with(".tmp_proj"))
        return file + "/";
    return std::nullopt;
}

const Cas::ManifestEntry * PartFolderView::findFile(const String & path) const
{
    return Cas::findEntry(manifest_body->entries, path);
}

bool PartFolderView::hasFile(const String & path) const
{
    return findFile(path) != nullptr;
}

std::optional<uint64_t> PartFolderView::fileSize(const String & path) const
{
    if (const auto * e = findFile(path))
        return e->size();
    return std::nullopt;
}

std::optional<String> PartFolderView::inlineBytes(const String & path) const
{
    const auto * e = findFile(path);
    if (e && e->placement == Cas::EntryPlacement::Inline)
        return e->inline_bytes;
    return std::nullopt;
}

std::vector<String> PartFolderView::listChildren(const String & dir_prefix) const
{
    /// First-component collapse over entries. This is bit-identical to the pre-view code for every
    /// real manifest: a part directory always needed first-component collapse, and a projection
    /// directory is STRUCTURALLY FLAT in MergeTree — a projection part folder holds only
    /// column/checksum/metadata files, never a nested subdirectory — so for a projection prefix the
    /// collapse equals the old uncollapsed `substr(prefix)` (no further '/' to collapse). The
    /// collapse is therefore an invariant-preserving unification, not a behavior change.
    std::unordered_set<String> names;
    auto add = [&](const String & full)
    {
        if (!full.starts_with(dir_prefix) || full.size() <= dir_prefix.size())
            return;
        const std::string_view rest = std::string_view(full).substr(dir_prefix.size());
        const auto slash = rest.find('/');
        names.emplace(slash == std::string_view::npos ? rest : rest.substr(0, slash));
    };
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    for (const auto * e = first; e != last; ++e)
        add(e->path);
    return {std::make_move_iterator(names.begin()), std::make_move_iterator(names.end())};
}

bool PartFolderView::hasDirectory(const String & dir_prefix) const
{
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    return first != last;
}

size_t PartFolderView::estimatedBytes() const
{
    /// Conservative cache weight (spec §Memory Bound): fixed overhead + manifest_size (deliberately
    /// over-counts the shared decode — safe direction; Phase 5 notes).
    return 256 + manifest_size;
}

}
