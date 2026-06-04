#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/DiskType.h>
#include <Disks/WriteMode.h>

#include <Common/Exception.h>
#include <Common/ObjectStorageKey.h>
#include <Common/getRandomASCIIString.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadSettings.h>
#include <IO/copyData.h>
#include <IO/WriteBufferFromString.h>

#include <base/hex.h>

#include <chrono>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
    extern const int CORRUPTED_DATA;
    extern const int CANNOT_OPEN_FILE;
    extern const int FILE_DOESNT_EXIST;
}

ContentAddressedTransaction::ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_, std::string key_prefix_, std::string local_scratch_path_)
    : metadata_storage(metadata_storage_)
    , key_prefix(std::move(key_prefix_))
    , local_scratch_path(std::move(local_scratch_path_))
{
}

ContentAddressedTransaction::~ContentAddressedTransaction()
{
    /// B52: an uncommitted transaction (an aborted/cancelled insert, or a directory-only transaction
    /// that staged blobs then threw) still holds its in-flight pins; release them so they do not block
    /// GC forever. A committed transaction already cleared pinned_blob_keys under gc_lock, so this is a
    /// no-op for it.
    releasePinnedBlobs();

    /// M8: an uncommitted transaction also leaves its cross-mounter WriteSession object behind. Its
    /// lease would expire anyway, but remove it eagerly (best-effort) so an aborted insert leaves no
    /// lingering session. A committed transaction already cleared it in commit(), so this is a no-op.
    releaseSession();
}

void ContentAddressedTransaction::releasePinnedBlobs() noexcept
{
    if (pinned_blob_keys.empty())
        return;
    try
    {
        for (const auto & key : pinned_blob_keys)
            metadata_storage.unpinBlob(key);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Best-effort: a failure to drop a pin only over-retains a blob (conservative — never data
        /// loss). Never let it escape a destructor.
    }
    pinned_blob_keys.clear();
}

void ContentAddressedTransaction::recordBlobInSession(const ContentAddressed::BlobHash & blob_hash)
{
    /// Called from the write buffer's finalizeImpl under the GC lock, BEFORE the blob is uploaded.
    /// Lazily open the session on the first recorded blob: generate a unique key (the server id plus a
    /// random suffix, so two concurrent transactions on the same mounter never collide), stamp the
    /// owning identity and an advisory lease, and leave the fence token 0 (Phase C wires real fencing).
    if (!session_open)
    {
        session_id = metadata_storage.server_id + "-" + getRandomASCIIString(24);

        session = ContentAddressed::WriteSession{};
        session.server_id = metadata_storage.server_id;
        session.fence_token = 0;
        /// The committed content PartId is only known at commit (it is derived from the full blob set),
        /// so the session identifies the part by the writer's part name — enough for a remote GC to
        /// attribute the pin and for diagnostics.
        session.part_id = ContentAddressed::PartId(part_name);
        session_open = true;
    }

    session.pending.push_back(blob_hash);
    persistSession();
}

void ContentAddressedTransaction::persistSession()
{
    /// RENEW the advisory lease on EVERY rewrite. The lease is a liveness HINT only — a remote GC treats
    /// an EXPIRED session as reclaimable so a crashed writer cannot pin blobs forever — but it MUST stay
    /// live while THIS write is making progress. If the deadline were stamped only once at open, a part
    /// whose write (or commit) outran the lease would expire its OWN session mid-flight; a remote sweep
    /// would then drop the pin and could reclaim an already-uploaded-but-not-yet-referenced blob before
    /// the ref is published → a dangling ref / data loss. Advancing the deadline on each rewrite (per
    /// blob, and again at commit before the ref is published) keeps a live root over every blob this
    /// part is about to reference, for as long as the writer is alive and progressing.
    constexpr UInt64 lease_seconds = 300;
    const UInt64 now_unix = static_cast<UInt64>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    session.lease_deadline_unix = now_unix + lease_seconds;

    /// Rewrite of the OWNER's uniquely-keyed session object: no CAS needed (no other writer touches this
    /// key). Persisting BEFORE the blob is uploaded is the whole point — the cross-mounter pin must be
    /// durable before the blob can be observed by a remote sweep.
    const std::string key = ContentAddressed::sessionKey(key_prefix, session_id);
    const std::string bytes = session.serialize();

    /// writeObject (LocalObjectStorage) runs fs::create_directories(parent) then opens the file, while
    /// removeObject (a SIBLING transaction's releaseSession) unlinks the object AND prunes empty parent
    /// dirs. Under concurrent inserts on the same mounter the shared sessions/ dir can be rmdir'd in the
    /// window between this create_directories and the open, surfacing CANNOT_OPEN_FILE/ENOENT on the
    /// INSERT. Retry: each attempt re-runs create_directories and almost always wins the next time. Fail
    /// closed (rethrow) after a bounded number of attempts so the cross-mounter pin is never silently
    /// dropped. A real object store has no directories, so this races only on a local-backed pool.
    constexpr size_t max_attempts = 5;
    for (size_t attempt = 1; ; ++attempt)
    {
        try
        {
            auto out = metadata_storage.object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size());
            out->finalize();
            return;
        }
        catch (const Exception & e)
        {
            if (attempt < max_attempts
                && (e.code() == ErrorCodes::CANNOT_OPEN_FILE || e.code() == ErrorCodes::FILE_DOESNT_EXIST))
                continue;
            throw;
        }
    }
}

void ContentAddressedTransaction::releaseSession() noexcept
{
    if (!session_open)
        return;
    try
    {
        metadata_storage.object_storage->removeObjectIfExists(
            StoredObject(ContentAddressed::sessionKey(key_prefix, session_id)));
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Best-effort: a failure to remove the session object only over-retains the pin (conservative
        /// — never data loss; the lease expires anyway). Never let it escape a destructor.
    }
    session_open = false;
}

void ContentAddressedTransaction::rememberTarget(const std::string & path)
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: not a part file path: {}", path);

    /// All files of one commit must belong to the same (table_uuid, part_name).
    if (table_uuid.empty() && part_name.empty())
    {
        table_uuid = p->table_uuid;
        part_name = p->part_name;
        frozen_backup_name = p->backup_name; /// empty for a live part; the FREEZE backup name otherwise
        frozen_table_dir = p->shadow_table_dir; /// empty for a live part; the shadow table dir otherwise
    }
    else if (table_uuid != p->table_uuid || part_name != p->part_name || frozen_backup_name != p->backup_name)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: a single transaction must write one part, got {}/{} (backup '{}') and {}/{} (backup '{}')",
            table_uuid, part_name, frozen_backup_name, p->table_uuid, p->part_name, p->backup_name);
    }
}

void ContentAddressedTransaction::recordBlob(const std::string & path, ContentAddressed::BlobEntry entry)
{
    auto p = ContentAddressed::parsePartFilePath(path);
    chassert(p && !p->file.empty());
    recorded[p->file] = std::move(entry);
}

std::optional<StoredObjects> ContentAddressedTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    /// A content file: its blob is already uploaded (recorded under the in-part file name). Mirror the
    /// committed getStorageObjects resolve: project the BARE BlobHash to the full blob object key.
    if (auto it = recorded.find(p->file); it != recorded.end())
        return StoredObjects{StoredObject(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size)};
    /// Mutable per-part files are staged inline (recorded_mutable), not as a blob object — they have no
    /// StoredObject; readers must use tryReadFileInFlight for them. Return nullopt here.
    return {};
}

std::unique_ptr<ReadBufferFromFileBase> ContentAddressedTransaction::tryReadFileInFlight(
    const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return nullptr;
    if (auto it = recorded.find(p->file); it != recorded.end())
    {
        StoredObject obj(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size);
        return metadata_storage.object_storage->readObject(obj, settings, read_hint);
    }
    if (auto it = recorded_mutable.find(p->file); it != recorded_mutable.end())
        return std::make_unique<ReadBufferFromOwnMemoryFile>(path, it->second); // inline staged bytes
    return nullptr;
}

std::optional<uint64_t> ContentAddressedTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    if (auto it = recorded.find(p->file); it != recorded.end())
        return it->second.size;
    if (auto it = recorded_mutable.find(p->file); it != recorded_mutable.end())
        return static_cast<uint64_t>(it->second.size());
    return {};
}

std::unique_ptr<WriteBufferFromFileBase> ContentAddressedTransaction::writeFile(
    const std::string & path,
    size_t buf_size,
    WriteMode mode,
    const WriteSettings & settings)
{
    /// Classification of a non-part write path:
    ///   - table-level file (e.g. format_version.txt) -> direct object key (tableFileKey);
    ///   - any other (generic disk-level) file, e.g. the server's startup access-check probe
    ///     clickhouse_access_check_<uuid> written at the disk root -> verbatim object key
    ///     (diskFileKey).
    /// Both are written verbatim — no content addressing, no ref, no manifest. They are durable on
    /// finalize and are not tracked by commit.
    /// TODO(phase4-gc): non-part objects are GC roots.
    if (!ContentAddressed::isPartFilePath(path))
    {
        std::string key;
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            key = ContentAddressed::tableFileKey(key_prefix, metadata_storage.server_id, tf->table_uuid, tf->tail);
        else
            key = ContentAddressed::diskFileKey(key_prefix, path);

        return metadata_storage.object_storage->writeObject(StoredObject(key, path), mode, /*attributes=*/std::nullopt, buf_size, settings);
    }

    rememberTarget(path);

    auto p = ContentAddressed::parsePartFilePath(path);
    const std::string file = p->file;

    /// A MUTABLE per-part file (uuid.txt / txn_version.txt / metadata_version.txt) is NOT
    /// content-addressed: it is stored inline in this part's per-ref sidecar so two parts with
    /// identical content keep their own mutable bytes. Capture the bytes in memory and record them
    /// for the sidecar at commit — no blob is uploaded (so no orphan blob is created).
    if (ContentAddressed::isMutablePerPartFile(file))
    {
        return std::make_unique<ContentAddressed::ContentAddressedInlineWriteBuffer>(
            [this, file](std::string bytes) { recorded_mutable[file] = std::move(bytes); });
    }

    /// The blob is keyed under the same common key prefix as the read path (key_prefix, taken from
    /// the metadata storage). The write buffer spills the part file to a real server-local scratch
    /// dir while hashing it, then uploads to blobs/<hash>. The scratch dir is a local filesystem
    /// path, NOT the object-storage key prefix: for a remote object storage (e.g. s3) that prefix is
    /// a remote key prefix and is not a usable local path.
    return std::make_unique<ContentAddressed::ContentAddressedWriteBuffer>(
        metadata_storage.object_storage,
        key_prefix,
        local_scratch_path,
        metadata_storage.gcLock(),
        metadata_storage.inFlightPinnedBlobs(),
        [this, file](const ContentAddressed::BlobHash & blob_hash, size_t size)
        {
            recorded[file] = ContentAddressed::BlobEntry{blob_hash, size, blob_hash.string()};
            /// B52: track the pinned blob key so it is released when the ref is published (commit) or
            /// when an uncommitted transaction is destroyed. The write buffer pinned it under the GC lock.
            pinned_blob_keys.insert(ContentAddressed::blobKey(key_prefix, blob_hash).string());
        },
        /// M8: the cross-mounter pin. Invoked from finalizeImpl under the GC lock, BEFORE the blob is
        /// uploaded — record the hash in this transaction's WriteSession and (re)persist it so a sweep
        /// on another mounter treats the hash as reachable before it ever exists in the bucket.
        [this](const ContentAddressed::BlobHash & blob_hash) { recordBlobInSession(blob_hash); });
}

void ContentAddressedTransaction::createHardLink(const std::string & from, const std::string & to)
{
    rememberTarget(to);

    auto dst = ContentAddressed::parsePartFilePath(to);
    chassert(dst && !dst->file.empty());

    /// A MUTABLE per-part file carries forward by VALUE (its private bytes), not by blob reference:
    /// prefer the in-flight inline bytes from this commit, else read the committed source part's
    /// sidecar. It never enters the manifest, so it must not go through recordBlob. The mutable
    /// decision is made on the SOURCE file's basename: clone paths (e.g. DETACH into detached/) hand a
    /// destination whose parsed "file" is a nested <part>/<file>, so only the source parses cleanly to
    /// the logical file name. The carried bytes are keyed by the FULL destination part-relative path
    /// (dst->file), exactly as the content blobs are keyed via recordBlob (recorded[dst->file]). For an
    /// ordinary part dst->file is the bare logical name (e.g. metadata_version.txt); for a DETACH clone
    /// into detached/<part>/ it is <part>/metadata_version.txt — the SAME <part>/ prefix the manifest
    /// blobs carry, so the detached part's mutable sidecar lives under the same key prefix as its blobs.
    /// This is what the ATTACH rekey/republish chain (rekeyDetachedPartDir,
    /// republishDetachedStagingIntoActive) expects: it re-keys the staging <part>/ prefix off the sidecar
    /// entries to publish the active part. Keying by the bare basename here dropped that prefix, so the
    /// staging-prefix filter never matched and the active part lost its metadata_version.txt entirely —
    /// the part then fell back to the table's CURRENT metadata version and skipped the on-fly RENAME
    /// COLUMN conversion, surfacing as "no column c0" on SELECT after a detach/rename/attach (B62).
    if (auto src = ContentAddressed::parsePartFilePath(from); src && !src->file.empty()
        && ContentAddressed::isMutablePerPartFile(src->file))
    {
        if (src->table_uuid == table_uuid && src->part_name == part_name)
        {
            if (auto it = recorded_mutable.find(src->file); it != recorded_mutable.end())
            {
                recorded_mutable[dst->file] = it->second;
                return;
            }
        }
        recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
        return;
    }

    /// Resolve the source blob: prefer an in-flight blob recorded earlier in this same commit,
    /// else fall back to the committed source part via the metadata storage.
    if (auto src = ContentAddressed::parsePartFilePath(from); src && !src->file.empty())
    {
        if (src->table_uuid == table_uuid && src->part_name == part_name)
        {
            if (auto it = recorded.find(src->file); it != recorded.end())
            {
                recordBlob(to, it->second);
                return;
            }
        }
    }

    recordBlob(to, metadata_storage.resolveBlobEntry(from));
}

void ContentAddressedTransaction::setLastModified(const std::string &, const Poco::Timestamp &)
{
    /// No-op: per-file timestamps are derived, not stored. They play no role in CA resolution
    /// (ref -> part_id -> manifest -> blob). Reached via DiskObjectStorageTransaction.cpp:459.
}

void ContentAddressedTransaction::chmod(const String &, mode_t)
{
    /// No-op: permission bits are not stored for content-addressed objects.
}

void ContentAddressedTransaction::setReadOnly(const std::string &)
{
    /// No-op: content-addressed parts are immutable by construction; a read-only flag is implicit.
}

void ContentAddressedTransaction::republishCommittedPartIntoDetached(
    const ContentAddressed::PartFilePath & src_committed, const ContentAddressed::PartFilePath & dst_detached)
{
    /// dst_detached->file is the detached part directory name (e.g. all_1_2_1); re-key the source
    /// part's files under it so detached/<detached_part>/<file> resolves and enumeration lists it.
    const std::string & detached_dir = dst_detached.file;

    auto src_pid = metadata_storage.readRefPartId(src_committed.table_uuid, src_committed.part_name);
    if (!src_pid)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveDirectory of {}/{} into detached, but the source ref is absent",
            src_committed.table_uuid, src_committed.part_name);

    auto src_manifest = metadata_storage.loadPartManifestOrThrow(*src_pid);

    /// Merge into the existing detached ref (if any) so several detached parts can coexist under the
    /// one "detached" ref, each under its own <detached_part>/ key prefix. Re-key the source part's
    /// content blobs under the detached part dir component; the blob entries (hash/size) are unchanged
    /// — the same shared content is referenced — only the logical file name changes.
    ContentAddressed::PartManifest detached_manifest;
    if (auto existing_pid = metadata_storage.readRefPartId(dst_detached.table_uuid, dst_detached.part_name))
        detached_manifest = metadata_storage.loadPartManifestOrThrow(*existing_pid);
    for (const auto & [file, entry] : src_manifest.blobs)
        detached_manifest.blobs[detached_dir + "/" + file] = entry;

    const ContentAddressed::PartId detached_pid = ContentAddressed::computePartId(detached_manifest.blobs);

    const std::string detached_part_key = ContentAddressed::partKey(key_prefix, detached_pid).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(detached_part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = detached_manifest.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(detached_part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }

    /// Re-key the mutable per-part files (the per-ref sidecar) under the detached part dir too, so the
    /// detached part keeps its private uuid/txn/metadata_version bytes. Merge into the existing detached
    /// sidecar so several detached parts coexist.
    if (auto src_sidecar = metadata_storage.readRefSidecarIfExists(src_committed.table_uuid, src_committed.part_name))
    {
        ContentAddressed::RefSidecar detached_sidecar;
        if (auto existing = metadata_storage.readRefSidecarIfExists(dst_detached.table_uuid, dst_detached.part_name))
            detached_sidecar = *existing;
        for (const auto & [file, bytes] : src_sidecar->files)
        {
            const std::string detached_file = detached_dir + "/" + file;
            detached_sidecar.files[detached_file] = bytes;

            const std::string file_key = ContentAddressed::refMutableFileKey(
                key_prefix, metadata_storage.server_id, dst_detached.table_uuid, dst_detached.part_name, detached_file).string();
            auto file_out = metadata_storage.object_storage->writeObject(StoredObject(file_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }

        const std::string meta_key = ContentAddressed::refMetaKey(
            key_prefix, metadata_storage.server_id, dst_detached.table_uuid, dst_detached.part_name).string();
        const std::string meta_bytes = detached_sidecar.serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }

    /// Publish the detached ref last (the commit point), then unlink the source ref + its sidecars so
    /// the part disappears from the active set and appears under detached.
    const std::string detached_ref_key = ContentAddressed::refKey(
        key_prefix, metadata_storage.server_id, dst_detached.table_uuid, dst_detached.part_name).string();
    const std::string ref_payload = ContentAddressed::serializeRefPayload(detached_pid);
    auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(detached_ref_key), WriteMode::Rewrite);
    ref_out->write(ref_payload.data(), ref_payload.size());
    ref_out->finalize();

    /// Unlink the source ref and any of its per-ref sidecar objects (ref-scoped, never the shared
    /// blobs/manifest). List under refsPrefix and remove the source ref + its <part>.* sidecars.
    const std::string src_refs_prefix = ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, src_committed.table_uuid);
    RelativePathsWithMetadata refs;
    metadata_storage.object_storage->listObjects(src_refs_prefix, refs, 0);
    const std::string src_ref = src_refs_prefix + src_committed.part_name;
    const std::string src_ref_sidecar_prefix = src_ref + ".";
    for (const auto & elem : refs)
    {
        const auto & rp = elem->relative_path;
        if (rp == src_ref || rp.starts_with(src_ref_sidecar_prefix))
            metadata_storage.object_storage->removeObjectIfExists(StoredObject(rp));
    }
}

void ContentAddressedTransaction::rekeyDetachedPartDir(
    const std::string & table_id, const std::string & old_dir, const std::string & new_dir)
{
    if (old_dir == new_dir)
        return;

    /// The detached parts share the one "detached" ref whose manifest/sidecar keys are
    /// <detached_part>/<file> (B36/B46). Re-key only this part's keys (prefix old_dir + "/") to
    /// new_dir + "/" in the shared manifest and sidecar bundle, rewrite the per-file sidecar objects
    /// under their new key, and re-publish the trimmed/rewritten ref. Other detached parts are
    /// untouched. The content blobs are content-addressed and never move.
    const std::string detached = std::string(ContentAddressed::kDetachedDirName);
    auto existing_pid = metadata_storage.readRefPartId(table_id, detached);
    if (!existing_pid)
        return; /// no detached ref => nothing to rename (a no-op rename of an absent part)

    const std::string old_pfx = old_dir + "/";
    const std::string new_pfx = new_dir + "/";

    auto manifest = metadata_storage.loadPartManifestOrThrow(*existing_pid);
    ContentAddressed::PartManifest rekeyed;
    for (const auto & [file, entry] : manifest.blobs)
    {
        if (file.rfind(old_pfx, 0) == 0)
            rekeyed.blobs[new_pfx + file.substr(old_pfx.size())] = entry;
        else
            rekeyed.blobs[file] = entry;
    }

    ContentAddressed::RefSidecar sidecar;
    if (auto existing_sidecar = metadata_storage.readRefSidecarIfExists(table_id, detached))
        sidecar = *existing_sidecar;
    ContentAddressed::RefSidecar rekeyed_sidecar;
    StoredObjects old_file_objects;
    for (const auto & [file, bytes] : sidecar.files)
    {
        std::string new_file = file;
        if (file.rfind(old_pfx, 0) == 0)
        {
            new_file = new_pfx + file.substr(old_pfx.size());
            /// Move the per-file sidecar object to its new key (delete the old one below).
            old_file_objects.emplace_back(
                ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_id, detached, file).string());
            const std::string new_key
                = ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_id, detached, new_file).string();
            auto file_out = metadata_storage.object_storage->writeObject(StoredObject(new_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }
        rekeyed_sidecar.files[new_file] = bytes;
    }

    /// Republish the manifest, sidecar bundle and ref (then drop the stale per-file objects).
    const ContentAddressed::PartId new_pid = ContentAddressed::computePartId(rekeyed.blobs);
    const std::string new_part_key = ContentAddressed::partKey(key_prefix, new_pid).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(new_part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = rekeyed.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(new_part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }
    {
        const std::string meta_key = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_id, detached).string();
        const std::string meta_bytes = rekeyed_sidecar.serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }
    {
        const std::string ref_key = ContentAddressed::refKey(key_prefix, metadata_storage.server_id, table_id, detached).string();
        const std::string ref_payload = ContentAddressed::serializeRefPayload(new_pid);
        auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
        ref_out->write(ref_payload.data(), ref_payload.size());
        ref_out->finalize();
    }
    if (!old_file_objects.empty())
        metadata_storage.object_storage->removeObjectsIfExist(old_file_objects);
}

void ContentAddressedTransaction::republishDetachedStagingIntoActive(
    const ContentAddressed::PartFilePath & src_staging, const ContentAddressed::PartFilePath & dst_active)
{
    /// The staging directory name under the shared "detached" ref (e.g. attaching_all_2_2_0); its keys
    /// are <staging>/<inner> (B36/B46). The active part takes the BARE <inner> names.
    const std::string & staging_dir = src_staging.file;
    const std::string staging_pfx = staging_dir + "/";
    const std::string detached = std::string(ContentAddressed::kDetachedDirName);

    auto detached_pid = metadata_storage.readRefPartId(src_staging.table_uuid, detached);
    if (!detached_pid)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveDirectory of detached/{} into active {}/{}, but the detached ref is absent",
            staging_dir, dst_active.table_uuid, dst_active.part_name);

    auto detached_manifest = metadata_storage.loadPartManifestOrThrow(*detached_pid);

    /// Build the active manifest from the staging dir's entries re-keyed to bare <inner>. The blob
    /// entries (hash/size) are unchanged — the same shared content is referenced — only the logical file
    /// name loses its <staging>/ prefix.
    ContentAddressed::PartManifest active_manifest;
    for (const auto & [file, entry] : detached_manifest.blobs)
    {
        if (file.rfind(staging_pfx, 0) == 0)
            active_manifest.blobs[file.substr(staging_pfx.size())] = entry;
    }
    if (active_manifest.blobs.empty())
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveDirectory of detached/{} into active {}/{}, but the detached ref has no "
            "entries under that staging directory",
            staging_dir, dst_active.table_uuid, dst_active.part_name);

    const ContentAddressed::PartId active_pid = ContentAddressed::computePartId(active_manifest.blobs);
    const std::string active_part_key = ContentAddressed::partKey(key_prefix, active_pid).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(active_part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = active_manifest.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(active_part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }

    /// Build the active part's per-ref sidecar from the detached sidecar's <staging>/<inner> entries
    /// re-keyed to bare <inner>, so the active part keeps its private uuid/txn/metadata_version bytes.
    if (auto detached_sidecar = metadata_storage.readRefSidecarIfExists(src_staging.table_uuid, detached))
    {
        ContentAddressed::RefSidecar active_sidecar;
        for (const auto & [file, bytes] : detached_sidecar->files)
        {
            if (file.rfind(staging_pfx, 0) != 0)
                continue;
            const std::string bare_file = file.substr(staging_pfx.size());
            active_sidecar.files[bare_file] = bytes;

            const std::string file_key = ContentAddressed::refMutableFileKey(
                key_prefix, metadata_storage.server_id, dst_active.table_uuid, dst_active.part_name, bare_file).string();
            auto file_out = metadata_storage.object_storage->writeObject(StoredObject(file_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }

        if (!active_sidecar.files.empty())
        {
            const std::string meta_key = ContentAddressed::refMetaKey(
                key_prefix, metadata_storage.server_id, dst_active.table_uuid, dst_active.part_name).string();
            const std::string meta_bytes = active_sidecar.serialize();
            auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
            meta_out->write(meta_bytes.data(), meta_bytes.size());
            meta_out->finalize();
        }
    }

    /// Publish the active ref (the commit point), then strip the staging keys from the detached ref.
    const std::string active_ref_key = ContentAddressed::refKey(
        key_prefix, metadata_storage.server_id, dst_active.table_uuid, dst_active.part_name).string();
    const std::string active_ref_payload = ContentAddressed::serializeRefPayload(active_pid);
    auto active_ref_out = metadata_storage.object_storage->writeObject(StoredObject(active_ref_key), WriteMode::Rewrite);
    active_ref_out->write(active_ref_payload.data(), active_ref_payload.size());
    active_ref_out->finalize();

    /// Remove the <staging>/ keys from the shared detached ref (manifest + sidecar bundle + per-file
    /// sidecar objects), leaving any other detached parts intact. This mirrors removeRecursive's
    /// detached-part removal: rewrite the trimmed detached ref under a new part id, or unlink the ref +
    /// bundle when no detached parts remain.
    std::erase_if(detached_manifest.blobs, [&](const auto & kv) { return kv.first.rfind(staging_pfx, 0) == 0; });

    ContentAddressed::RefSidecar detached_sidecar;
    if (auto existing_sidecar = metadata_storage.readRefSidecarIfExists(src_staging.table_uuid, detached))
        detached_sidecar = *existing_sidecar;
    StoredObjects mutable_to_remove;
    for (auto it = detached_sidecar.files.begin(); it != detached_sidecar.files.end();)
    {
        if (it->first.rfind(staging_pfx, 0) == 0)
        {
            mutable_to_remove.emplace_back(
                ContentAddressed::refMutableFileKey(
                    key_prefix, metadata_storage.server_id, src_staging.table_uuid, detached, it->first).string());
            it = detached_sidecar.files.erase(it);
        }
        else
            ++it;
    }
    if (!mutable_to_remove.empty())
        metadata_storage.object_storage->removeObjectsIfExist(mutable_to_remove);

    const std::string detached_ref_key
        = ContentAddressed::refKey(key_prefix, metadata_storage.server_id, src_staging.table_uuid, detached).string();
    const std::string detached_meta_key
        = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, src_staging.table_uuid, detached).string();

    if (detached_manifest.blobs.empty())
    {
        /// No detached parts remain: unlink the shared detached ref and its bundle sidecar.
        metadata_storage.object_storage->removeObjectsIfExist({StoredObject(detached_ref_key), StoredObject(detached_meta_key)});
        return;
    }

    /// Republish the trimmed detached manifest under its new part id, rewrite the bundle, re-point the ref.
    const ContentAddressed::PartId new_detached_pid = ContentAddressed::computePartId(detached_manifest.blobs);
    const std::string new_detached_part_key = ContentAddressed::partKey(key_prefix, new_detached_pid).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(new_detached_part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = detached_manifest.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(new_detached_part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }
    {
        const std::string meta_bytes = detached_sidecar.serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(detached_meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }
    {
        const std::string ref_payload = ContentAddressed::serializeRefPayload(new_detached_pid);
        auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(detached_ref_key), WriteMode::Rewrite);
        ref_out->write(ref_payload.data(), ref_payload.size());
        ref_out->finalize();
    }
}

void ContentAddressedTransaction::republishTableRefs(const std::string & src_table_id, const std::string & dst_table_id)
{
    if (src_table_id == dst_table_id)
        return;

    /// Re-key every object under a source per-(server,table) prefix to the destination prefix by
    /// copying the small pointer object verbatim and unlinking the source. Refs, per-ref sidecars and
    /// verbatim table-level files are all tiny pointer objects, so a read+write+delete is correct (the
    /// content blobs/manifests they point at are content-addressed and shared — never copied/moved).
    auto rekey_prefix = [this](const std::string & src_prefix, const std::string & dst_prefix)
    {
        RelativePathsWithMetadata children;
        metadata_storage.object_storage->listObjects(src_prefix, children, /*max_keys=*/0);

        StoredObjects to_remove;
        for (const auto & child : children)
        {
            const std::string & src_key = child->relative_path;
            const auto pos = src_key.rfind(src_prefix);
            if (pos == std::string::npos)
                continue;
            const std::string tail = src_key.substr(pos + src_prefix.size());
            const std::string dst_key = dst_prefix + tail;

            auto bytes = metadata_storage.readSmallObjectIfExists(src_key);
            if (!bytes)
                continue; /// raced away; nothing to move

            auto out = metadata_storage.object_storage->writeObject(StoredObject(dst_key), WriteMode::Rewrite);
            out->write(bytes->data(), bytes->size());
            out->finalize();

            to_remove.emplace_back(src_key);
        }

        if (!to_remove.empty())
            metadata_storage.object_storage->removeObjectsIfExist(to_remove);
    };

    rekey_prefix(
        ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, src_table_id),
        ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, dst_table_id));
    rekey_prefix(
        ContentAddressed::tableFilesPrefix(key_prefix, metadata_storage.server_id, src_table_id),
        ContentAddressed::tableFilesPrefix(key_prefix, metadata_storage.server_id, dst_table_id));
}

bool ContentAddressedTransaction::rekeyStagedProjectionDir(
    const ContentAddressed::PartFilePath & src, const ContentAddressed::PartFilePath & dst)
{
    /// Only meaningful for the part this transaction is currently assembling: the projection rename
    /// happens after the projection files have been written into this transaction's staged maps, so the
    /// target must already be pinned and must match. If nothing is staged or it is a different part, this
    /// is not the staged-projection-rename case — let moveDirectory fall through to its other branches.
    if (table_uuid.empty() || part_name.empty()
        || src.table_uuid != table_uuid || src.part_name != part_name)
        return false;

    /// Re-key every staged blob and inline mutable file whose in-part name lives under the old projection
    /// directory (<src.file>/...) to the new one (<dst.file>/...). The blob/byte payloads are unchanged —
    /// only the logical key changes — so the manifest published at commit carries the final <proj>.proj/
    /// names. A whole-directory rename has nothing keyed at the bare <src.file> (projection files are
    /// always <proj>.proj/<inner>), so we match strictly on the "<src.file>/" prefix.
    const std::string src_prefix = src.file + "/";
    const std::string dst_prefix = dst.file + "/";

    auto rekey_map = [&](auto & m)
    {
        using MapT = std::decay_t<decltype(m)>;
        MapT moved;
        for (auto it = m.begin(); it != m.end();)
        {
            if (it->first.rfind(src_prefix, 0) == 0)
            {
                moved[dst_prefix + it->first.substr(src_prefix.size())] = std::move(it->second);
                it = m.erase(it);
            }
            else
                ++it;
        }
        for (auto & [k, v] : moved)
            m[k] = std::move(v);
    };

    rekey_map(recorded);
    rekey_map(recorded_mutable);
    return true;
}

void ContentAddressedTransaction::moveDirectory(const std::string & from, const std::string & to)
{
    /// DETACH PARTITION moves a COMMITTED part directory <uuid[:3]>/<uuid>/<part> into the detached
    /// namespace <uuid[:3]>/<uuid>/detached/<detached_part>. Content addressing has no rename, and the
    /// committed part is not staged in this transaction, so re-publish the ref: the detached ref is
    /// named "detached" and carries the source part's blob entries (and mutable sidecar files) re-keyed
    /// under the detached part directory component, so the read/enumeration path resolves
    /// detached/<detached_part>/<file> and system.detached_parts lists <detached_part> (B36). The shared
    /// content blobs and the source manifest are untouched (immutable, content-addressed); only the
    /// small ref/manifest/sidecar pointer objects are written, and the source ref is unlinked.
    if (auto src_committed = ContentAddressed::parsePartFilePath(from); src_committed && src_committed->file.empty())
    {
        if (auto dst_detached = ContentAddressed::parsePartFilePath(to);
            dst_detached && dst_detached->part_name == ContentAddressed::kDetachedDirName && !dst_detached->file.empty())
        {
            republishCommittedPartIntoDetached(*src_committed, *dst_detached);
            return;
        }
    }

    /// A DETACHED part rename WITHIN the detached namespace: detached/OLD -> detached/NEW. DROP DETACHED
    /// PARTITION renames the detached part to "deleting_OLD" before removing it (MergeTreeData::dropDetached
    /// -> PartsTemporaryRename), and ATTACH renames "attaching_OLD" -> OLD. Both detached parts share the
    /// one "detached" ref whose manifest/sidecar keys are PART/FILE (B36/B46), so this re-keys the OLD/
    /// key prefix to NEW/ in the shared detached manifest and sidecar (the per-file sidecar objects too),
    /// leaving the other detached parts intact. No content blobs move (content-addressed).
    if (auto src_detached = ContentAddressed::parsePartFilePath(from);
        src_detached && src_detached->part_name == ContentAddressed::kDetachedDirName && !src_detached->file.empty()
        && src_detached->file.find('/') == std::string::npos)
    {
        if (auto dst_detached = ContentAddressed::parsePartFilePath(to);
            dst_detached && dst_detached->part_name == ContentAddressed::kDetachedDirName && !dst_detached->file.empty()
            && dst_detached->file.find('/') == std::string::npos
            && src_detached->table_uuid == dst_detached->table_uuid)
        {
            rekeyDetachedPartDir(src_detached->table_uuid, src_detached->file, dst_detached->file);
            return;
        }
    }

    /// ATTACH PARTITION / ATTACH PART publishes an ACTIVE part out of a DETACHED staging directory.
    /// MergeTree stages the detached source as detached/attaching_<part> (a single component under the
    /// shared "detached" ref, B36/B46), then renames it to the final active part dir
    /// <uuid[:3]>/<uuid>/<active_part>. Content addressing has no rename and the staging dir is not a ref
    /// of its own, so re-publish: the inverse of republishCommittedPartIntoDetached. Fire only when `from`
    /// is detached/<staging> (part_name == "detached", file a SINGLE non-empty component) and `to` is an
    /// ACTIVE part dir (file empty, part_name != "detached"). Both guards are exclusive with the
    /// committed->detached, detached->detached and tmp->active branches above.
    if (auto src_staging = ContentAddressed::parsePartFilePath(from);
        src_staging && src_staging->part_name == ContentAddressed::kDetachedDirName && !src_staging->file.empty()
        && src_staging->file.find('/') == std::string::npos)
    {
        if (auto dst_active = ContentAddressed::parsePartFilePath(to);
            dst_active && dst_active->file.empty() && dst_active->part_name != ContentAddressed::kDetachedDirName)
        {
            republishDetachedStagingIntoActive(*src_staging, *dst_active);
            return;
        }
    }

    /// RENAME TABLE / cross-engine table move (MergeTreeData::rename) renames the whole table data
    /// directory: disk->moveDirectory(relative_data_path, new_table_path). Both endpoints are TABLE
    /// dirs (no part component). The refs/sidecars are keyed by the table identifier (the <uuid> for
    /// Atomic, the data/<db>/<table> path for non-Atomic), so the table identifier changes across the
    /// rename (e.g. Ordinary data/db/mt -> Atomic <uuid>) and the read at the new identity would find
    /// no ref (B40). Re-key every ref + sidecar under the source table id to the destination table id.
    /// The shared blobs/manifests are content-addressed and untouched; only the small ref/sidecar
    /// pointer objects are rewritten. This is a no-op when the source has no refs (an empty table).
    if (auto src_table = ContentAddressed::parseTableUuid(from))
    {
        if (auto dst_table = ContentAddressed::parseTableUuid(to))
        {
            republishTableRefs(*src_table, *dst_table);
            return;
        }
    }

    /// Projection MATERIALIZE / merge stages the projection part under a temporary subdirectory of the
    /// part being assembled, <part>/<proj>_<n>.tmp_proj, and renames it to the final <part>/<proj>.proj
    /// before the parent part commits (MergeProjectionPartsTask). Both endpoints are files (a projection
    /// subdir) WITHIN the same staged part: `from` ends in ".tmp_proj", `to` ends in ".proj", and the
    /// part component matches the (table_uuid, part_name) this transaction is assembling. Re-key the
    /// staged projection files in place so the parent part's manifest, published at commit, carries the
    /// final <proj>.proj/<inner> keys. (Without this the recorded keys stay <proj>_<n>.tmp_proj/<inner>
    /// and the read path, which resolves <proj>.proj/<inner>, finds them absent — FILE_DOESNT_EXIST.)
    if (auto src_proj = ContentAddressed::parsePartFilePath(from); src_proj && !src_proj->file.empty())
    {
        if (auto dst_proj = ContentAddressed::parsePartFilePath(to);
            dst_proj && !dst_proj->file.empty()
            && src_proj->table_uuid == dst_proj->table_uuid && src_proj->part_name == dst_proj->part_name
            && src_proj->file.ends_with(".tmp_proj") && dst_proj->file.ends_with(".proj")
            && rekeyStagedProjectionDir(*src_proj, *dst_proj))
            return;
    }

    /// MergeTree assembles a part under a temporary directory (e.g. tmp_insert_<part>) and renames
    /// it to the final <part> at commit. Re-pin the (table_uuid, part_name) target to the
    /// destination so the ref is published under the final part name. The recorded blobs are keyed
    /// by their in-part file name only and need not change; the content is already addressed, so no
    /// objects are moved in storage.
    auto dst_part = ContentAddressed::parsePartFilePath(to);

    /// Only the part-directory rename (<uuid[:3]>/<uuid>/<part>) is meaningful for CA. A part
    /// directory has no file component (file is empty). Anything else (e.g. a deeper move) is not
    /// reached for an INSERT and would indicate an unexpected path shape.
    if (!dst_part || !dst_part->file.empty())
        return;

    /// Nothing staged yet. Two distinct cases share this shape:
    ///   (a) a rename of a COMMITTED part (MergeTree renames a part to delete_tmp_<part> before
    ///       removing it, and renames merged/mutated parts) — the source has a live ref that must be
    ///       re-keyed to the destination, or the source ref survives and the part is rediscovered on
    ///       the next ATTACH (B45);
    ///   (b) a directory-only move before any file write (an INSERT's tmp_insert_<part> whose files are
    ///       written into this transaction AFTER the rename) — the source has no committed ref, so just
    ///       adopt the destination as the staged target.
    /// renameCommittedPartRef distinguishes them: it returns true iff a committed source ref existed.
    if (table_uuid.empty() && part_name.empty())
    {
        if (auto src_part = ContentAddressed::parsePartFilePath(from);
            src_part && src_part->file.empty() && renameCommittedPartRef(*src_part, *dst_part))
            return;

        table_uuid = dst_part->table_uuid;
        part_name = dst_part->part_name;
        return;
    }

    /// The source must be the part we have been assembling.
    auto src_part = ContentAddressed::parsePartFilePath(from);
    if (src_part && src_part->file.empty()
        && src_part->table_uuid == table_uuid && src_part->part_name == part_name)
    {
        /// A projection MATERIALIZE / merge writes the projection part into a SEPARATE child storage of
        /// the staged part and COMMITS that child's own transaction early (MutateTask /
        /// MergeProjectionPartsTask). On a content-addressed pool that early commit publishes a standalone
        /// ref under the staged part name (e.g. tmp_mut_<part>), carrying the projection's files. After the
        /// projection dir is renamed into the parent manifest (rekeyStagedProjectionDir above) the parent
        /// part is renamed away from tmp_mut_<part>, leaving that standalone ref orphaned — and
        /// clearOldTemporaryDirectories then trips over it on the next startup/ATTACH (it lists
        /// tmp_mut_<part> as a live directory). Unlink the source part's ref + sidecars here so the
        /// rename leaves nothing behind. For a plain INSERT (tmp_insert_<part> with no early sub-commit)
        /// there is no such ref, so this is a no-op.
        unlinkPartDirRefs(from);

        table_uuid = dst_part->table_uuid;
        part_name = dst_part->part_name;
    }
    else
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: moveDirectory from {} to {} does not match the staged part {}/{}",
            from, to, table_uuid, part_name);
    }
}

void ContentAddressedTransaction::moveFile(const std::string & from, const std::string & to)
{
    /// A table-level / generic file (e.g. the mutation entry `mutation_N.txt`, renamed from
    /// `tmp_mutation_N.txt` by `MergeTreeMutationEntry::commit`) is stored verbatim under a key derived
    /// from its PATH (`writeFile`'s non-part branch: `tableFileKey` / `diskFileKey`). It is not
    /// content-addressed and not tracked by this transaction's commit, and the source object is already
    /// durable (the write buffer finalized before the rename). A rename therefore physically moves the
    /// verbatim object from the source key to the destination key, mirroring a filesystem rename so the
    /// read path (which recomputes the key from the path) finds it under the new name.
    if (!ContentAddressed::isPartFilePath(from) && !ContentAddressed::isPartFilePath(to))
    {
        auto verbatimKey = [&](const std::string & path) -> std::string
        {
            if (auto tf = ContentAddressed::parseTableFilePath(path))
                return ContentAddressed::tableFileKey(key_prefix, metadata_storage.server_id, tf->table_uuid, tf->tail);
            return ContentAddressed::diskFileKey(key_prefix, path);
        };

        const std::string from_key = verbatimKey(from);
        const std::string to_key = verbatimKey(to);
        if (from_key == to_key)
            return;

        const StoredObject from_obj(from_key, from);
        const StoredObject to_obj(to_key, to);
        metadata_storage.object_storage->copyObject(from_obj, to_obj, ReadSettings{}, WriteSettings{});
        metadata_storage.object_storage->removeObjectIfExists(from_obj);
        return;
    }

    /// Rename of a single in-part file: re-key the recorded blob. No object is moved in storage.
    auto src = ContentAddressed::parsePartFilePath(from);
    auto dst = ContentAddressed::parsePartFilePath(to);
    if (!src || src->file.empty() || !dst || dst->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile requires two part-file paths: {} -> {}", from, to);

    rememberTarget(to);

    /// A mutable per-part file is staged inline (recorded_mutable), not as a blob; re-key it there.
    if (auto mit = recorded_mutable.find(src->file); mit != recorded_mutable.end())
    {
        auto bytes = std::move(mit->second);
        recorded_mutable.erase(mit);
        recorded_mutable[dst->file] = std::move(bytes);
        return;
    }

    auto it = recorded.find(src->file);
    if (it == recorded.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile source not recorded: {}", from);

    auto entry = it->second;
    recorded.erase(it);
    recorded[dst->file] = std::move(entry);
}

std::string ContentAddressedTransaction::readStagedOrCommittedBytes(const std::string & path) const
{
    /// Read the bytes of a part file written in THIS transaction as a content blob (just uploaded), so a
    /// rename whose destination is mutable can re-stage them inline. Falls back to the committed sidecar.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->file.empty())
    {
        if (auto it = recorded.find(p->file); it != recorded.end())
        {
            StoredObject obj(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size);
            auto in = metadata_storage.object_storage->readObject(obj, ReadSettings{}, std::nullopt);
            std::string bytes;
            WriteBufferFromString out(bytes);
            copyData(*in, out);
            out.finalize();
            return bytes;
        }
    }
    return metadata_storage.resolveMutableFileBytes(path);
}

void ContentAddressedTransaction::replaceFile(const std::string & from, const std::string & to)
{
    /// replaceFile = moveFile that overwrites the destination. A content-addressed rename of an in-part
    /// file re-keys the staged entry (no object moves). The txn_version.txt destination is a mutable
    /// per-part file, so the result must land in recorded_mutable (the per-ref sidecar), never the manifest.
    auto src = ContentAddressed::parsePartFilePath(from);
    auto dst = ContentAddressed::parsePartFilePath(to);
    if (!src || src->file.empty() || !dst || dst->file.empty())
    {
        /// Table-level / generic verbatim path: delegate to moveFile (its non-part branch does a
        /// copyObject(Rewrite)+remove, which already overwrites the destination).
        moveFile(from, to);
        return;
    }
    rememberTarget(to);
    /// Overwrite: drop any staged destination entry first.
    recorded.erase(dst->file);
    recorded_mutable.erase(dst->file);
    recorded_mutable_removed.erase(dst->file);
    /// Common case — source staged inline (txn_version.txt.tmp recognized as mutable by Piece 1): move bytes.
    if (auto mit = recorded_mutable.find(src->file); mit != recorded_mutable.end())
    {
        recorded_mutable[dst->file] = std::move(mit->second);
        recorded_mutable.erase(mit);
        return;
    }
    /// Source staged as a content blob but destination is mutable: should be rare after Piece 1. Read the
    /// just-written bytes back inline (the orphan tmp blob is GC-reclaimed). Otherwise re-key the blob.
    if (auto it = recorded.find(src->file); it != recorded.end())
    {
        if (ContentAddressed::isMutablePerPartFile(dst->file))
        {
            recorded_mutable[dst->file] = readStagedOrCommittedBytes(from);
            recorded.erase(it);
        }
        else
        {
            recorded[dst->file] = std::move(it->second);
            recorded.erase(it);
        }
        return;
    }
    /// Source not staged in THIS transaction (standalone autocommit across ops): resolve the committed
    /// source bytes from the sidecar and re-stage under the destination, marking the source removed.
    recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
    recorded_mutable_removed.insert(src->file);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool, bool)
{
    /// Part file: drop the staged blob so it is excluded from the manifest. The underlying content
    /// blob, if shared, is reclaimed by GC, not here.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->file.empty())
    {
        const bool staged_here = recorded.contains(p->file) || recorded_mutable.contains(p->file);
        recorded.erase(p->file);
        recorded_mutable.erase(p->file);
        /// A mutable per-part file that exists only in the already-committed sidecar (not staged in THIS
        /// transaction) must be deleted from that sidecar at commit — the mutable-only commit branch reads
        /// recorded_mutable_removed to do so. This handles removeTmpMetadataFile's
        /// removeFile(txn_version.txt.tmp) on a committed part (the .tmp is recognized as mutable by Piece 1).
        if (!staged_here && ContentAddressed::isMutablePerPartFile(p->file))
            recorded_mutable_removed.insert(p->file);
        return;
    }

    /// Table-level file (e.g. format_version.txt, a `mutation_N.txt` entry): remove its verbatim
    /// object. These are stored verbatim under tableFileKey and are not content-addressed, so a
    /// mid-life delete (e.g. cleanup of a stale `tmp_mutation_*.txt`, or a pruned finished mutation
    /// entry) must reclaim the object now — the reachability sweep only scans blobs/+parts/, so a
    /// no-op here would leak the object until DROP (B50). removeObjectIfExists is a no-op when absent.
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        metadata_storage.object_storage->removeObjectIfExists(StoredObject(
            ContentAddressed::tableFileKey(key_prefix, metadata_storage.server_id, tf->table_uuid, tf->tail), path));
        return;
    }

    /// Generic disk-level file (e.g. the server's startup access-check probe): delete its verbatim
    /// object so a write -> read -> unlink round-trip leaves nothing behind. removeObjectIfExists is
    /// a no-op when the object is absent, mirroring an unlink of a never-written generic file.
    metadata_storage.object_storage->removeObjectIfExists(
        StoredObject(ContentAddressed::diskFileKey(key_prefix, path), path));
}

void ContentAddressedTransaction::truncateFile(const std::string &, size_t)
{
    /// No-op: content-addressed blobs are immutable; committed part files are never truncated.
}

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant & options)
{
    if (!std::holds_alternative<NoCommitOptions>(options))
        throwNotImplemented();

    /// Nothing was staged (e.g. a directory-only transaction): no part to publish.
    if (recorded.empty() && recorded_mutable.empty() && recorded_mutable_removed.empty())
        return;

    if (table_uuid.empty() || part_name.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: commit without a resolved part target");

    /// Mutable-only update of an ALREADY-COMMITTED part: this transaction staged NO content blobs, only
    /// mutable per-part files (txn_version.txt and/or removals) — the MVCC creation-CSN fill-in and
    /// removal-TID lock/unlock rewrite txn_version.txt on a LIVE part. Update only the per-ref sidecar +
    /// the per-file mutable objects in place; KEEP the existing manifest, part_id and ref. The normal path
    /// below would compute a part_id over an empty manifest and republish the ref — clobbering the part.
    if (recorded.empty() && (!recorded_mutable.empty() || !recorded_mutable_removed.empty()))
    {
        auto existing_pid = metadata_storage.readRefPartId(table_uuid, part_name);
        if (!existing_pid)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "ContentAddressed: mutable-only commit for {}/{} with no existing ref", table_uuid, part_name);

        std::lock_guard<std::mutex> gc_guard(*metadata_storage.gc_lock);

        ContentAddressed::RefSidecar sidecar;
        if (auto existing = metadata_storage.readRefSidecarIfExists(table_uuid, part_name))
            sidecar = *existing;
        for (const auto & f : recorded_mutable_removed)
        {
            sidecar.files.erase(f);
            metadata_storage.object_storage->removeObjectIfExists(StoredObject(
                ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, f).string()));
        }
        for (const auto & [file, bytes] : recorded_mutable)
        {
            sidecar.files[file] = bytes;
            const std::string fk = ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, file).string();
            auto out = metadata_storage.object_storage->writeObject(StoredObject(fk), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size());
            out->finalize();
        }
        const std::string meta_key = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();
        const std::string meta_bytes = sidecar.serialize();
        auto mo = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        mo->write(meta_bytes.data(), meta_bytes.size());
        mo->finalize();

        if (session_open)
            releaseSession();
        return;
    }

    /// Partition: content-identical files go to the shared manifest (and dedup); the mutable per-part
    /// files (recorded_mutable) go to this part's PRIVATE per-ref sidecar, never the manifest. The
    /// manifest holds only content state, so two parts with identical content share one manifest while
    /// each keeps its own uuid / txn / metadata version (B23).
    /// NOTE: frozen targets (FREEZE, shadow/<backup>/…) are always a real part_name (never "detached"),
    /// so the two kDetachedDirName manifest-merge + sidecar-merge branches below are skipped for them.
    ContentAddressed::PartManifest manifest;
    manifest.blobs = recorded;

    /// B49: take the per-pool in-process GC lock for the publish, and re-validate every referenced blob
    /// UNDER it before writing the manifest or publishing the ref. finalizeImpl skips re-uploading a
    /// blob that already exists (dedup); in the window between that skip and this publish a concurrent
    /// background sweep could reclaim such a blob (it was unreferenced + past grace), leaving the ref we
    /// are about to publish dangling -> data loss on read. The sweep holds this SAME lock for its whole
    /// mark+delete, so once we hold it the live set is stable: re-HEAD every blob the manifest names and,
    /// if any is missing, FAIL CLOSED (a retryable error) WITHOUT publishing a dangling ref. The insert
    /// is then retried and re-uploads the blob. With mutual exclusion the only outcomes are: we publish
    /// first (the sweep's next pass sees the blob reachable and keeps it) or the sweep reclaimed it first
    /// (we throw and retry) — never a published ref to a deleted blob.
    /// Renew the write-session pin so it is FRESHLY live across the ref publish below (cross-process
    /// data-loss fix): the per-blob renewals kept it live during the write, but the session must not be
    /// allowed to lapse before the ref is published. A live session root covers every freshly-written
    /// blob this part is about to reference; a REMOTE mounter's sweep re-reads sessions in its
    /// re-validate-under-lock step immediately before deleting, so a live session makes it skip these
    /// blobs. (Carried-forward blobs are instead covered by their source part's ref.) The in-process
    /// re-HEAD below is the same-process backstop (B49). Renew before taking the in-process lock since
    /// this is a bucket write coordinating with OTHER mounters, not the local sweep.
    if (session_open)
        persistSession();

    std::lock_guard<std::mutex> gc_guard(*metadata_storage.gc_lock);

    /// The "detached" ref is a SHARED container of detached part directories: each detach/FETCH lands one
    /// part into detached/<detached_part>/ via its own transaction whose recorded keys are
    /// <detached_part>/<file> (B36). Several parts land independently and must COEXIST under the one
    /// "detached" ref. The default publish below rewrites the ref, which would make each publish overwrite
    /// the previous one (only the last detached part would be listed — B46). So when the target is the
    /// detached namespace, merge the new keys into the existing detached ref's manifest first. This
    /// read-modify-write of the SHARED detached ref MUST happen UNDER the per-pool gc_lock: a FETCH
    /// PARTITION downloads its parts concurrently (the FETCH thread pool, 03350) and each part's commit
    /// publishes into the same "detached" ref, so an unlocked read-merge-publish loses entries — two
    /// concurrent commits both read the same prior manifest and the second overwrites the first's
    /// contribution, leaving only one part's blobs and a FILE_DOESNT_EXIST on the lost parts (B66). The
    /// lock serializes the read-merge-publish so every part accumulates. (A regular part_name is never
    /// re-committed with new content under the same name, so this only affects "detached".)
    if (part_name == ContentAddressed::kDetachedDirName)
    {
        if (auto existing_pid = metadata_storage.readRefPartId(table_uuid, part_name))
        {
            auto existing = metadata_storage.loadPartManifestOrThrow(*existing_pid);
            for (const auto & [file, entry] : existing.blobs)
                manifest.blobs.emplace(file, entry); /// keep the new entry on a key collision (re-land)
        }
    }

    const ContentAddressed::PartId part_id = ContentAddressed::computePartId(manifest.blobs);

    for (const auto & [file, entry] : manifest.blobs)
    {
        const std::string blob_key = ContentAddressed::blobKey(key_prefix, entry.key).string();
        if (!metadata_storage.object_storage->tryGetObjectMetadata(blob_key, /*with_tags=*/false).has_value())
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ContentAddressed: blob {} (file {}) referenced by part {}/{} was concurrently reclaimed "
                "by GC before the ref could be published; retry the insert",
                blob_key, file, table_uuid, part_name);
    }

    /// Put-if-absent the manifest: identical parts (same deterministic blobs) share one manifest object.
    const std::string part_key = ContentAddressed::partKey(key_prefix, part_id).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = manifest.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }

    /// Write the per-ref sidecar BEFORE the ref (the ref is the last, publishing step). A crashed
    /// write that gets this far but never publishes the ref leaves a sidecar whose ref is absent; that
    /// sidecar lives under the same refs/ prefix as the ref, so removeRecursive's ref-scoped deletion
    /// reclaims it on the next DROP, and it is never a content-addressed object the reachability sweep
    /// could miss (the sweep scans only blobs/+parts/). So sidecars cannot leak (B23 orphan concern).
    /// For the shared "detached" ref the bundle sidecar must MERGE with any prior detached part's
    /// mutable files (same B46 reason as the manifest above): each detached part keeps its own
    /// uuid/txn/metadata_version under its <detached_part>/ key prefix and they must coexist. The
    /// per-file objects are keyed per <detached_part>/<file> so they never collide across detached parts;
    /// only the bundle index overwrites, so merge the existing bundle in. The merged map also drives
    /// whether the sidecar must be (re)written even when this detach contributed no mutable files but a
    /// prior one did.
    std::map<std::string, std::string> merged_mutable(recorded_mutable.begin(), recorded_mutable.end());
    if (part_name == ContentAddressed::kDetachedDirName)
    {
        if (auto existing = metadata_storage.readRefSidecarIfExists(table_uuid, part_name))
            for (const auto & [file, bytes] : existing->files)
                merged_mutable.emplace(file, bytes); /// keep the new bytes on a key collision (re-detach)
    }

    /// FREEZE publishes into the shadow/<backup>/ namespace (one ref per frozen part); a live part uses
    /// the store/.../refs/ location. Select the ref-family keys accordingly.
    const bool is_frozen = !frozen_backup_name.empty();
    auto mutable_file_key = [&](const std::string & file)
    {
        return is_frozen
            ? ContentAddressed::shadowRefMutableFileKey(key_prefix, frozen_table_dir, part_name, file).string()
            : ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid, part_name, file).string();
    };
    const std::string meta_key = is_frozen
        ? ContentAddressed::shadowRefMetaKey(key_prefix, frozen_table_dir, part_name).string()
        : ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();

    if (!merged_mutable.empty())
    {
        /// Per-file objects FIRST: each mutable file's bytes verbatim in its own tiny object so the
        /// read path (getStorageObjects -> readObject) returns exactly that file's bytes. Only this
        /// detach's NEW files need writing; prior detached parts' per-file objects already exist.
        for (const auto & [file, bytes] : recorded_mutable)
        {
            const std::string file_key = mutable_file_key(file);
            auto file_out = metadata_storage.object_storage->writeObject(StoredObject(file_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }

        /// The bundle sidecar (the atomic per-part index of mutable files) written before the ref. It
        /// is the list/carry-forward source; per-file objects back the byte reads. Both live under the
        /// refs/ prefix, so a crashed write that never publishes the ref leaves only ref-scoped objects
        /// that removeRecursive reclaims and the reachability sweep (blobs/+parts/ only) cannot miss.
        ContentAddressed::RefSidecar sidecar;
        sidecar.files = merged_mutable;
        const std::string meta_bytes = sidecar.serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }

    // Publish the ref last: store/{server_id}/{table_uuid}/refs/{part_name} for a live part, or
    // shadow/<backup>/{server_id}/{table_uuid}/refs/{part_name} for a FREEZE target. The on-disk payload
    // is the versioned ref-payload struct (MAGIC+version+part_id) written by serializeRefPayload and
    // parsed by the single partIdFromRefPayload shared with the read path and the GC live-set scan (B28).
    const std::string ref_key = is_frozen
        ? ContentAddressed::shadowRefKey(key_prefix, frozen_table_dir, part_name).string()
        : ContentAddressed::refKey(key_prefix, metadata_storage.server_id, table_uuid, part_name).string();
    const std::string ref_payload = ContentAddressed::serializeRefPayload(part_id);
    auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
    ref_out->write(ref_payload.data(), ref_payload.size());
    ref_out->finalize();

    /// B52: the ref now names this part and keeps every referenced blob reachable, so the in-flight
    /// pins are no longer needed. Release them while STILL holding gc_lock (gc_guard above) so the pin
    /// drop and the ref publish are atomic w.r.t. the sweep: it can never observe a blob this commit
    /// reused as both unpinned AND not-yet-referenced. Erase directly here (do NOT call the lock-taking
    /// releasePinnedBlobs / unpinBlob — gc_lock is already held) and clear the local set so the
    /// destructor's release is a no-op.
    for (const auto & key : pinned_blob_keys)
        metadata_storage.inFlightPinnedBlobs()->erase(key);
    pinned_blob_keys.clear();

    /// M8: the ref is published, so the cross-mounter pin (the WriteSession object) is no longer needed
    /// — the ref now keeps every referenced blob reachable for a sweep on ANY mounter. Remove it. This
    /// is done OUTSIDE the gc_lock window above on purpose: it is a plain object delete on this
    /// transaction's OWN uniquely-keyed session object (no in-process state, so the local lock is
    /// irrelevant), and the ref publish above already closed the local race.
    releaseSession();
}

TransactionCommitOutcomeVariant ContentAddressedTransaction::tryCommit(const TransactionCommitOptionsVariant & options)
{
    if (!std::holds_alternative<NoCommitOptions>(options))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed transaction supports only tryCommit without options");

    commit(NoCommitOptions{});
    return true;
}

ObjectStorageKey ContentAddressedTransaction::generateObjectKeyForPath(const std::string & path)
{
    /// Phase 2 placeholder. Real content-addressed keying (blob/part/ref pools) is Phase 3.
    return ObjectStorageKey::createAsAbsolute(path);
}

StoredObjects ContentAddressedTransaction::getSubmittedForRemovalBlobs()
{
    return {};
}

void ContentAddressedTransaction::createMetadataFile(const std::string &, const StoredObjects &)
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ContentAddressed: createMetadataFile implemented in Phase 3");
}

void ContentAddressedTransaction::createDirectory(const std::string &)
{
    /// No-op: object storage has no real directories. existsDirectory derives from the
    /// refs/objects prefix. Mirrors the plain-rewritable transaction's treatment of directories.
}

void ContentAddressedTransaction::createDirectoryRecursive(const std::string &)
{
    /// No-op (see createDirectory).
}

bool ContentAddressedTransaction::renameCommittedPartRef(
    const ContentAddressed::PartFilePath & src, const ContentAddressed::PartFilePath & dst)
{
    /// Only a committed source ref can be renamed. An absent source ref means there is nothing
    /// committed to move (e.g. an INSERT's tmp_insert_<part> that was only staged in this transaction);
    /// the caller handles that case by adopting the destination as the staged target.
    auto src_pid = metadata_storage.readRefPartId(src.table_uuid, src.part_name);
    if (!src_pid)
        return false;

    /// Re-key the per-ref sidecar bundle and its per-file objects under the destination part name. The
    /// sidecar's logical keys are in-part file names and do not change; only the OBJECT keys, which are
    /// keyed by part name, move. Write the destination objects first, then publish the destination ref,
    /// then unlink every source ref object — so a crash never loses the only pointer to the manifest.
    if (auto src_sidecar = metadata_storage.readRefSidecarIfExists(src.table_uuid, src.part_name))
    {
        for (const auto & [file, bytes] : src_sidecar->files)
        {
            const std::string dst_file_key = ContentAddressed::refMutableFileKey(
                key_prefix, metadata_storage.server_id, dst.table_uuid, dst.part_name, file).string();
            auto file_out = metadata_storage.object_storage->writeObject(StoredObject(dst_file_key), WriteMode::Rewrite);
            file_out->write(bytes.data(), bytes.size());
            file_out->finalize();
        }

        const std::string dst_meta_key
            = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, dst.table_uuid, dst.part_name).string();
        const std::string meta_bytes = src_sidecar->serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(dst_meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }

    /// Publish the destination ref (the commit point of the rename).
    const std::string dst_ref_key
        = ContentAddressed::refKey(key_prefix, metadata_storage.server_id, dst.table_uuid, dst.part_name).string();
    const std::string ref_payload = ContentAddressed::serializeRefPayload(*src_pid);
    auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(dst_ref_key), WriteMode::Rewrite);
    ref_out->write(ref_payload.data(), ref_payload.size());
    ref_out->finalize();

    /// Unlink the source ref and its per-ref sidecar objects (ref + <part>.* sidecars), keeping the
    /// shared blobs/manifest. Match the basename exactly src.part_name or beginning with
    /// "src.part_name." so a different part sharing a name prefix is never touched.
    const std::string src_refs_prefix
        = ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, src.table_uuid);
    RelativePathsWithMetadata children;
    metadata_storage.object_storage->listObjects(src_refs_prefix, children, /*max_keys=*/0);
    StoredObjects to_remove;
    for (const auto & child : children)
    {
        const std::string & key = child->relative_path;
        const auto pos = key.rfind(src_refs_prefix);
        if (pos == std::string::npos)
            continue;
        const std::string name = key.substr(pos + src_refs_prefix.size());
        if (name == src.part_name || name.rfind(src.part_name + ".", 0) == 0)
            to_remove.emplace_back(key);
    }
    if (!to_remove.empty())
        metadata_storage.object_storage->removeObjectsIfExist(to_remove);
    return true;
}

bool ContentAddressedTransaction::unlinkPartDirRefs(const std::string & path)
{
    /// A regular part directory <uuid[:3]>/<uuid>/<part> (a part path with no file component): delete
    /// this part's ref plus its per-ref sidecars. The objects for part P are: <refsPrefix>P (the ref),
    /// <refsPrefix>P.meta (the bundle), and <refsPrefix>P.<file>.meta (per-file). Match the basename
    /// exactly P or beginning with "P." — never a different part that merely shares a name prefix (e.g.
    /// all_1_1_0 must not reach all_1_1_0_1, whose next char is '_', not '.'). The sidecars are
    /// ref-scoped and NOT content-addressed, so they MUST be removed synchronously here or they leak
    /// (the reachability sweep scans only blobs/+parts/). Absent ref (e.g. an uncommitted tmp part) is
    /// a no-op. PartManifest + blobs are kept (deferred GC). The detached namespace and table dirs are
    /// NOT regular part dirs and are not handled here.
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || !p->file.empty() || p->part_name == ContentAddressed::kDetachedDirName)
        return false;

    const std::string refs_prefix
        = ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, p->table_uuid);
    RelativePathsWithMetadata children;
    metadata_storage.object_storage->listObjects(refs_prefix, children, /*max_keys=*/0);
    StoredObjects to_remove;
    for (const auto & child : children)
    {
        const std::string & key = child->relative_path;
        const auto pos = key.rfind(refs_prefix);
        if (pos == std::string::npos)
            continue;
        const std::string name = key.substr(pos + refs_prefix.size());
        if (name == p->part_name || name.rfind(p->part_name + ".", 0) == 0)
            to_remove.emplace_back(key);
    }
    if (!to_remove.empty())
        metadata_storage.object_storage->removeObjectsIfExist(to_remove);
    return true;
}

void ContentAddressedTransaction::removeDirectory(const std::string & path)
{
    /// MergeTree removes a COMPLETE part via the fast path (DataPartStorageOnDiskBase::clearDirectory):
    /// it unlinks the part's files one by one and then calls removeDirectory(<part>) — it never calls
    /// removeRecursive on the part directory. For a content-addressed disk the per-file unlinks are
    /// no-ops on committed refs (the ref/manifest must survive until the directory is dropped), so this
    /// removeDirectory(<part>) is the SINGLE authoritative point at which the part's ref must be
    /// unlinked. If it stays a no-op the ref lingers and the part is rediscovered on the next
    /// DETACH/ATTACH (B45). Route a part-directory removal through the same ref-unlink as removeRecursive.
    if (unlinkPartDirRefs(path))
        return;

    /// Otherwise (table dir, detached namespace, generic dir): no-op (see createDirectory). object
    /// storage has no real directories; existsDirectory derives from the refs/objects prefix. The
    /// detached namespace and table dirs are removed via removeRecursive, never plain removeDirectory.
}

void ContentAddressedTransaction::removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate &)
{
    /// Removal = pointer-unlink + deferred GC. We delete only the pointer objects the path covers
    /// (refs and verbatim table-level / generic files) and never the shared blobs/ content or
    /// parts/ manifests — the Phase-4 reachability GC reclaims those. The should_remove_objects
    /// predicate is about whether to delete the shared backing objects (blobs); for content
    /// addressing we always KEEP them (deferred GC) and always remove the ref metadata, so the
    /// predicate does not gate ref deletion and is intentionally ignored here.
    /// TODO(phase4-gc): orphaned blobs/manifests leak until the reachability GC runs.

    /// Enumerate every object under a key prefix and delete it. listObjects(prefix) returns objects
    /// whose key starts with prefix, exactly as ContentAddressedMetadataStorage's directory listing
    /// enumerates refs.
    auto remove_under_prefix = [this](const std::string & prefix)
    {
        RelativePathsWithMetadata children;
        metadata_storage.object_storage->listObjects(prefix, children, /*max_keys=*/0);

        StoredObjects to_remove;
        to_remove.reserve(children.size());
        for (const auto & child : children)
            to_remove.emplace_back(child->relative_path);

        if (!to_remove.empty())
            metadata_storage.object_storage->removeObjectsIfExist(to_remove);
    };

    /// FREEZE shadow namespace (shadow/<backup>/store/…). Routed BEFORE every live branch so a shadow
    /// part dir never hits unlinkPartDirRefs (which uses the LIVE refsPrefix and would mistarget), and a
    /// shadow table/intermediate/backup dir never hits the live parseTableUuid branch.
    if (ContentAddressed::isShadowPath(path))
    {
        /// Shadow PART dir shadow/<backup>/store/<uuid[:3]>/<uuid>/<part>: delete this frozen part's
        /// shadow ref plus its sidecars (the bundle <part>.meta and each per-file <part>.<file>.meta).
        /// Mirror unlinkPartDirRefs but list under the SHADOW refs prefix and match the basename exactly
        /// <part> or beginning with "<part>." (never a sibling part sharing a name prefix). Blobs and the
        /// manifest are kept (deferred GC); the shadow ref's disappearance lets the GC reclaim them once
        /// no other ref keeps them reachable.
        if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->backup_name.empty() && p->file.empty())
        {
            const std::string refs_prefix = ContentAddressed::shadowRefsPrefix(key_prefix, p->shadow_table_dir);
            RelativePathsWithMetadata children;
            metadata_storage.object_storage->listObjects(refs_prefix, children, /*max_keys=*/0);
            StoredObjects to_remove;
            for (const auto & child : children)
            {
                const std::string & key = child->relative_path;
                const auto pos = key.rfind(refs_prefix);
                if (pos == std::string::npos)
                    continue;
                const std::string name = key.substr(pos + refs_prefix.size());
                if (name == p->part_name || name.rfind(p->part_name + ".", 0) == 0)
                    to_remove.emplace_back(key);
            }
            if (!to_remove.empty())
                metadata_storage.object_storage->removeObjectsIfExist(to_remove);
            return;
        }

        /// Shadow TABLE dir / INTERMEDIATE / backup root (shadow/<backup>[/store[/<uuid[:3]>[/<uuid>]]]):
        /// delete every object under the dir's key prefix — all shadow refs/sidecars beneath it. This
        /// handles removeRecursive(shadow/<backup>) (the full UNFREEZE WITH NAME cleanup) and any subtree.
        remove_under_prefix(ContentAddressed::diskFileKey(key_prefix, path + "/"));
        return;
    }

    /// Part directory <uuid[:3]>/<uuid>/<part> (a part path with no file component): delete the
    /// single ref AND the part's per-ref sidecar objects (the bundle <part>.meta and each per-file
    /// <part>.<file>.meta). The sidecars are ref-scoped and NOT content-addressed, so the reachability
    /// sweep (blobs/+parts/ only) would never reclaim them — they MUST be removed synchronously with
    /// the ref here or they leak (B23 orphan concern). Absent ref (e.g. an uncommitted tmp part) is a
    /// no-op. PartManifest + blobs are kept (deferred GC).
    /// A detached part DIRECTORY <uuid[:3]>/<uuid>/detached/<detached_part>: the detached parts share the
    /// one "detached" ref whose manifest/sidecar keys are <detached_part>/<file> (B36/B46). DROP DETACHED
    /// PARTITION reaches here; it must remove ONLY this <detached_part>/ key prefix from the shared
    /// detached ref (manifest + sidecar bundle) and the matching per-file sidecar objects, leaving the
    /// other detached parts intact. When the last detached part is removed the ref+bundle are unlinked.
    /// (Blobs/manifests are kept for deferred GC, as everywhere.)
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && p->part_name == ContentAddressed::kDetachedDirName && !p->file.empty() && p->file.find('/') == std::string::npos)
    {
        const std::string detached_dir = p->file;
        const std::string key_pfx = detached_dir + "/";

        auto existing_pid = metadata_storage.readRefPartId(p->table_uuid, p->part_name);
        if (!existing_pid)
            return; /// no detached ref at all => nothing to drop

        /// Rebuild the manifest without this detached part's keys.
        auto manifest = metadata_storage.loadPartManifestOrThrow(*existing_pid);
        std::erase_if(manifest.blobs, [&](const auto & kv) { return kv.first.rfind(key_pfx, 0) == 0; });

        /// Rebuild the sidecar bundle without this detached part's keys, and delete the per-file sidecar
        /// objects (refs/<detached>.<detached_part>/<file>.meta) backing this detached part's byte reads.
        ContentAddressed::RefSidecar sidecar;
        if (auto existing_sidecar = metadata_storage.readRefSidecarIfExists(p->table_uuid, p->part_name))
            sidecar = *existing_sidecar;
        StoredObjects mutable_to_remove;
        for (auto it = sidecar.files.begin(); it != sidecar.files.end();)
        {
            if (it->first.rfind(key_pfx, 0) == 0)
            {
                mutable_to_remove.emplace_back(
                    ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, p->table_uuid, p->part_name, it->first).string());
                it = sidecar.files.erase(it);
            }
            else
                ++it;
        }
        if (!mutable_to_remove.empty())
            metadata_storage.object_storage->removeObjectsIfExist(mutable_to_remove);

        const std::string ref_key
            = ContentAddressed::refKey(key_prefix, metadata_storage.server_id, p->table_uuid, p->part_name).string();
        const std::string meta_key
            = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, p->table_uuid, p->part_name).string();

        if (manifest.blobs.empty())
        {
            /// No detached parts remain: unlink the shared ref and its bundle sidecar.
            metadata_storage.object_storage->removeObjectsIfExist({StoredObject(ref_key), StoredObject(meta_key)});
            return;
        }

        /// Republish the trimmed manifest under its new part id and re-point the ref; rewrite the bundle.
        const ContentAddressed::PartId new_pid = ContentAddressed::computePartId(manifest.blobs);
        const std::string new_part_key = ContentAddressed::partKey(key_prefix, new_pid).string();
        if (!metadata_storage.object_storage->tryGetObjectMetadata(new_part_key, /*with_tags=*/false).has_value())
        {
            const std::string bytes = manifest.serialize();
            auto out = metadata_storage.object_storage->writeObject(StoredObject(new_part_key), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size());
            out->finalize();
        }
        {
            const std::string meta_bytes = sidecar.serialize();
            auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
            meta_out->write(meta_bytes.data(), meta_bytes.size());
            meta_out->finalize();
        }
        {
            const std::string ref_payload = ContentAddressed::serializeRefPayload(new_pid);
            auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
            ref_out->write(ref_payload.data(), ref_payload.size());
            ref_out->finalize();
        }
        return;
    }

    /// A regular part directory <uuid[:3]>/<uuid>/<part> (no file component): unlink this part's ref
    /// plus its per-ref sidecars (shared with the fast-removal removeDirectory path — B45). Blobs and
    /// the manifest are kept (deferred GC).
    if (unlinkPartDirRefs(path))
        return;

    /// Table directory <uuid[:3]>/<uuid> (parses to a table uuid, no part): delete ALL refs and ALL
    /// verbatim table-level files for this (server, table). Scoped by table_uuid so a DROP of one
    /// table never touches another table's refs in the shared pool. Blobs/manifests are kept.
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        remove_under_prefix(ContentAddressed::refsPrefix(key_prefix, metadata_storage.server_id, *uuid));
        remove_under_prefix(ContentAddressed::tableFilesPrefix(key_prefix, metadata_storage.server_id, *uuid));
        return;
    }

    /// Generic disk-level path (neither a part nor a table dir, e.g. the disk root or an
    /// access-check probe directory): delete the verbatim objects under its diskFileKey prefix.
    remove_under_prefix(ContentAddressed::diskFileKey(key_prefix, path));
}

}

namespace DB::ContentAddressed
{

ContentAddressedWriteBuffer::ContentAddressedWriteBuffer(
    ObjectStoragePtr object_storage_,
    std::string key_prefix_,
    std::string temp_dir_,
    std::shared_ptr<std::mutex> gc_lock_,
    std::shared_ptr<std::set<std::string>> in_flight_pinned_blobs_,
    OnFinalized on_finalized_,
    OnPinBlob on_pin_blob_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , object_storage(std::move(object_storage_))
    , key_prefix(std::move(key_prefix_))
    , gc_lock(std::move(gc_lock_))
    , in_flight_pinned_blobs(std::move(in_flight_pinned_blobs_))
    , on_finalized(std::move(on_finalized_))
    , on_pin_blob(std::move(on_pin_blob_))
{
    fs::create_directories(temp_dir_);
    temp_path = temp_dir_ + "/" + getRandomASCIIString(32) + ".tmp";

    temp_file = std::make_unique<WriteBufferFromFile>(temp_path);
    hashing = std::make_unique<HashingWriteBuffer>(*temp_file);
}

ContentAddressedWriteBuffer::~ContentAddressedWriteBuffer()
{
    /// Best-effort cleanup if finalize() was never called (e.g. an exception unwound the stack).
    cancel();
    removeTempFile();
}

void ContentAddressedWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    hashing->write(working_buffer.begin(), offset());
}

void ContentAddressedWriteBuffer::uploadBlobAtomically(const std::string & key)
{
    /// Publish the blob so a concurrent reader/writer NEVER observes a partially-written object at the
    /// final content-hash key (B41). LocalObjectStorage writes objects IN PLACE (no temp+rename), so a
    /// plain writeObject(key) followed by a streaming copy is visible at `key` while it is still being
    /// filled — a second writer racing the same content hash then sees a size-0 (or short) object and
    /// the size-guard above fires `CORRUPTED_DATA` on a perfectly valid concurrent insert. So for the
    /// local backend we upload to a UNIQUE temp object key in the SAME directory (same filesystem, so
    /// rename is a cheap metadata op) and then atomically rename it onto the final key — the final key
    /// only ever appears fully written. For an object storage whose single PUT is already atomic (S3,
    /// Azure: an object is not visible until the PUT completes) the plain one-shot upload is sufficient.
    if (object_storage->getType() == ObjectStorageType::Local)
    {
        /// For LocalObjectStorage the object "key" IS the on-disk path, so a temp key in the same
        /// parent directory shares the filesystem and std::filesystem::rename is atomic.
        const std::string temp_key = key + ".tmp." + getRandomASCIIString(16);
        {
            ReadBufferFromFile in(temp_path);
            auto out = object_storage->writeObject(StoredObject(temp_key), WriteMode::Rewrite);
            copyData(in, *out);
            out->finalize();
        }

        std::error_code ec;
        fs::rename(temp_key, key, ec);
        if (ec)
        {
            /// Clean up the temp object on failure; the final key is untouched (never partial).
            object_storage->removeObjectIfExists(StoredObject(temp_key));
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "Failed to atomically publish content-addressed blob {} (rename from {} failed: {})",
                key, temp_key, ec.message());
        }
        return;
    }

    ReadBufferFromFile in(temp_path);
    auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
    copyData(in, *out);
    out->finalize();
}

void ContentAddressedWriteBuffer::finalizeImpl()
{
    /// Flush our own buffered data into the hashing buffer first.
    next();

    size = count();

    /// getHash() flushes the hashing buffer (and thus the temp file buffer) and returns the
    /// cityHash128 of everything written.
    const auto hash = hashing->getHash();
    blob_hash = getHexUIntLowercase(hash);

    hashing->finalize();
    temp_file->finalize();

    const std::string key = blobKey(key_prefix, BlobHash(blob_hash)).string();

    /// B52: PIN the blob key for the lifetime of this transaction BEFORE deciding whether to skip the
    /// upload, and make that existence-check decision UNDER the GC lock. The background sweep holds the
    /// SAME lock for its whole mark+delete and treats every pinned key as reachable, so the only two
    /// orderings are: (a) we pin first -> the next sweep sees the pin and keeps the blob, even though no
    /// ref names it yet; (b) the sweep deleted the blob just before we took the lock -> our existence
    /// check now sees it absent and we RE-UPLOAD it (we still hold the local temp file). Either way the
    /// blob this transaction reuses is alive when the ref is later published. The pin is released by the
    /// owning transaction once the ref is published (commit) or when an uncommitted transaction is
    /// destroyed. Without the background GC wiring (unit/legacy construction) gc_lock is null and we
    /// fall back to a plain existence check (no concurrent sweep can race).
    auto decide_and_pin = [&]() -> std::optional<ObjectMetadata>
    {
        if (in_flight_pinned_blobs)
            in_flight_pinned_blobs->insert(key);
        /// M8: also publish the CROSS-mounter pin (the WriteSession object) BEFORE the upload, while
        /// the GC lock is held. The in-process pin above only protects this server's own sweep; the
        /// session object protects a sweep running on ANOTHER mounter that lists the bucket and would
        /// otherwise see this just-uploaded-but-not-yet-referenced blob as unreachable (data loss). It
        /// must be durable before the blob exists, so it is taken here (pin-before-upload), not in the
        /// post-upload on_finalized callback.
        if (on_pin_blob)
            on_pin_blob(BlobHash(blob_hash));
        return object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
    };

    std::optional<ObjectMetadata> existing;
    if (gc_lock)
    {
        std::lock_guard<std::mutex> gc_guard(*gc_lock);
        existing = decide_and_pin();
    }
    else
    {
        existing = decide_and_pin();
    }

    /// Skip re-uploading when the blob already exists (content dedup). The key IS the content hash, so
    /// a racing writer to the same key has identical bytes; the worst case is a redundant upload, never
    /// wrong content. We DO guard one thing: if an object already exists at the key with a DIFFERENT
    /// size, that is either a 128-bit hash collision or a genuinely corrupt blob — fail closed.
    if (existing.has_value())
    {
        if (existing->size_bytes != size)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "Content-addressed blob {} already exists with size {} but new content has size {} "
                "(hash collision or partially-written blob)",
                key, existing->size_bytes, size);
    }
    else
    {
        uploadBlobAtomically(key);
    }

    removeTempFile();

    /// The hash is known and the blob is durable; let the owning transaction record it (typed).
    if (on_finalized)
        on_finalized(BlobHash(blob_hash), size);
}

void ContentAddressedWriteBuffer::sync()
{
    next();
    if (hashing)
        hashing->sync();
}

std::string ContentAddressedWriteBuffer::getFileName() const
{
    return temp_path;
}

void ContentAddressedWriteBuffer::removeTempFile() noexcept
{
    if (temp_path.empty())
        return;
    std::error_code ec;
    fs::remove(temp_path, ec);
}

ContentAddressedInlineWriteBuffer::ContentAddressedInlineWriteBuffer(OnInlined on_inlined_)
    : WriteBufferFromFileBase(DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
    , on_inlined(std::move(on_inlined_))
{
}

ContentAddressedInlineWriteBuffer::~ContentAddressedInlineWriteBuffer()
{
    /// Best-effort cleanup if finalize() was never called (e.g. an exception unwound the stack).
    cancel();
}

void ContentAddressedInlineWriteBuffer::nextImpl()
{
    if (!offset())
        return;
    accumulated.append(working_buffer.begin(), offset());
}

void ContentAddressedInlineWriteBuffer::finalizeImpl()
{
    /// Flush our own buffered data into the accumulator first.
    next();
    if (on_inlined)
        on_inlined(std::move(accumulated));
}

void ContentAddressedInlineWriteBuffer::sync()
{
    next();
}

std::string ContentAddressedInlineWriteBuffer::getFileName() const
{
    return "<content-addressed-inline>";
}

}
