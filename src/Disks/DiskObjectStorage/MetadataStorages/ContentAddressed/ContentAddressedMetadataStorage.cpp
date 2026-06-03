#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <unordered_set>

#include <filesystem>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>
#include <Common/Logger.h>

#include <fmt/format.h>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}

ContentAddressedMetadataStorage::ContentAddressedMetadataStorage(
    ObjectStoragePtr object_storage_,
    String storage_path_prefix_,
    String server_id_,
    String local_scratch_path_,
    ContextPtr context_,
    bool allow_shared_pool_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
    , local_scratch_path(std::move(local_scratch_path_))
    , allow_shared_pool(allow_shared_pool_)
{
    /// The GC thread exists only on the disk-factory path. The GC scans the same object storage under
    /// the same key prefix used by the read/write sides (single source of truth), so its live set and
    /// the read path resolve refs identically (B28).
    if (context_)
        gc_thread = std::make_shared<ContentAddressedGCThread>(
            storage_path_full,
            context_,
            object_storage,
            storage_path_prefix,
            getLogger(fmt::format("{}::ContentAddressedGC", storage_path_full)));
}

void ContentAddressedMetadataStorage::startup()
{
    /// Pool-ownership self-check FIRST (before the GC thread starts): claim a fresh pool, accept our
    /// own pool, and fail closed on an unknown format version or a second/concurrent mounter (B11).
    /// This is the guard that makes it safe to run background GC without the full coordination
    /// protocol (B32): a pool another live server could be writing to is never swept here.
    ContentAddressed::claimPoolOwnership(
        object_storage,
        storage_path_prefix,
        server_id,
        allow_shared_pool,
        getLogger(fmt::format("{}::ContentAddressedPoolMeta", storage_path_full)));

    if (gc_thread)
        gc_thread->startup();
}

void ContentAddressedMetadataStorage::shutdown()
{
    if (gc_thread)
        gc_thread->shutdown();
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    return std::make_shared<ContentAddressedTransaction>(*this, storage_path_prefix, local_scratch_path);
}

std::optional<std::string> ContentAddressedMetadataStorage::readSmallObjectIfExists(const std::string & key) const
{
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
        return std::nullopt;

    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

std::optional<ContentAddressed::PartId> ContentAddressedMetadataStorage::readRefPartId(const std::string & table_uuid, const std::string & part_name) const
{
    auto payload = readSmallObjectIfExists(ContentAddressed::refKey(storage_path_prefix, server_id, table_uuid, part_name).string());
    if (!payload)
        return std::nullopt;
    /// Resolve through the single ref-payload parser shared with the GC live-set scan (B28): the read
    /// path and GC's reachability roots therefore name the SAME part id for a given ref by construction.
    return ContentAddressed::partIdFromRefPayload(*payload);
}

ContentAddressed::PartManifest ContentAddressedMetadataStorage::loadPartManifestOrThrow(const ContentAddressed::PartId & part_id) const
{
    auto bytes = readSmallObjectIfExists(ContentAddressed::partKey(storage_path_prefix, part_id).string());
    if (!bytes)
        throw Exception(
            ErrorCodes::CORRUPTED_DATA,
            "ContentAddressed: live ref points at missing manifest parts/{}",
            part_id.string());
    return ContentAddressed::PartManifest::deserialize(*bytes);
}

ContentAddressed::BlobEntry ContentAddressedMetadataStorage::resolveBlobEntry(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto manifest = loadPartManifestOrThrow(*pid);
    auto it = manifest.blobs.find(p->file);
    if (it == manifest.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", p->file, path);
    return it->second;
}

std::optional<ContentAddressed::RefSidecar> ContentAddressedMetadataStorage::readRefSidecarIfExists(const std::string & table_uuid, const std::string & part_name) const
{
    auto bytes = readSmallObjectIfExists(ContentAddressed::refMetaKey(storage_path_prefix, server_id, table_uuid, part_name).string());
    if (!bytes)
        return std::nullopt;
    return ContentAddressed::RefSidecar::deserialize(*bytes);
}

std::string ContentAddressedMetadataStorage::resolveMutableFileBytes(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty() || !ContentAddressed::isMutablePerPartFile(p->file))
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a mutable per-part file path: {}", path);
    auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name);
    if (!sidecar)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref sidecar for {}", path);
    auto it = sidecar->files.find(p->file);
    if (it == sidecar->files.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: mutable file {} not in sidecar of {}", p->file, path);
    return it->second;
}

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    /// Non-part files resolve to a direct object key: a table-level file (e.g. format_version.txt)
    /// at tableFileKey, any other (generic disk-level) file at its verbatim diskFileKey.
    if (!ContentAddressed::isPartFilePath(path))
    {
        std::string key;
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            key = ContentAddressed::tableFileKey(storage_path_prefix, server_id, tf->table_uuid, tf->tail);
        else
            key = ContentAddressed::diskFileKey(storage_path_prefix, path);
        return object_storage->tryGetObjectMetadata(key, /*with_tags=*/false).has_value();
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;

    /// A mutable per-part file is overlaid from the per-ref sidecar, never the manifest. A missing
    /// sidecar entry is a missing file (fail-close): the manifest never carries these files.
    if (ContentAddressed::isMutablePerPartFile(p->file))
        return object_storage->tryGetObjectMetadata(
            ContentAddressed::refMutableFileKey(storage_path_prefix, server_id, p->table_uuid, p->part_name, p->file).string(),
            /*with_tags=*/false).has_value();

    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        return false;
    auto manifest = loadPartManifestOrThrow(*pid);
    return manifest.blobs.contains(p->file);
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        // Table dir exists iff it has at least one ref (part). Skip per-ref sidecars (.meta): they are
        // not refs and a sidecar without a ref must not make a dropped table dir look non-empty.
        RelativePathsWithMetadata files;
        object_storage->listObjects(ContentAddressed::refsPrefix(storage_path_prefix, server_id, *uuid), files, 0);
        for (const auto & file : files)
            if (!ContentAddressed::isRefMetaKey(file->relative_path))
                return true;
        return false;
    }
    if (auto p = ContentAddressed::parsePartFilePath(path); p && p->file.empty())
    {
        // Part dir exists iff its ref is present.
        return readRefPartId(p->table_uuid, p->part_name).has_value();
    }
    return false;
}

bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    return existsFile(path) || existsDirectory(path);
}

uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    /// Non-part files resolve to a direct object key: table-level (tableFileKey) or generic
    /// disk-level (verbatim diskFileKey).
    if (!ContentAddressed::isPartFilePath(path))
    {
        std::string key;
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            key = ContentAddressed::tableFileKey(storage_path_prefix, server_id, tf->table_uuid, tf->tail);
        else
            key = ContentAddressed::diskFileKey(storage_path_prefix, path);
        auto meta = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
        if (!meta)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
        return meta->size_bytes;
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    /// Mutable per-part file: size of the per-file sidecar object (fail-close if absent).
    if (ContentAddressed::isMutablePerPartFile(p->file))
    {
        auto meta = object_storage->tryGetObjectMetadata(
            ContentAddressed::refMutableFileKey(storage_path_prefix, server_id, p->table_uuid, p->part_name, p->file).string(),
            /*with_tags=*/false);
        if (!meta)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no sidecar object for {}", path);
        return meta->size_bytes;
    }

    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto manifest = loadPartManifestOrThrow(*pid);
    auto it = manifest.blobs.find(p->file);
    if (it == manifest.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", p->file, path);
    return it->second.size;
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string & path) const
{
    // A part directory (<uuid[:3]>/<uuid>/<part>) has no single blob; report the manifest object's
    // mtime. MergeTree calls this on the part directory while loading parts (modification_time).
    // Timestamps are derived for content addressing, so the manifest's mtime is a reasonable proxy.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && p->file.empty())
    {
        auto pid = readRefPartId(p->table_uuid, p->part_name);
        if (!pid)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
        auto metadata = object_storage->getObjectMetadata(ContentAddressed::partKey(storage_path_prefix, *pid).string(), /*with_tags=*/false);
        return metadata.last_modified;
    }

    // A detached part DIRECTORY (<uuid[:3]>/<uuid>/detached/<detached_part>) is not a part file: the
    // detached part is stored under a ref named "detached" whose manifest keys are shaped
    // <detached_part>/<file> (B36), so parsePartFilePath reports part_name="detached" and a NON-empty
    // file equal to the <detached_part> directory name (no further '/'). system.detached_parts reads
    // modification_time by calling getLastModified on exactly this directory (StorageSystemDetachedParts
    // -> disk->getLastModified(data_path/detached/<dir_name>)). It has no single blob; report the
    // "detached" ref manifest object's mtime, mirroring the regular part-dir branch above.
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && p->part_name == ContentAddressed::kDetachedDirName && !p->file.empty() && p->file.find('/') == std::string::npos)
    {
        auto pid = readRefPartId(p->table_uuid, p->part_name);
        if (!pid)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
        auto metadata = object_storage->getObjectMetadata(ContentAddressed::partKey(storage_path_prefix, *pid).string(), /*with_tags=*/false);
        return metadata.last_modified;
    }

    // Mirror MetadataStorageFromPlainObjectStorage: report the resolved blob object's mtime.
    auto objects = getStorageObjects(path);
    chassert(!objects.empty());
    auto metadata = object_storage->getObjectMetadata(objects.front().remote_path, /*with_tags=*/false);
    return metadata.last_modified;
}

std::vector<std::string> ContentAddressedMetadataStorage::listDirectory(const std::string & path) const
{
    // Table dir <uuid[:3]>/<uuid>[/]: list the part names from refs/.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        // Mirror MetadataStorageFromPlainObjectStorage::listDirectory child-derivation:
        // list under the prefix, strip it, and take the immediate child component.
        std::string prefix = ContentAddressed::refsPrefix(storage_path_prefix, server_id, *uuid);
        RelativePathsWithMetadata files;
        object_storage->listObjects(prefix, files, 0);

        std::unordered_set<std::string> result;
        for (const auto & elem : files)
        {
            const auto & p = elem->relative_path;
            // Per-ref sidecars (.meta) live under the same refs/ prefix but are not parts; skip them
            // so a part dir never appears twice (once as <part>, once as <part>.meta).
            if (ContentAddressed::isRefMetaKey(p))
                continue;
            const auto child_pos = p.find(prefix);
            if (child_pos != 0)
                continue;
            const auto rest = p.substr(prefix.size());
            const auto slash_pos = rest.find('/');
            // string::npos is ok: take the whole remainder.
            result.emplace(rest.substr(0, slash_pos));
        }
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }

    // Part dir <uuid[:3]>/<uuid>/<part>[/]: list the logical file names from the manifest.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && p->file.empty())
    {
        auto pid = readRefPartId(p->table_uuid, p->part_name);
        if (!pid)
            return {}; // absent ref => empty listing
        auto manifest = loadPartManifestOrThrow(*pid); // missing manifest for a present ref => CORRUPTED_DATA

        // The "detached" namespace is not a part: it is a container of detached part *directories*
        // (detached/<detached_part>/<file>). A detach clones a part file-by-file into
        // detached/<detached_part>/<file>, so the ref named "detached" carries manifest keys shaped
        // <detached_part>/<file>. Enumerating it (system.detached_parts via getDetachedParts) must
        // yield the detached part DIRECTORY names, not the files inside them (and never a dir-stripped
        // mutable sidecar file such as metadata_version.txt) — B36.
        if (p->part_name == ContentAddressed::kDetachedDirName)
        {
            std::unordered_set<std::string> dirs;
            for (const auto & [file, _] : manifest.blobs)
            {
                const auto slash_pos = file.find('/');
                // A nested <detached_part>/<file> key contributes its first component. A bare key with
                // no '/' is not a valid detached part directory entry (e.g. a mutable file whose
                // directory prefix was dropped) and is skipped, so it never surfaces as a part dir.
                if (slash_pos != std::string::npos)
                    dirs.emplace(file.substr(0, slash_pos));
            }
            return std::vector<std::string>(std::make_move_iterator(dirs.begin()), std::make_move_iterator(dirs.end()));
        }

        std::vector<std::string> result;
        result.reserve(manifest.blobs.size());
        for (const auto & [file, _] : manifest.blobs)
            result.push_back(file);
        /// Overlay the mutable per-part files from the per-ref sidecar (they live per-ref, not in the
        /// shared manifest), so the part dir lists its full file set just like a normal part.
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                result.push_back(file);
        return result;
    }

    // Root or unrecognized path.
    return {};
}

bool ContentAddressedMetadataStorage::isDirectoryEmpty(const std::string & path) const
{
    // A PART directory <uuid[:3]>/<uuid>/<part> has no real sub-objects: its files are virtual, derived
    // from the manifest. A content-addressed part is removed authoritatively by unlinking its ref (see
    // ContentAddressedTransaction::removeDirectory), so the MergeTree fast-removal path's per-file
    // unlinks are no-ops and the manifest-derived listing never empties. DiskObjectStorage::removeDirectory
    // checks isDirectoryEmpty BEFORE removing and would throw CANNOT_RMDIR on every part removal, forcing
    // a noisy recursive fallback (logged as <Error>) even though the result is correct (B45). Report a
    // part directory as empty so removeDirectory proceeds straight to the ref-unlink. The detached
    // namespace and TABLE dirs are NOT part dirs and keep the default iterateDirectory-based emptiness
    // (so e.g. the DROP TABLE non-empty-data-dir guard still sees a table dir with live refs as non-empty).
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && p->file.empty() && p->part_name != ContentAddressed::kDetachedDirName)
        return true;

    return !iterateDirectory(path)->isValid();
}

DirectoryIteratorPtr ContentAddressedMetadataStorage::iterateDirectory(const std::string & path) const
{
    // Mirror MetadataStorageFromPlainObjectStorage::iterateDirectory: prepend the path to each
    // child name, since iterateDirectory includes the path while listDirectory does not.
    auto names = listDirectory(path);
    std::vector<fs::path> fs_paths;
    fs_paths.reserve(names.size());
    for (const auto & child : names)
        fs_paths.push_back(fs::path(path) / child);
    return std::make_unique<StaticDirectoryIterator>(std::move(fs_paths));
}

StoredObjects ContentAddressedMetadataStorage::getStorageObjects(const std::string & path) const
{
    /// Non-part files resolve to a single direct object key (no manifest/ref/blob): a table-level
    /// file at tableFileKey, any other (generic disk-level) file at its verbatim diskFileKey.
    if (!ContentAddressed::isPartFilePath(path))
    {
        std::string key;
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            key = ContentAddressed::tableFileKey(storage_path_prefix, server_id, tf->table_uuid, tf->tail);
        else
            key = ContentAddressed::diskFileKey(storage_path_prefix, path);
        auto meta = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
        if (!meta)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
        return {StoredObject(key, path, meta->size_bytes)};
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    /// Mutable per-part file: resolve to its OWN per-file sidecar object, whose bytes are EXACTLY this
    /// file's content (not the manifest -> blob path). Fail-close if the part never wrote this file.
    if (ContentAddressed::isMutablePerPartFile(p->file))
    {
        const std::string file_key
            = ContentAddressed::refMutableFileKey(storage_path_prefix, server_id, p->table_uuid, p->part_name, p->file).string();
        auto meta = object_storage->tryGetObjectMetadata(file_key, /*with_tags=*/false);
        if (!meta)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no sidecar object for {}", path);
        return {StoredObject(file_key, path, meta->size_bytes)};
    }

    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto manifest = loadPartManifestOrThrow(*pid);
    auto it = manifest.blobs.find(p->file);
    if (it == manifest.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", p->file, path);

    const auto & e = it->second;
    /// Resolve under the same common key prefix used on the write side (single source of truth:
    /// storage_path_prefix), so blobs are read from where ContentAddressedWriteBuffer stored them.
    /// e.key is the BARE BlobHash; blobKey projects it to the full BlobObjectKey, .string() at the
    /// object-storage boundary.
    return {StoredObject(ContentAddressed::blobKey(storage_path_prefix, e.key).string(), path, e.size)};
}

}
