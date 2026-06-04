#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <chrono>
#include <unordered_set>

#include <filesystem>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>
#include <Common/Logger.h>
#include <Common/getRandomASCIIString.h>

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
            server_id,
            gc_lock,
            in_flight_pinned_blobs,
            getLogger(fmt::format("{}::ContentAddressedGC", storage_path_full)));
}

void ContentAddressedMetadataStorage::unpinBlob(const std::string & blob_key)
{
    std::lock_guard<std::mutex> gc_guard(*gc_lock);
    in_flight_pinned_blobs->erase(blob_key);
}

std::optional<ContentAddressedMetadataStorage::RelinkPin>
ContentAddressedMetadataStorage::relinkPin(const ContentAddressed::PartId & part_id)
{
    /// STEP 1 — PIN (spec §4). The relink uploads NO blobs (another replica already wrote them), so it
    /// creates none of the per-blob WriteSession pins a normal write does. Without a pin, a concurrent
    /// source-ref drop + GC sweep on ANY mounter would see this part_id named by no ref and no session
    /// and reclaim its blobs in the window before this server publishes its own ref → a dangling ref /
    /// lost part. So we open a durable WriteSession seeded with the EXISTING part's blob hash set (read
    /// from parts/<part_id>) and persist it BEFORE trusting the source: from now until release, a remote
    /// sweep treats every listed hash as reachable (sessionPinnedBlobs), closing the window.
    const std::string manifest_key = ContentAddressed::partKey(storage_path_prefix, part_id).string();
    auto manifest_bytes = readSmallObjectIfExists(manifest_key);
    if (!manifest_bytes)
        /// The part_id this server was asked to relink does not exist in the pool: relink not possible.
        /// Publish NOTHING; the caller falls back to a byte fetch. (A present-but-corrupt manifest throws
        /// in deserialize below — fail-closed, never best-effort.)
        return std::nullopt;

    const ContentAddressed::PartManifest manifest = ContentAddressed::PartManifest::deserialize(*manifest_bytes);

    /// Seed the session's pending set with the EXISTING blob hashes the manifest names, AND identify the
    /// part_id/manifest object itself (session.part_id). This is the relink analogue of the write path's
    /// recordBlobInSession, except the hash set is KNOWN up front rather than accumulated per upload.
    ContentAddressed::WriteSession session;
    session.server_id = server_id;
    session.fence_token = 0;
    session.part_id = part_id;
    session.pending.reserve(manifest.blobs.size());
    for (const auto & [file, entry] : manifest.blobs)
        session.pending.push_back(entry.key);

    /// Advisory lease: a liveness HINT only (a remote sweep treats an EXPIRED session as reclaimable so a
    /// crashed relink cannot pin blobs forever). Mirrors persistSession in the transaction.
    constexpr UInt64 lease_seconds = 300;
    const UInt64 now_unix = static_cast<UInt64>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    session.lease_deadline_unix = now_unix + lease_seconds;

    RelinkPin pin;
    pin.session_id = server_id + "-relink-" + getRandomASCIIString(24);
    pin.part_id = part_id;
    pin.open = true;

    /// Persist the session object at a unique key owned by this relink (no CAS needed — no other writer
    /// touches it). After finalize the cross-mounter pin is durable and the blobs are protected.
    const std::string key = ContentAddressed::sessionKey(storage_path_prefix, pin.session_id);
    const std::string bytes = session.serialize();
    auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
    out->write(bytes.data(), bytes.size());
    out->finalize();

    return pin;
}

bool ContentAddressedMetadataStorage::relinkRevalidate(const ContentAddressed::PartId & part_id) const
{
    /// STEP 2 — RE-VALIDATE (spec §4). Under the held pin, confirm the manifest still exists and every
    /// blob it names is present. A missing manifest or blob means the relink is not possible: return
    /// false so the caller releases the pin and falls back to a byte fetch — a ref must NEVER be
    /// published to a missing blob (that would be a dangling ref / data loss).
    auto manifest_bytes = readSmallObjectIfExists(ContentAddressed::partKey(storage_path_prefix, part_id).string());
    if (!manifest_bytes)
        return false;

    const ContentAddressed::PartManifest manifest = ContentAddressed::PartManifest::deserialize(*manifest_bytes);
    for (const auto & [file, entry] : manifest.blobs)
    {
        const std::string blob_key = ContentAddressed::blobKey(storage_path_prefix, entry.key).string();
        if (!object_storage->tryGetObjectMetadata(blob_key, /*with_tags=*/false).has_value())
            return false;
    }
    return true;
}

void ContentAddressedMetadataStorage::relinkPublishRef(
    const std::string & table_uuid,
    const std::string & part_name,
    const ContentAddressed::PartId & part_id,
    const std::map<std::string, std::string> & sidecar_values)
{
    /// STEP 3 — PUBLISH (spec §4). Write the per-ref sidecar (the per-part MUTABLE files carried in the
    /// fetch header — uuid.txt / txn_version.txt / metadata_version.txt) THEN the ref, mirroring
    /// ContentAddressedTransaction::commit's publish order (sidecar before the ref, the ref last, since
    /// the ref is the publishing step). The blobs and manifest already exist (another replica wrote
    /// them); this writes only the small pointer objects.
    if (!sidecar_values.empty())
    {
        /// Per-file objects FIRST: each mutable file's bytes verbatim in its own tiny object so the read
        /// path (getStorageObjects -> readObject) returns exactly that file's bytes.
        for (const auto & [file, bytes] : sidecar_values)
        {
            const std::string file_key
                = ContentAddressed::refMutableFileKey(storage_path_prefix, server_id, table_uuid, part_name, file).string();
            auto file_out = object_storage->writeObject(StoredObject(file_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }

        /// The bundle sidecar (the atomic per-part index of mutable files), written before the ref.
        ContentAddressed::RefSidecar sidecar;
        sidecar.files = sidecar_values;
        const std::string meta_key = ContentAddressed::refMetaKey(storage_path_prefix, server_id, table_uuid, part_name).string();
        const std::string meta_bytes = sidecar.serialize();
        auto meta_out = object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }

    /// Publish the ref last (the commit point): refKey(self, table_uuid, part_name) -> part_id.
    /// The on-disk payload is the same versioned ref-payload struct the write path publishes, so the read
    /// path and the GC live-set scan resolve it identically (B28).
    const std::string ref_key = ContentAddressed::refKey(storage_path_prefix, server_id, table_uuid, part_name).string();
    const std::string ref_payload = ContentAddressed::serializeRefPayload(part_id);
    auto ref_out = object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
    ref_out->write(ref_payload.data(), ref_payload.size());
    ref_out->finalize();
}

void ContentAddressedMetadataStorage::relinkReleasePin(RelinkPin & pin) noexcept
{
    /// STEP 4 — RELEASE (spec §4). The published ref now keeps every referenced blob reachable for a
    /// sweep on ANY mounter, so the cross-mounter pin is no longer needed; remove it. Best-effort: a
    /// failure to remove the session object only over-retains the pin (conservative — never data loss;
    /// the lease expires anyway). Idempotent.
    if (!pin.open)
        return;
    try
    {
        object_storage->removeObjectIfExists(StoredObject(ContentAddressed::sessionKey(storage_path_prefix, pin.session_id)));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
    pin.open = false;
}

bool ContentAddressedMetadataStorage::relinkExistingPart(
    const std::string & table_uuid,
    const std::string & part_name,
    const ContentAddressed::PartId & part_id,
    const std::map<std::string, std::string> & sidecar_values)
{
    /// The full pin-before-publish relink (spec §4), in STRICT order. The four steps are also public so a
    /// test can interleave a concurrent source-ref drop + GC sweep between PIN and PUBLISH.
    auto pin = relinkPin(part_id);
    if (!pin)
        return false; /// the manifest is absent — relink not possible; the caller falls back to a byte fetch.

    /// RE-VALIDATE under the pin. If the source vanished (manifest or a blob missing) between the pin and
    /// now, release the pin and signal "not possible" — never publish a ref to a missing blob.
    if (!relinkRevalidate(part_id))
    {
        relinkReleasePin(*pin);
        return false;
    }

    relinkPublishRef(table_uuid, part_name, part_id, sidecar_values);
    relinkReleasePin(*pin);
    return true;
}

void ContentAddressedMetadataStorage::startup()
{
    /// Pool-ownership self-check FIRST (before the GC thread starts): claim a fresh pool, accept our
    /// own pool, and fail closed on an unknown format version or a second/concurrent mounter (B11).
    /// This is the guard that makes it safe to run background GC without the full coordination
    /// protocol (B32): a pool another live server could be writing to is never swept here.
    pool_uuid = ContentAddressed::claimPoolOwnership(
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

std::optional<ContentAddressed::PartId> ContentAddressedMetadataStorage::getPartId(const std::string & part_path) const
{
    /// The fetch-by-relink SENDER path: resolve a part directory path to the part_id named by this
    /// server's own ref. The path is the part STORAGE's relative path (`store/<uuid[:3]>/<uuid>/<part>/`);
    /// `parsePartFilePath` anchors on the uuid pair and is robust to the trailing slash / `store/` prefix.
    auto p = ContentAddressed::parsePartFilePath(part_path);
    if (!p || !p->file.empty())
        return std::nullopt;
    return readRefPartId(p->table_uuid, p->part_name);
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
    // A single detached part DIRECTORY <uuid[:3]>/<uuid>/detached/<detached_part>: its files do not
    // live as their own ref — they are re-keyed inside the shared "detached" ref's manifest (and the
    // per-ref sidecar) as <detached_part>/<file> (B36). The directory exists iff that shared ref carries
    // at least one key with prefix <detached_part>/. MergeTreeData::getDiskForDetachedPart relies on this
    // to locate a detached part for ATTACH PART.
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && p->part_name == ContentAddressed::kDetachedDirName && !p->file.empty() && p->file.find('/') == std::string::npos)
    {
        const std::string prefix = p->file + "/";
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    return true;
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    return true;
        return false;
    }
    // A projection DIRECTORY <uuid[:3]>/<uuid>/<part>/<proj>.proj: a projection's files live nested in
    // the PARENT part's manifest under the key prefix <proj>.proj/ (Approach A — no separate part/ref).
    // The directory exists iff the part's manifest (or per-ref sidecar) carries at least one key with
    // that prefix. This is what makes IMergeTreeDataPart::loadProjections (which calls
    // existsDirectory("<proj>.proj")) discover a projection on a content-addressed part. Mirrors the
    // single-detached-part-dir branch above; keyed on the .proj/.tmp_proj suffix instead of "detached".
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && !p->file.empty() && p->file.find('/') == std::string::npos
        && (p->file.ends_with(".proj") || p->file.ends_with(".tmp_proj")))
    {
        const std::string prefix = p->file + "/";
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    return true;
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    return true;
        return false;
    }
    // A table-level SUBDIRECTORY <uuid[:3]>/<uuid>/<subdir>[/...] (e.g. deduplication_logs/): table-level
    // files may live in subdirectories under the table's files/ namespace. The directory exists iff at
    // least one verbatim table file is keyed under tableFileKey(<subdir>/...). This is what lets the
    // non-replicated deduplication log (deduplication_logs/deduplication_log_N.txt) discover its dir on
    // load just as it would on a plain disk.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string dir_prefix = ContentAddressed::tableFileKey(storage_path_prefix, server_id, tf->table_uuid, tf->tail + "/");
        RelativePathsWithMetadata files;
        object_storage->listObjects(dir_prefix, files, 1);
        return !files.empty();
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
    // Table dir <uuid[:3]>/<uuid>[/]: list the part names from refs/ AND the table-level file names
    // from files/ (e.g. format_version.txt, mutation_N.txt). A real disk's table data dir lists both
    // its part directories and its table-level files, and consumers depend on it: in particular
    // StorageMergeTree::loadMutations scans this dir for `mutation_*` entries to populate
    // system.mutations, so omitting the verbatim table-level files made every CA table report an empty
    // mutation list even though the mutations ran.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        std::unordered_set<std::string> result;

        // Mirror MetadataStorageFromPlainObjectStorage::listDirectory child-derivation: list under the
        // prefix, strip it, and take the immediate child component.
        auto collect_children = [&](const std::string & prefix, bool skip_ref_meta)
        {
            RelativePathsWithMetadata objects;
            object_storage->listObjects(prefix, objects, 0);
            for (const auto & elem : objects)
            {
                const auto & p = elem->relative_path;
                // Per-ref sidecars (.meta) live under the refs/ prefix but are not parts; skip them so a
                // part dir never appears twice (once as <part>, once as <part>.meta).
                if (skip_ref_meta && ContentAddressed::isRefMetaKey(p))
                    continue;
                if (p.find(prefix) != 0)
                    continue;
                const auto rest = p.substr(prefix.size());
                const auto slash_pos = rest.find('/');
                // string::npos is ok: take the whole remainder.
                result.emplace(rest.substr(0, slash_pos));
            }
        };

        collect_children(ContentAddressed::refsPrefix(storage_path_prefix, server_id, *uuid), /*skip_ref_meta=*/true);
        collect_children(ContentAddressed::tableFilesPrefix(storage_path_prefix, server_id, *uuid), /*skip_ref_meta=*/false);

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

        /// Collapse nested keys to their first path component so a projection's files (stored as
        /// <proj>.proj/<file> in this same manifest, Approach A) surface as a SINGLE <proj>.proj
        /// directory entry rather than a flood of nested files; top-level files (no '/') are emitted
        /// verbatim. Both projection discovery (iterate() + existsDirectory("<proj>.proj")) and a clean
        /// top-level column listing depend on this. Overlay the mutable per-part files from the per-ref
        /// sidecar (they live per-ref, not in the shared manifest) the same way.
        std::unordered_set<std::string> result;
        auto add_first_component = [&result](const std::string & file)
        {
            const auto slash = file.find('/');
            result.emplace(slash == std::string::npos ? file : file.substr(0, slash));
        };
        for (const auto & [file, _] : manifest.blobs)
            add_first_component(file);
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                add_first_component(file);
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }

    // A single detached part DIRECTORY <uuid[:3]>/<uuid>/detached/<detached_part>: this is NOT a part of
    // its own (no ref named <detached_part>) — its files are re-keyed inside the shared "detached" ref's
    // manifest (and the per-ref sidecar) as <detached_part>/<file> (B36). List the detached part's
    // complete inner file set exactly as an active part dir lists its files: load the shared "detached"
    // ref, then for every key shaped <detached_part>/<inner> strip the <detached_part>/ prefix and emit
    // <inner>. Overlay the per-ref sidecar the same way so the mutable per-part files (e.g.
    // metadata_version.txt) appear too. A whole-part clone of a detached source (ATTACH PARTITION /
    // ATTACH PART) enumerates this dir; returning the inner files (not the <detached_part>/<inner> keys,
    // not empty) is what makes the clone's manifest complete (B-Task W2).
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && p->part_name == ContentAddressed::kDetachedDirName && !p->file.empty() && p->file.find('/') == std::string::npos)
    {
        const std::string prefix = p->file + "/";
        std::unordered_set<std::string> result;
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }

    // A projection DIRECTORY <uuid[:3]>/<uuid>/<part>/<proj>.proj: list the projection's inner file
    // names by stripping the <proj>.proj/ prefix from the PARENT part's manifest (and per-ref sidecar)
    // keys, so the projection's child DataPartStorage enumerates and reads exactly its own files.
    // Mirrors the single-detached-part-dir listing branch; keyed on the .proj/.tmp_proj suffix.
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && !p->file.empty() && p->file.find('/') == std::string::npos
        && (p->file.ends_with(".proj") || p->file.ends_with(".tmp_proj")))
    {
        const std::string prefix = p->file + "/";
        std::unordered_set<std::string> result;
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }

    // A table-level SUBDIRECTORY <uuid[:3]>/<uuid>/<subdir>[/...] (e.g. deduplication_logs/): list the
    // verbatim table files keyed under tableFileKey(<subdir>/...), collapsing each to its first path
    // component after the subdir prefix (so a nested file appears as its child entry, a deeper subdir as
    // a single dir entry). Mirrors the table-dir branch's first-component collapse. This lets the
    // non-replicated deduplication log enumerate deduplication_log_N.txt files on load.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        const std::string dir_prefix = ContentAddressed::tableFileKey(storage_path_prefix, server_id, tf->table_uuid, tf->tail + "/");
        RelativePathsWithMetadata files;
        object_storage->listObjects(dir_prefix, files, 0);
        std::unordered_set<std::string> result;
        for (const auto & elem : files)
        {
            const auto & rp = elem->relative_path;
            if (rp.find(dir_prefix) != 0)
                continue;
            const auto rest = rp.substr(dir_prefix.size());
            const auto slash_pos = rest.find('/');
            result.emplace(rest.substr(0, slash_pos));
        }
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
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

    // A projection subdirectory <uuid[:3]>/<uuid>/<part>/<proj>.proj has the same shape: its files are
    // virtual, nested in the PARENT part's manifest under the <proj>.proj/ prefix (Approach A — no separate
    // part/ref). Removing a projection subdir on CA is a no-op — the projection's blobs are reclaimed when
    // the part's ref is unlinked — but the default iterateDirectory-based check lists the projection's inner
    // files and reports the subdir as NON-empty, so DiskObjectStorage::removeDirectory would attempt rmdir on
    // it and throw CANNOT_RMDIR (logged as <Error>), forcing a noisy recursive fallback even though the result
    // is correct (B60). Report it empty so removeDirectory skips the failing rmdir, exactly as for the part dir
    // itself (B45). Gated on a SINGLE-component .proj/.tmp_proj file, the same gate the projection branches in
    // existsDirectory/listDirectory use — so this never matches the detached namespace or a real table dir.
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && !p->file.empty() && p->file.find('/') == std::string::npos
        && (p->file.ends_with(".proj") || p->file.ends_with(".tmp_proj")))
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
