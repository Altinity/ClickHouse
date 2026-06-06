#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/RefPayload.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcCompaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcLogWriter.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Common/Exception.h>
#include <Common/ObjectStorageKey.h>
#include <Common/ProfileEvents.h>
#include <Common/getRandomASCIIString.h>
#include <Common/logger_useful.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadSettings.h>
#include <IO/copyData.h>
#include <IO/WriteBufferFromString.h>

#include <chrono>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;

namespace ProfileEvents
{
    extern const Event ContentAddressedGenerationResurrectionsTotal;
    extern const Event ContentAddressedDuplicateGenerationBytes;
}

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

namespace
{
    /// A part-DIRECTORY (not a part FILE) reached a moveFile/replaceFile path. Two dir shapes occur:
    ///   - an ACTIVE part dir `store/<uuid>/<part>` -> parsePartFilePath yields an empty `file`;
    ///   - a DETACHED part dir `store/<uuid>/detached/<part>` -> part_name is `kDetachedDirName` and `file`
    ///     is the single-component detached dir name (no '/').
    /// Both are directory re-keys moveDirectory owns. A real part-FILE never matches: an active file has a
    /// non-empty `file`, and a detached file is `detached/<part>/<subfile>` so its `file` contains '/'.
    /// Shared by moveFile (B87) and replaceFile (symmetry / defense-in-depth — no caller passes a part dir to
    /// replaceFile today, but mirror the guard so a future caller cannot regress into the B87 LOGICAL_ERROR).
    bool isPartDirPath(const std::optional<ContentAddressed::PartFilePath> & p)
    {
        if (!p)
            return false;
        if (p->file.empty())
            return true; /// active part dir: store/<uuid>/<part>
        return p->part_name == ContentAddressed::kDetachedDirName
            && p->file.find('/') == std::string::npos; /// detached part dir: store/<uuid>/detached/<part>
    }
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
    /// lingering session — abort-before-commit stays O(1) (drop the session; nothing was referenced).
    ///
    /// CA GC S4 (§5.1 rule 3, §7.3): a COMMITTED session must NOT be dropped here — it is retained until its
    /// `+` deltas are FOLDED (the session-until-folded reaper in the GC sweep deletes it once the folded
    /// watermark passes its recorded epochs). commit() already either dropped it (all epochs already folded)
    /// or marked it committed and persisted it for the reaper; in the latter case the transaction object is
    /// destroyed while its durable session legitimately lives on. So only release an UNCOMMITTED session.
    if (session_open && !session.committed)
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

    /// The LAST-remembered target (most recent rememberTarget). A single transaction may write more than
    /// one part (a transactional merge — B67), so this no longer means "the only part".
    table_uuid = p->table_uuid;
    part_name = p->part_name;

    auto & st = stagingForPath(*p);
    /// Pin the FREEZE target this part's files must agree on. The first remember of a part stamps it;
    /// later files of the same part must carry the same backup name.
    if (st.frozen_backup_name.empty() && st.frozen_table_dir.empty())
    {
        st.frozen_backup_name = p->backup_name; /// empty for a live part; the FREEZE backup name otherwise
        st.frozen_table_dir = p->shadow_table_dir; /// empty for a live part; the shadow table dir otherwise
    }
    else if (st.frozen_backup_name != p->backup_name)
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: inconsistent FREEZE target for {}/{}: '{}' vs '{}'",
            p->table_uuid, p->part_name, st.frozen_backup_name, p->backup_name);
}

void ContentAddressedTransaction::recordBlob(const std::string & path, ContentAddressed::BlobEntry entry)
{
    auto p = ContentAddressed::parsePartFilePath(path);
    chassert(p && !p->file.empty());
    stagingForPath(*p).recorded[p->file] = std::move(entry);
}

std::optional<StoredObjects> ContentAddressedTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    const auto * st = findStaging(p->table_uuid, p->part_name);
    if (!st)
        return {};
    /// A content file: its blob is already uploaded (recorded under the in-part file name). Mirror the
    /// committed getStorageObjects resolve: project the BARE BlobHash to the full blob object key.
    if (auto it = st->recorded.find(p->file); it != st->recorded.end())
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
    const auto * st = findStaging(p->table_uuid, p->part_name);
    if (!st)
        return nullptr;
    if (auto it = st->recorded.find(p->file); it != st->recorded.end())
    {
        StoredObject obj(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size);
        return metadata_storage.object_storage->readObject(obj, settings, read_hint);
    }
    if (auto it = st->recorded_mutable.find(p->file); it != st->recorded_mutable.end())
        return std::make_unique<ReadBufferFromOwnMemoryFile>(path, it->second); // inline staged bytes
    return nullptr;
}

std::optional<uint64_t> ContentAddressedTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    const auto * st = findStaging(p->table_uuid, p->part_name);
    if (!st)
        return {};
    if (auto it = st->recorded.find(p->file); it != st->recorded.end())
        return it->second.size;
    if (auto it = st->recorded_mutable.find(p->file); it != st->recorded_mutable.end())
        return static_cast<uint64_t>(it->second.size());
    return {};
}

bool ContentAddressedTransaction::hasInFlightDirectory(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    const auto * st = findStaging(p->table_uuid, p->part_name);
    if (!st)
        return false;
    // A staged file directly under p->file (i.e. p->file + "/" + inner) means the directory p->file exists
    // in-flight. Scan both the content-blob staging (recorded) and the inline mutable-file staging
    // (recorded_mutable).
    const std::string prefix = p->file + "/";
    auto under = [&prefix](const auto & m)
    {
        // map is sorted: the first key >= prefix that starts with it is the only one we need to check.
        auto it = m.lower_bound(prefix);
        return it != m.end() && it->first.starts_with(prefix);
    };
    return under(st->recorded) || under(st->recorded_mutable);
}

std::vector<std::string> ContentAddressedTransaction::listInFlightDirectory(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    const auto * st = findStaging(p->table_uuid, p->part_name);
    if (!st)
        return {};
    const std::string prefix = p->file + "/";
    std::set<std::string> children;
    auto collect = [&](const auto & m)
    {
        for (auto it = m.lower_bound(prefix); it != m.end() && it->first.starts_with(prefix); ++it)
        {
            // The immediate child is the single path component following the prefix; a deeper-nested file
            // surfaces only as its first component (a sub-directory name), one level only.
            std::string_view rest(it->first);
            rest.remove_prefix(prefix.size());
            auto slash = rest.find('/');
            children.emplace(slash == std::string_view::npos ? std::string(rest) : std::string(rest.substr(0, slash)));
        }
    };
    collect(st->recorded);
    collect(st->recorded_mutable);
    return {children.begin(), children.end()};
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

        if (mode == WriteMode::Append)
        {
            /// Object storage cannot append; carry the existing bytes forward and rewrite the whole
            /// object so the caller's appended bytes land after them. The verbatim object is small
            /// (e.g. mutation_<n>.txt, to which the MVCC commit appends the CSN line in afterCommit) and
            /// fully readable at its stable key. readSmallObjectIfExists returns nullopt when the object
            /// does not exist yet (an append that creates the file), in which case there is nothing to
            /// carry forward.
            std::optional<std::string> existing = metadata_storage.readSmallObjectIfExists(key);
            auto out = metadata_storage.object_storage->writeObject(
                StoredObject(key, path), WriteMode::Rewrite, /*attributes=*/std::nullopt, buf_size, settings);
            if (existing && !existing->empty())
                out->write(existing->data(), existing->size());
            return out;
        }

        return metadata_storage.object_storage->writeObject(StoredObject(key, path), mode, /*attributes=*/std::nullopt, buf_size, settings);
    }

    rememberTarget(path);

    auto p = ContentAddressed::parsePartFilePath(path);
    const std::string file = p->file;
    /// Route the staged file into THIS part's entry. The callbacks below fire on finalize, possibly after
    /// other parts have been remembered, so they must address the part by its own (table_uuid, part_name)
    /// rather than the transaction-wide "last remembered" target.
    const std::string part_table_uuid = p->table_uuid;
    const std::string part_part_name = p->part_name;

    /// A MUTABLE per-part file (uuid.txt / txn_version.txt / metadata_version.txt) is NOT
    /// content-addressed: it is stored inline in this part's per-ref sidecar so two parts with
    /// identical content keep their own mutable bytes. Capture the bytes in memory and record them
    /// for the sidecar at commit — no blob is uploaded (so no orphan blob is created).
    if (ContentAddressed::isMutablePerPartFile(file))
    {
        return std::make_unique<ContentAddressed::ContentAddressedInlineWriteBuffer>(
            [this, part_table_uuid, part_part_name, file](std::string bytes)
            { stagingFor(part_table_uuid, part_part_name).recorded_mutable[file] = std::move(bytes); });
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
        buf_size,
        settings.use_adaptive_write_buffer,
        settings.adaptive_write_buffer_initial_size,
        metadata_storage.gcLock(),
        metadata_storage.inFlightPinnedBlobs(),
        [this, part_table_uuid, part_part_name, file](const ContentAddressed::BlobHash & blob_hash, size_t size)
        {
            stagingFor(part_table_uuid, part_part_name).recorded[file]
                = ContentAddressed::BlobEntry{blob_hash, size, blob_hash.string()};
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
    auto & dst_st = stagingForPath(*dst);
    if (auto src = ContentAddressed::parsePartFilePath(from); src && !src->file.empty()
        && ContentAddressed::isMutablePerPartFile(src->file))
    {
        if (const auto * src_st = findStaging(src->table_uuid, src->part_name))
        {
            if (auto it = src_st->recorded_mutable.find(src->file); it != src_st->recorded_mutable.end())
            {
                dst_st.recorded_mutable[dst->file] = it->second;
                return;
            }
        }
        dst_st.recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
        return;
    }

    /// Resolve the source blob: prefer an in-flight blob recorded earlier in this same commit,
    /// else fall back to the committed source part via the metadata storage.
    if (auto src = ContentAddressed::parsePartFilePath(from); src && !src->file.empty())
    {
        if (const auto * src_st = findStaging(src->table_uuid, src->part_name))
        {
            if (auto it = src_st->recorded.find(src->file); it != src_st->recorded.end())
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
        /// A concurrent ATTACH of the SAME detached part won the race: it already consumed the shared
        /// `detached` ref (republished it under a new part id and unlinked the staging keys, below), so our
        /// staging source no longer exists. On a plain disk the same race surfaces as `FILE_DOESNT_EXIST`
        /// from `DataPartStorageOnDiskBase::rename` (its `setLastModified`/`moveDirectory` on the vanished
        /// staging dir), which ATTACH treats as a recoverable failure of this attempt — NOT a logical error.
        /// Mirror that here instead of aborting the server (the staging+load happen outside `lockParts`, so
        /// two ATTACH queries can both stage `attaching_X` for the same detached part — see 01164).
        throw Exception(
            ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: moveDirectory of detached/{} into active {}/{}, but the detached ref is absent "
            "(a concurrent ATTACH consumed it)",
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
        /// As above: a concurrent ATTACH of the same detached part already stripped this staging dir's keys
        /// from the still-present shared `detached` ref (another detached part keeps the ref alive). Our
        /// staging source is gone — a recoverable per-attempt failure, mirroring the plain disk's
        /// `FILE_DOESNT_EXIST`, not a logical error / server abort.
        throw Exception(
            ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: moveDirectory of detached/{} into active {}/{}, but the detached ref has no "
            "entries under that staging directory (a concurrent ATTACH consumed it)",
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

    auto st_it = parts.find(PartKey{src.table_uuid, src.part_name});
    if (st_it == parts.end())
        return false;
    PartStaging & st = st_it->second;

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

    rekey_map(st.recorded);
    rekey_map(st.recorded_mutable);
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

    auto src_part = ContentAddressed::parsePartFilePath(from);

    /// Re-key any STAGED source part into the destination key (B67 deferred merge tmp_merge_X -> X). A
    /// transactional merge stages the merge-output part under a temporary name and a CSN write can arrive
    /// under the FINAL name before this rename re-keys (the deferred-rename window). Merge the source
    /// staging entry into the destination entry: carry over the content blobs and the mutable files; on a
    /// recorded_mutable key collision PREFER the existing DEST bytes (a txn_version.txt already staged under
    /// the final name is the newer MVCC state); preserve the FREEZE target (dest's if set, else source's).
    /// For the single-part case the source key equals the LAST-remembered target and the dest is a fresh
    /// key, so this is exactly the old "re-pin to destination" with no collisions — byte-equivalent.
    if (src_part && src_part->file.empty())
    {
        const PartKey src_key{src_part->table_uuid, src_part->part_name};
        const PartKey dst_key{dst_part->table_uuid, dst_part->part_name};
        if (src_key != dst_key)
        {
            if (auto src_it = parts.find(src_key); src_it != parts.end())
            {
                PartStaging & dst_st = stagingFor(dst_part->table_uuid, dst_part->part_name);
                PartStaging & src_st = src_it->second;
                for (auto & [file, entry] : src_st.recorded)
                    dst_st.recorded.insert_or_assign(file, std::move(entry));
                for (auto & [file, bytes] : src_st.recorded_mutable)
                    dst_st.recorded_mutable.emplace(file, std::move(bytes)); /// PREFER existing dest bytes on collision
                for (const auto & file : src_st.recorded_mutable_removed)
                    dst_st.recorded_mutable_removed.insert(file);
                if (dst_st.frozen_backup_name.empty() && dst_st.frozen_table_dir.empty())
                {
                    dst_st.frozen_backup_name = src_st.frozen_backup_name;
                    dst_st.frozen_table_dir = src_st.frozen_table_dir;
                }
                parts.erase(src_it);
            }
        }
    }

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
        if (src_part && src_part->file.empty() && renameCommittedPartRef(*src_part, *dst_part))
            return;

        table_uuid = dst_part->table_uuid;
        part_name = dst_part->part_name;
        return;
    }

    /// The source must be the part we have been assembling.
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
        return;
    }

    /// The source is NOT the last-remembered part. With per-part staging a single transaction may rename a
    /// part other than the most-recently-touched one (B67: a merge's deferred tmp_merge_X -> X while the
    /// last write touched a covered source part's txn_version). The staging re-key above already moved the
    /// source entry into the destination key. If the source was a COMMITTED part (a merged/mutated part, or
    /// a delete_tmp_ rename), re-key its ref so it is not rediscovered (B45); otherwise it was an
    /// in-this-transaction tmp dir whose ref does not exist yet — unlink is a no-op. Re-pin the
    /// last-remembered target to the destination so a subsequent bare reference resolves it.
    if (src_part && src_part->file.empty())
    {
        if (!renameCommittedPartRef(*src_part, *dst_part))
            unlinkPartDirRefs(from);
        table_uuid = dst_part->table_uuid;
        part_name = dst_part->part_name;
        return;
    }

    throw Exception(
        ErrorCodes::LOGICAL_ERROR,
        "ContentAddressed: moveDirectory from {} to {} does not match the staged part {}/{}",
        from, to, table_uuid, part_name);
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
    /// A part-DIRECTORY rename reached moveFile (PartsTemporaryRename::rollBackAll undoes an attach via
    /// moveFile, whereas the forward tryRenameAll uses moveDirectory). Two dir shapes occur:
    ///   - an ACTIVE part dir `store/<uuid>/<part>`  -> parsePartFilePath yields an empty `file`;
    ///   - a DETACHED part dir `store/<uuid>/detached/<part>` (the attach-rollback case) -> part_name is
    ///     `kDetachedDirName` and `file` is the single-component detached dir name (no '/').
    /// Both are directory re-keys moveDirectory already owns (incl. the detached<->detached rekeyDetachedPartDir
    /// branch). Delegate instead of throwing LOGICAL_ERROR (B87). A real part-FILE move never matches: an active
    /// file has a non-empty `file`, and a detached file is `detached/<part>/<subfile>` so its `file` contains '/'.
    if (isPartDirPath(src) && isPartDirPath(dst))
    {
        moveDirectory(from, to);
        return;
    }
    if (!src || src->file.empty() || !dst || dst->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile requires two part-file paths: {} -> {}", from, to);

    rememberTarget(to);

    /// Re-key the file from the source part's entry to the destination part's entry. For the common
    /// same-part rename both resolve to the SAME entry (current behavior); a cross-part rename moves the
    /// file between two distinct entries.
    auto & src_st = stagingForPath(*src);
    auto & dst_st = stagingForPath(*dst);

    /// A mutable per-part file is staged inline (recorded_mutable), not as a blob; re-key it there.
    if (auto mit = src_st.recorded_mutable.find(src->file); mit != src_st.recorded_mutable.end())
    {
        auto bytes = std::move(mit->second);
        src_st.recorded_mutable.erase(mit);
        dst_st.recorded_mutable[dst->file] = std::move(bytes);
        return;
    }

    if (auto it = src_st.recorded.find(src->file); it != src_st.recorded.end())
    {
        auto entry = it->second;
        src_st.recorded.erase(it);
        dst_st.recorded[dst->file] = std::move(entry);
        return;
    }

    /// Source not staged in THIS transaction: a standalone autocommit rename across one-shot
    /// transactions (e.g. the MVCC layer's first txn_version.txt write — VersionMetadataOnDisk
    /// autocommits txn_version.txt.tmp into the part's sidecar, then DiskObjectStorage::replaceFile
    /// finds no existing txn_version.txt and so calls moveFile(.tmp -> txn_version.txt) as its own
    /// one-shot). The destination is a mutable per-part file, so resolve the already-committed source
    /// bytes from the sidecar and re-stage them under the destination, marking the source removed; the
    /// mutable-only commit branch then publishes the rename to the sidecar. (A content destination
    /// cannot be renamed standalone — its blob+manifest were never committed without a part target.)
    if (ContentAddressed::isMutablePerPartFile(dst->file))
    {
        dst_st.recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
        src_st.recorded_mutable_removed.insert(src->file);
        return;
    }

    throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile source not recorded: {}", from);
}

std::string ContentAddressedTransaction::readStagedOrCommittedBytes(const std::string & path) const
{
    /// Read the bytes of a part file written in THIS transaction as a content blob (just uploaded), so a
    /// rename whose destination is mutable can re-stage them inline. Falls back to the committed sidecar.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->file.empty())
    {
        if (const auto * st = findStaging(p->table_uuid, p->part_name); st)
        if (auto it = st->recorded.find(p->file); it != st->recorded.end())
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
    /// A part-DIRECTORY rename reached replaceFile — delegate to moveDirectory exactly as moveFile does (B87
    /// symmetry / defense-in-depth). No caller passes a part dir to replaceFile today, but mirror the guard so
    /// a future caller cannot regress: without it an active part dir (empty `file`) would fall into the
    /// verbatim branch below and moveFile a directory verbatim. Routed before the verbatim branch.
    if (isPartDirPath(src) && isPartDirPath(dst))
    {
        moveDirectory(from, to);
        return;
    }
    if (!src || src->file.empty() || !dst || dst->file.empty())
    {
        /// Table-level / generic verbatim path: delegate to moveFile (its non-part branch does a
        /// copyObject(Rewrite)+remove, which already overwrites the destination).
        moveFile(from, to);
        return;
    }
    rememberTarget(to);

    /// Re-key from the source part's entry to the destination part's entry. Same-part rename (common) →
    /// both resolve to the SAME entry; cross-part → move the file between the two entries.
    auto & src_st = stagingForPath(*src);
    auto & dst_st = stagingForPath(*dst);

    /// Overwrite: drop any staged destination entry first.
    dst_st.recorded.erase(dst->file);
    dst_st.recorded_mutable.erase(dst->file);
    dst_st.recorded_mutable_removed.erase(dst->file);
    /// Common case — source staged inline (txn_version.txt.tmp recognized as mutable by Piece 1): move bytes.
    if (auto mit = src_st.recorded_mutable.find(src->file); mit != src_st.recorded_mutable.end())
    {
        dst_st.recorded_mutable[dst->file] = std::move(mit->second);
        src_st.recorded_mutable.erase(mit);
        return;
    }
    /// Source staged as a content blob but destination is mutable: should be rare after Piece 1. Read the
    /// just-written bytes back inline (the orphan tmp blob is GC-reclaimed). Otherwise re-key the blob.
    if (auto it = src_st.recorded.find(src->file); it != src_st.recorded.end())
    {
        if (ContentAddressed::isMutablePerPartFile(dst->file))
        {
            dst_st.recorded_mutable[dst->file] = readStagedOrCommittedBytes(from);
            src_st.recorded.erase(it);
        }
        else
        {
            dst_st.recorded[dst->file] = std::move(it->second);
            src_st.recorded.erase(it);
        }
        return;
    }
    /// Source not staged in THIS transaction (standalone autocommit across ops): resolve the committed
    /// source bytes from the sidecar and re-stage under the destination, marking the source removed.
    dst_st.recorded_mutable[dst->file] = metadata_storage.resolveMutableFileBytes(from);
    src_st.recorded_mutable_removed.insert(src->file);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool, bool)
{
    /// Part file: drop the staged blob so it is excluded from the manifest. The underlying content
    /// blob, if shared, is reclaimed by GC, not here.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->file.empty())
    {
        auto & st = stagingForPath(*p);
        const bool staged_here = st.recorded.contains(p->file) || st.recorded_mutable.contains(p->file);
        st.recorded.erase(p->file);
        st.recorded_mutable.erase(p->file);
        /// A mutable per-part file that exists only in the already-committed sidecar (not staged in THIS
        /// transaction) must be deleted from that sidecar at commit — the mutable-only commit branch reads
        /// recorded_mutable_removed to do so. This handles removeTmpMetadataFile's
        /// removeFile(txn_version.txt.tmp) on a committed part (the .tmp is recognized as mutable by Piece 1).
        if (!staged_here && ContentAddressed::isMutablePerPartFile(p->file))
        {
            /// Pin the (table_uuid, part_name) target so a standalone remove of a mutable per-part file
            /// (e.g. removeTmpMetadataFile calling removeFile(txn_version.txt.tmp) with no other staged
            /// operations) sets the target that the mutable-only commit branch requires.
            rememberTarget(path);
            stagingForPath(*p).recorded_mutable_removed.insert(p->file);
        }
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

uint64_t ContentAddressedTransaction::readActiveGenHint(const std::string & active_key) const
{
    /// Read a best-effort `active` generation hint (default 0 if absent or unparseable). Used on the
    /// read-only DROP path: a drop resolves the generation its `+` settled on but NEVER resurrects (it is
    /// removing a reference, not attaching one). A stale/absent hint resolves to 0 — the common case.
    auto bytes = metadata_storage.readSmallObjectIfExists(active_key);
    if (!bytes || bytes->empty())
        return 0;
    uint64_t parsed = 0;
    for (char c : *bytes)
    {
        if (c < '0' || c > '9')
            return 0;
        parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
    }
    return parsed;
}

uint64_t ContentAddressedTransaction::resolveAndResurrectGeneration(
    const std::string & id_for_log,
    const std::function<std::string(uint64_t)> & make_gen_key,
    const std::function<std::string(uint64_t)> & make_tomb_key,
    const std::function<std::string()> & make_active_key)
{
    auto & object_storage = *metadata_storage.object_storage;

    /// Resolve the current generation via the best-effort `active` hint (default 0 — absent in the common
    /// case). `active` is a plain hint, never authoritative: a stale value only costs an extra re-check.
    uint64_t g = 0;
    if (auto active_bytes = metadata_storage.readSmallObjectIfExists(make_active_key()))
    {
        uint64_t parsed = 0;
        bool ok = !active_bytes->empty();
        for (char c : *active_bytes)
        {
            if (c < '0' || c > '9')
            {
                ok = false;
                break;
            }
            parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
        }
        if (ok)
            g = parsed;
    }

    /// Bounded re-check + resurrect loop (§6, §7.1 steps 4-5). Each iteration: ensure g exists, re-check its
    /// tombstone; if SEALED, resurrect to g+1 and retry. The loop is correct and terminating at ANY cap: it
    /// advances ONLY when it observes a present tombstone (a GC-owned seal), creates an unsealed successor at
    /// g+1, and settles the moment it reaches a generation with no tombstone. The cap is therefore purely a
    /// backstop against a pathological UNBOUNDED sealed chain — not a correctness bound.
    ///
    /// We size it at 256 (was 8) for headroom against transient GC seal bursts: the GC writes a permanent
    /// `.tombstone` gravestone per sweep round, so under a fast GC cadence (e.g. the 5s interval the
    /// content-addressed stateless tests run with) a hot content identity can accrue sealed generations at
    /// roughly one per round. A short cap turned such a transient burst into a user-visible `CORRUPTED_DATA`
    /// failure on an otherwise healthy commit; 256 absorbs the burst while still bounding a truly runaway
    /// chain. (The principled cure — an in-flight manifest pin so a seal cannot race a live commit — is a
    /// separate backlog item.)
    constexpr size_t max_iterations = 256;
    for (size_t iter = 0; iter < max_iterations; ++iter)
    {
        const std::string gen_key = make_gen_key(g);

        /// RE-CHECK the tombstone for this exact generation FIRST. The seal is GC-owned; its presence means
        /// "no new attachment may target g" (I4). We never delete it, never rescue g. The TOMBSTONE — not the
        /// object's presence — is the only condemnation signal (§6, §12.1): a generation is condemned iff it
        /// is sealed. SWEEP keeps the `<g>.tombstone` gravestone forever, so a swept (and therefore absent)
        /// generation is still `sealed` here and routes to resurrection; an absent-but-UNSEALED generation
        /// was never condemned and is simply a not-yet-created object (the fresh g=0 blob whose upload the
        /// write buffer already did, or the manifest this commit is about to condCreate at g).
        const bool sealed
            = object_storage.tryGetObjectMetadata(make_tomb_key(g), /*with_tags=*/false).has_value();

        if (!sealed)
            return g; /// not condemned — attach to / create this generation (fresh OR present, both safe).

        /// Sealed: confirm whether the sealed object is still present (only to source the resurrected bytes
        /// from it when it survives during grace; a swept generation is absent but its gravestone routes us
        /// on to g+1 regardless).
        const bool present = object_storage.tryGetObjectMetadata(gen_key, /*with_tags=*/false).has_value();

        /// Abandon g, resurrect to g+1 (§6). condCreateIfAbsent the g+1 object so concurrent resurrectors
        /// collapse onto one object (I5). Its bytes are the byte-identical content of the present (sealed)
        /// generation, which the GC keeps until sweep — read it and create g+1. If the present generation
        /// is itself gone (already swept), fall through to the next higher generation the next iteration
        /// (its tombstone/active will route us there).
        const uint64_t next = g + 1;
        const std::string next_key = make_gen_key(next);
        /// §13 observability: a resurrection happened (a sealed generation forced us to advance to g+1).
        /// Counted once per advance, regardless of whether THIS caller wins the condCreate race for the new
        /// object (a high rate flags hot content cycling zero-refs→resurrection — gravestones are safe but
        /// not free).
        ProfileEvents::increment(ProfileEvents::ContentAddressedGenerationResurrectionsTotal);
        if (!object_storage.tryGetObjectMetadata(next_key, /*with_tags=*/false).has_value())
        {
            /// Source the bytes from whichever generation is still present (prefer the just-checked g).
            std::optional<std::string> content;
            if (present)
                content = metadata_storage.readSmallObjectIfExists(gen_key);
            if (content)
            {
                /// The resurrected object is a byte-identical DUPLICATE of the condemned predecessor (kept by
                /// the GC until it drains) — track the duplicate bytes only when WE create it (the winner).
                if (ContentAddressed::condCreateIfAbsent(object_storage, next_key, *content))
                    ProfileEvents::increment(ProfileEvents::ContentAddressedDuplicateGenerationBytes, content->size());
            }
        }

        /// Best-effort advance `active → g+1` (a plain PUT, NOT a CAS — G4/I7d). A failure only costs the
        /// next writer/reader an extra re-check/fallback.
        try
        {
            const std::string bytes = std::to_string(next);
            auto out = object_storage.writeObject(StoredObject(make_active_key()), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size());
            out->finalize();
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }

        g = next;
    }

    throw Exception(
        ErrorCodes::CORRUPTED_DATA,
        "ContentAddressed: generation resurrection for {} exceeded {} iterations (hot content cycling "
        "zero-refs→resurrection); retry the operation",
        id_for_log, max_iterations);
}

void ContentAddressedTransaction::commitOnePart(const PartKey & key, PartStaging & st, std::vector<std::pair<ContentAddressed::ShardId, uint64_t>> & settled_delta_epochs)
{
    const std::string & table_uuid_ = key.first;
    const std::string & part_name_ = key.second;

    /// CA GC S4 (G1) — the held-across-commit `gc_lock` is gone, but the SHARED "detached" ref still needs
    /// COMMIT-vs-COMMIT serialization (B66): a FETCH PARTITION downloads parts concurrently and each part's
    /// commit does a read-merge-publish into the ONE `detached` ref; without serialization two commits read
    /// the same prior manifest and the second overwrites the first's contribution (lost parts). This is a
    /// concurrent-writer-to-a-shared-mutable-object problem, NOT the commit-vs-sweep handshake, so the §7
    /// proof does not cover it. Re-use the per-pool in-process mutex (now the narrow container guard) as the
    /// detached-ref serializer, scoped to THIS commit's read-merge-publish — a regular part_name is never
    /// re-committed with new content under the same name, so only "detached" needs it. The sweep no longer
    /// takes this lock, so detached commits serialize only among themselves, never against the GC.
    std::optional<std::lock_guard<std::mutex>> detached_ref_guard;
    if (part_name_ == ContentAddressed::kDetachedDirName && metadata_storage.gc_lock)
        detached_ref_guard.emplace(*metadata_storage.gc_lock);

    /// A part entry with ALL-empty staging is a no-op (a part that was remembered but never written, e.g.
    /// adopted as a tmp rename target with nothing staged under it).
    if (st.recorded.empty() && st.recorded_mutable.empty() && st.recorded_mutable_removed.empty())
        return;

    /// Mutable-only update of an ALREADY-COMMITTED part: this transaction staged NO content blobs, only
    /// mutable per-part files (txn_version.txt and/or removals) — the MVCC creation-CSN fill-in and
    /// removal-TID lock/unlock rewrite txn_version.txt on a LIVE part. Update only the per-ref sidecar +
    /// the per-file mutable objects in place; KEEP the existing manifest, part_id and ref. The normal path
    /// below would compute a part_id over an empty manifest and republish the ref — clobbering the part.
    if (st.recorded.empty() && (!st.recorded_mutable.empty() || !st.recorded_mutable_removed.empty()))
    {
        auto existing_pid = metadata_storage.readRefPartId(table_uuid_, part_name_);
        if (!existing_pid)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "ContentAddressed: mutable-only commit for {}/{} with no existing ref", table_uuid_, part_name_);

        ContentAddressed::RefSidecar sidecar;
        /// CA GC S3 (#6): carry forward manifest_generation + pin_generations from the prior sidecar so
        /// a mutable-only update does not erase the generations a prior content publish recorded. The full
        /// struct copy (sidecar = *existing) preserves all generation fields; only sidecar.files is patched.
        if (auto existing = metadata_storage.readRefSidecarIfExists(table_uuid_, part_name_))
            sidecar = *existing;
        for (const auto & f : st.recorded_mutable_removed)
        {
            sidecar.files.erase(f);
            metadata_storage.object_storage->removeObjectIfExists(StoredObject(
                ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_, f).string()));
        }
        for (const auto & [file, bytes] : st.recorded_mutable)
        {
            sidecar.files[file] = bytes;
            const std::string fk = ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_, file).string();
            auto out = metadata_storage.object_storage->writeObject(StoredObject(fk), WriteMode::Rewrite);
            out->write(bytes.data(), bytes.size());
            out->finalize();
        }
        const std::string meta_key = ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_).string();
        const std::string meta_bytes = sidecar.serialize();
        auto mo = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        mo->write(meta_bytes.data(), meta_bytes.size());
        mo->finalize();
        return;
    }

    /// Partition: content-identical files go to the shared manifest (and dedup); the mutable per-part
    /// files (recorded_mutable) go to this part's PRIVATE per-ref sidecar, never the manifest. The
    /// manifest holds only content state, so two parts with identical content share one manifest while
    /// each keeps its own uuid / txn / metadata version (B23).
    /// NOTE: frozen targets (FREEZE, shadow/<backup>/…) are always a real part_name (never "detached"),
    /// so the two kDetachedDirName manifest-merge + sidecar-merge branches below are skipped for them.
    ContentAddressed::PartManifest manifest;
    manifest.blobs = st.recorded;

    /// The "detached" ref is a SHARED container of detached part directories: each detach/FETCH lands one
    /// part into detached/<detached_part>/ via its own transaction whose recorded keys are
    /// <detached_part>/<file> (B36). Several parts land independently and must COEXIST under the one
    /// "detached" ref. The default publish below rewrites the ref, which would make each publish overwrite
    /// the previous one (only the last detached part would be listed — B46). So when the target is the
    /// detached namespace, merge the new keys into the existing detached ref's manifest first. This
    /// read-modify-write of the SHARED detached ref happens UNDER `detached_ref_guard` (taken at the top of
    /// commitOnePart for the detached case — CA GC S4 G1): a FETCH PARTITION downloads its parts
    /// concurrently (the FETCH thread pool, 03350) and each part's commit publishes into the same "detached"
    /// ref, so an unlocked read-merge-publish loses entries — two concurrent commits both read the same
    /// prior manifest and the second overwrites the first's contribution, leaving only one part's blobs and
    /// a FILE_DOESNT_EXIST on the lost parts (B66). The guard serializes the read-merge-publish so every
    /// part accumulates. (A regular part_name is never re-committed with new content under the same name, so
    /// this only affects "detached".)
    if (part_name_ == ContentAddressed::kDetachedDirName)
    {
        if (auto existing_pid = metadata_storage.readRefPartId(table_uuid_, part_name_))
        {
            auto existing = metadata_storage.loadPartManifestOrThrow(*existing_pid);
            for (const auto & [file, entry] : existing.blobs)
                manifest.blobs.emplace(file, entry); /// keep the new entry on a key collision (re-land)
        }
    }

    const ContentAddressed::PartId part_id = ContentAddressed::computePartId(manifest.blobs);

    /// CA GC S3: resolve every referenced blob's GENERATION and RE-CHECK its tombstone before publishing
    /// (spec §6, §7.1 steps 4-5). This generalizes the B49 re-validate: resolve (H → g) per blob via its
    /// `active` hint (default 0), confirm a present, non-tombstoned generation, and — if the resolved
    /// generation has been SEALED — resurrect to g+1 (a different physical key, ABA-proof: I4). The held
    /// gc_lock still makes the live set stable for the common case (a fresh g=0 blob the sweep cannot have
    /// sealed); resurrection is the carrier the S4 lockless handshake will rely on. The resolved generation
    /// is captured per pin so the `+` delta records the exact (H,g) the manifest now references. The
    /// manifest body still pins BARE H (part_id is content-only), so dedup/idempotency are unchanged.
    std::map<ContentAddressed::BlobHash, uint64_t> resolved_blob_gen;
    for (const auto & [file, entry] : manifest.blobs)
    {
        if (resolved_blob_gen.contains(entry.key))
            continue; /// two files can reference the same blob — resolve each unique hash once.
        const uint64_t g = resolveAndResurrectGeneration(
            entry.key.string(),
            [&](uint64_t gen) { return ContentAddressed::blobGenKey(key_prefix, entry.key, gen).string(); },
            [&](uint64_t gen) { return ContentAddressed::blobTombstoneKey(key_prefix, entry.key, gen).string(); },
            [&]() { return ContentAddressed::blobActiveKey(key_prefix, entry.key); });
        resolved_blob_gen.emplace(entry.key, g);
    }

    /// Resolve the manifest GENERATION mg the same way (§9, symmetric to blobs): the manifest object lives
    /// at parts/<part_id>/<mg>. A freshly-created manifest is mg=0; a relink-after-full-drop re-creation
    /// routes to mg+1 if mg=0 was sealed (the ABA hole the bare-key delete had). Put-if-absent the manifest
    /// at the resolved generation: identical parts (same deterministic blobs → same part_id) still share
    /// ONE manifest object at that generation (dedup unchanged).
    const uint64_t manifest_gen = resolveAndResurrectGeneration(
        part_id.string(),
        [&](uint64_t gen) { return ContentAddressed::partGenKey(key_prefix, part_id, gen).string(); },
        [&](uint64_t gen) { return ContentAddressed::partTombstoneKey(key_prefix, part_id, gen).string(); },
        [&]() { return ContentAddressed::partActiveKey(key_prefix, part_id); });
    const std::string part_key = ContentAddressed::partGenKey(key_prefix, part_id, manifest_gen).string();
    if (!metadata_storage.object_storage->tryGetObjectMetadata(part_key, /*with_tags=*/false).has_value())
    {
        /// condCreateIfAbsent so concurrent creators of the same (part_id, mg) collapse onto one object (I5).
        ContentAddressed::condCreateIfAbsent(*metadata_storage.object_storage, part_key, manifest.serialize());
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
    std::map<std::string, std::string> merged_mutable(st.recorded_mutable.begin(), st.recorded_mutable.end());
    if (part_name_ == ContentAddressed::kDetachedDirName)
    {
        if (auto existing = metadata_storage.readRefSidecarIfExists(table_uuid_, part_name_))
            for (const auto & [file, bytes] : existing->files)
                merged_mutable.emplace(file, bytes); /// keep the new bytes on a key collision (re-detach)
    }

    /// FREEZE publishes into the shadow/<backup>/ namespace (one ref per frozen part); a live part uses
    /// the store/.../refs/ location. Select the ref-family keys accordingly.
    const bool is_frozen = !st.frozen_backup_name.empty();
    auto mutable_file_key = [&](const std::string & file)
    {
        return is_frozen
            ? ContentAddressed::shadowRefMutableFileKey(key_prefix, st.frozen_table_dir, part_name_, file).string()
            : ContentAddressed::refMutableFileKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_, file).string();
    };
    const std::string meta_key = is_frozen
        ? ContentAddressed::shadowRefMetaKey(key_prefix, st.frozen_table_dir, part_name_).string()
        : ContentAddressed::refMetaKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_).string();

    if (!merged_mutable.empty())
    {
        /// Per-file objects FIRST: each mutable file's bytes verbatim in its own tiny object so the
        /// read path (getStorageObjects -> readObject) returns exactly that file's bytes. Only this
        /// detach's NEW files need writing; prior detached parts' per-file objects already exist.
        for (const auto & [file, bytes] : st.recorded_mutable)
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
        /// CA GC S3 (#6): record the resolved generations the `+` settled on, so the DROP path emits its
        /// `-` at the matching generation (not the racy `active` hint). resolved_blob_gen / manifest_gen
        /// are the same values threaded into the `+` delta above.
        sidecar.manifest_generation = manifest_gen;
        for (const auto & [hash, g] : resolved_blob_gen)
            sidecar.pin_generations.emplace(hash.string(), g);
        const std::string meta_bytes = sidecar.serialize();
        auto meta_out = metadata_storage.object_storage->writeObject(StoredObject(meta_key), WriteMode::Rewrite);
        meta_out->write(meta_bytes.data(), meta_bytes.size());
        meta_out->finalize();
    }

    /// CA GC S4 (§7.1 step 6) — enqueue the `+` GC delta AFTER the tomb re-check / resurrection above (so an
    /// abandoned generation never leaves a stale `+`) and BEFORE the live ref. The `+` carries the RESOLVED
    /// (part_id, mg) and (H, g). The I1/I6 ordering (`+` before ref) is preserved by the synchronous flush —
    /// *ref exists ⇒ delta enqueued* — so a fold can only over-count (reconciled), never under-count a live
    /// blob. In S4 the durable WriteSession (raised FIRST, before any upload — §7.1 step 2, the handshake
    /// flag) ALSO covers the reference across the `+`-before-fold gap (§5.1 rule 3): the session is retained
    /// until every `+` delta is folded, so `sessions ∪ folded-snapshot` covers every live reference even
    /// before the `+` lands in a snapshot. The settled `(shard, epoch)` of each fragment is captured into
    /// `settled_delta_epochs` so commit() records it in the session for the folded-watermark reaper. (Lock
    /// still held in this phase — purely the §7.1 ordering + the session-until-folded lifetime; the lock
    /// removal is the next phase.) Scope it to a LIVE regular part for the same reason the S1 reverse-index
    /// update is so scoped (a FREEZE/shadow ref and the shared "detached" ref pin blobs but re-key their
    /// part_id and are out of the count model — reconciliation covers them). The append is wrapped so an
    /// exception is logged + swallowed, but the flush DOES precede the ref so the ordering is preserved.
    if (!is_frozen && part_name_ != ContentAddressed::kDetachedDirName)
    {
        try
        {
            ContentAddressed::GcDelta delta;
            delta.op = ContentAddressed::GcDelta::Op::Add;
            delta.part_id = part_id;
            /// CA GC S3: thread the RESOLVED (part_id, mg) and (H, g) into the `+` delta — the generation
            /// discriminator the S2 codec reserved. The manifest generation is folded into the event_id so
            /// two re-appends of the SAME resolved delta still dedup; the per-pin generations are recorded
            /// parallel to the pins. The delta is logged AFTER the tomb re-check / resurrection above, so an
            /// abandoned generation never leaves a stale `+` (§7.1 step 4 ordering).
            delta.manifest_generation = manifest_gen;
            delta.event_id = ContentAddressed::GcDelta::computeEventId(part_id, delta.op, manifest_gen);
            /// De-duplicate the pins by hash (a blob referenced by several files is one pinned (H,g)).
            std::set<ContentAddressed::BlobHash> seen;
            delta.pins.reserve(resolved_blob_gen.size());
            delta.pin_generations.reserve(resolved_blob_gen.size());
            for (const auto & [file, entry] : manifest.blobs)
            {
                if (!seen.insert(entry.key).second)
                    continue;
                delta.pins.push_back(entry.key);
                delta.pin_generations.push_back(resolved_blob_gen.at(entry.key));
            }
            try
            {
                /// CA GC S4: capture the `(shard, epoch)` each fragment settled in (after the §5.1 rule-2
                /// re-append) so the session-until-folded reaper can gate this commit's session on the folded
                /// watermark of exactly those epochs.
                for (auto & shard_epoch : metadata_storage.gcLogWriter()->appendAndFlushForCommit(delta))
                    settled_delta_epochs.push_back(shard_epoch);
            }
            catch (...)
            {
                /// FAIL-CLOSED (#2): the ref is about to be published but the `+` did not land durably.
                /// Accumulate the delta so commit() makes the session STICKY (retained, lease-exempt),
                /// carrying ALL failed parts' `+` deltas as one batch. The GC reaper re-logs each. Do NOT
                /// serialize here — a per-part slot would drop all but the last failure in a multi-part
                /// transaction (B67 merge), and an OOM in serialize would escape to the outer catch.
                failed_add_deltas.push_back(delta);
                tryLogCurrentException(
                    getLogger("ContentAddressedTransaction"),
                    "CA GC S4 (#2): + flush failed for part " + part_id.string()
                        + " — session will be retained sticky and the + re-logged by the GC reaper");
            }
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("ContentAddressedTransaction"),
                "CA GC S2: failed to build the + delta for part " + part_id.string());
        }
    }

    // Publish the ref last: store/{server_id}/{table_uuid}/refs/{part_name} for a live part, or
    // shadow/<backup>/{server_id}/{table_uuid}/refs/{part_name} for a FREEZE target. The on-disk payload
    // is the versioned ref-payload struct (MAGIC+version+part_id) written by serializeRefPayload and
    // parsed by the single partIdFromRefPayload shared with the read path and the GC live-set scan (B28).
    const std::string ref_key = is_frozen
        ? ContentAddressed::shadowRefKey(key_prefix, st.frozen_table_dir, part_name_).string()
        : ContentAddressed::refKey(key_prefix, metadata_storage.server_id, table_uuid_, part_name_).string();
    const std::string ref_payload = ContentAddressed::serializeRefPayload(part_id);
    auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
    ref_out->write(ref_payload.data(), ref_payload.size());
    ref_out->finalize();

    /// CA GC S1 (B9): record this part's blob pins in the per-pool incremental reverse index. This is
    /// the live-ref CONTENT publish branch (a mutable-only sidecar update returned far above and pins no
    /// new blobs). Scope it to a LIVE regular part: a FREEZE/shadow ref and the shared "detached" ref
    /// also pin blobs but are out of S1 scope (the sweep's drift log notes them as a known under-count;
    /// the detached ref also re-keys its part_id on each merge, which would mis-track here). The update
    /// is idempotent (applied_parts) and INSTRUMENTATION ONLY: the sweep's authoritative full-scan still
    /// drives every deletion, so an exception here must NEVER break the commit — log and swallow it.
    if (!is_frozen && part_name_ != ContentAddressed::kDetachedDirName)
    {
        try
        {
            metadata_storage.blobRefIndex()->addPart(part_id, manifest);
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("ContentAddressedTransaction"),
                "CA GC S1: failed to add part " + part_id.string() + " to the reverse index (instrumentation only; "
                "the authoritative GC scan is unaffected)");
        }
    }
}

void ContentAddressedTransaction::commit(const TransactionCommitOptionsVariant & options)
{
    if (!std::holds_alternative<NoCommitOptions>(options))
        throwNotImplemented();

    /// Nothing was staged (e.g. a directory-only transaction): no part to publish.
    if (parts.empty())
        return;

    /// CA GC S4 (§7.1) — the writer commit follows the EXACT handshake order: the durable WriteSession is
    /// the SYNCHRONOUS REFERENCE FLAG (written FIRST, before any upload), then per part: upload missing
    /// blobs+manifest, RE-CHECK every pinned generation's `.tombstone` and resurrect on a seal, enqueue the
    /// `+` (AFTER the re-check, so an abandoned generation leaves no stale `+`), and publish the LIVE REF
    /// LAST (the commit point). The session is the flag — NOT the `+`, NOT the visible ref — so in the
    /// lockless phase the writer's re-check can precede the `+`: the session was already raised (durable,
    /// GC-visible via `sessionPinnedBlobs`/`sessionPinnedPartKeys`) before either, so a concurrent GC's
    /// §6.2 re-check observes it and skips the blobs. It is then kept UNTIL FOLDED (Task 1).
    ///
    /// STEP 2 — raise/renew the flag FRESHLY before the publish sequence: the per-blob renewals kept it live
    /// during the write, but it must not lapse before the refs are published. A live session root covers
    /// every freshly-written blob this transaction's parts are about to reference; a REMOTE mounter's sweep
    /// re-reads sessions in its re-validate step immediately before deleting, so a live session makes it
    /// skip these blobs. (Carried-forward blobs are instead covered by their source part's ref.) Renew
    /// before taking the in-process lock since this is a bucket write coordinating with OTHER mounters.
    /// CA GC S4 (G1) — the per-pool GC lock is NO LONGER held across the multi-part publish. The §7
    /// handshake is the sole cross-process commit-vs-sweep gate: the session pin (raised above, the §7
    /// proof's flag `A`) is durable and GC-visible BEFORE the per-part tomb re-check and the `+` below, and
    /// the GC seals `.tombstone` (flag `M`) BEFORE its fresh §6.2 re-check that reads the live session set.
    /// The lock survives ONLY as the narrow container guard for `in_flight_pinned_blobs` (taken around the
    /// per-blob pin insert in `decide_and_pin` and around the pin-erase loop below) — it no longer
    /// serializes commit against the whole sweep.
    ///
    /// Re-assert the flag is durable AT the §7.1 step-2 point, BEFORE any per-part upload / recheck / `+` /
    /// ref below. This is the store of the §7 proof's flag `A` (the session pin) that must PRECEDE both the
    /// writer's own tomb re-check and GC's §6.2 load.
    if (session_open)
        persistSession();

    /// STEPS 3–7, per part (§7.1): upload (manifest condCreate) -> RE-CHECK tomb + resurrect -> enqueue the
    /// `+` (after the re-check) -> publish the LIVE REF LAST. A single transaction may write more than one
    /// part (a transactional merge — B67); an all-empty entry is a no-op (handled inside commitOnePart).
    /// CA GC S4: each part's `+`-delta settled `(shard, epoch)` is collected so the session can be retained
    /// until folded (Task 1 / step 8).
    std::vector<std::pair<ContentAddressed::ShardId, uint64_t>> settled_delta_epochs;
    for (auto & [key, st] : parts)
        commitOnePart(key, st, settled_delta_epochs);

    /// B52: the refs now name these parts and keep every referenced blob reachable, so the in-flight pins
    /// are no longer needed. CA GC S4 (G1): the refs are ALREADY published above (the live ref is the
    /// commit point), so a blob this commit reused is now reachable via its ref; dropping the in-process
    /// pin here can only ever release a now-referenced blob. Take the NARROW container guard just around the
    /// erase (the same mutex `decide_and_pin` / `unpinBlob` take) so the `std::set` mutation does not race a
    /// concurrent sweep's snapshot read. Clear the local set so the destructor's release is a no-op.
    {
        std::lock_guard<std::mutex> pin_guard(*metadata_storage.gc_lock);
        for (const auto & blob_key : pinned_blob_keys)
            metadata_storage.inFlightPinnedBlobs()->erase(blob_key);
    }
    pinned_blob_keys.clear();

    /// CA GC S4 (§5.1 rule 3, §7.3) — session lifetime = UNTIL FOLDED, not until commit. The refs are now
    /// published, but the `+` deltas covering them may not yet be folded into a durable snapshot. Until they
    /// are, *sessions ∪ folded-snapshot* must still cover every live reference (the §6.2 gate's completeness
    /// premise), so we MUST NOT drop the session at commit. Instead: mark the session COMMITTED, record the
    /// `+` deltas' settled `(shard, epoch)`, and re-persist it (under the lock window's renewed lease). The
    /// session-until-folded reaper (in the GC sweep, gated on the folded watermark) deletes it once every
    /// recorded epoch is folded. We do a cheap POST-COMMIT folded check here too: if every settled epoch is
    /// ALREADY folded (the common case where a recent fold has overtaken this commit), we can drop the
    /// session immediately; otherwise it lingers for the reaper. The commit NEVER blocks on folding.
    if (session_open)
    {
        session.committed = true;
        session.delta_epochs = settled_delta_epochs;
        /// CA GC S4 (#2): if one or more parts' `+` flush threw, the refs are published but those `+` are
        /// not durable. Mark the session STICKY (retained, lease-exempt), carrying ALL the failed `+`
        /// deltas as one batch, so neither the lease reaper nor the folded reaper drops it; the GC reaper
        /// re-logs `pending_add_delta` and clears sticky once the re-logged `+` are folded. NEVER release
        /// the session in this state.
        if (!failed_add_deltas.empty())
        {
            session.deltas_failed = true;
            session.pending_add_delta = ContentAddressed::serializeGcDeltasForSession(failed_add_deltas);
            persistSession();
            return; /// fail-closed: keep the sticky session; do not run the folded-release below.
        }
        persistSession();

        if (allSettledEpochsFolded(settled_delta_epochs))
            releaseSession(); /// already folded — the snapshot covers the reference, drop the pin now.
        /// else: leave the durable committed session for the folded-watermark reaper (do NOT releaseSession;
        /// the destructor also keeps a committed session — see ~ContentAddressedTransaction).
    }
}

bool ContentAddressedTransaction::allSettledEpochsFolded(
    const std::vector<std::pair<ContentAddressed::ShardId, uint64_t>> & settled_delta_epochs) const
{
    /// CA GC S4 — the post-commit folded check (§5.1 rule 3): true iff EVERY `(shard, epoch)` this commit's
    /// `+` deltas settled in is folded into a durable snapshot (`GcCompaction::isEpochFolded`). A delta-less
    /// commit (empty set) is trivially folded. A throw is treated conservatively as "not folded" so the
    /// session lingers for the reaper rather than being dropped early.
    if (settled_delta_epochs.empty())
        return true;
    try
    {
        ContentAddressed::GcCompaction compaction(metadata_storage.object_storage, key_prefix);
        for (const auto & [shard, epoch] : settled_delta_epochs)
            if (!compaction.isEpochFolded(shard, epoch))
                return false;
        return true;
    }
    catch (...)
    {
        return false; /// conservative: keep the session (it is reaped later once provably folded).
    }
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

    /// CA GC S1 (B9): mirror the commit-side addPart by removing this part's blob pins from the per-pool
    /// reverse index as its live ref is unlinked. Resolve the (part_id, manifest) FROM the ref BEFORE the
    /// deletion below (the ref/manifest still exist now). INSTRUMENTATION ONLY: the sweep's authoritative
    /// full-scan still drives every deletion, so an exception here must NEVER block the drop — log and
    /// continue (the scan stays authoritative; a missed removePart only makes the index over-count, which
    /// the drift log surfaces and the scan corrects). removePart is idempotent (applied_parts), so a part
    /// the index never saw (committed by another process, or before this process started) is a no-op.
    /// CA GC S2: resolve the (part_id, manifest) FROM the ref BEFORE the deletion below (the ref/manifest
    /// still exist now) so the `-` delta can carry the resolved pins + the (part_id) edge. The `-` is
    /// appended AFTER the ref removal (§7.1 unlink ordering, bias to over-count): a crash between the
    /// removal and the `-` leaves a not-live part whose blob is briefly over-counted (safe, reconciled),
    /// never an under-count that could strand a delete.
    std::optional<ContentAddressed::PartId> dropped_part_id;
    std::optional<ContentAddressed::PartManifest> dropped_manifest;
    std::optional<ContentAddressed::RefSidecar> dropped_sidecar;
    try
    {
        if (auto part_id = metadata_storage.readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = metadata_storage.loadPartManifestOrThrow(*part_id);
            metadata_storage.blobRefIndex()->removePart(*part_id, manifest);
            dropped_part_id = part_id;
            dropped_manifest = std::move(manifest);
            /// CA GC S3 (#6): read the per-part generations the `+` settled on, so the `-` is keyed to match.
            dropped_sidecar = metadata_storage.readRefSidecarIfExists(p->table_uuid, p->part_name);
        }
    }
    catch (...)
    {
        tryLogCurrentException(
            getLogger("ContentAddressedTransaction"),
            "CA GC S1: failed to remove part " + p->part_name + " from the reverse index (instrumentation only; "
            "the authoritative GC scan is unaffected)");
    }

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

    /// CA GC S2: append the `-` delta AFTER the ref removal (bias to over-count, §7.1). Wrapped so an
    /// exception is logged + swallowed — the authoritative scan is unaffected for S2. The drop side is
    /// not on the I1/I6 critical ordering (that is the commit's `+`-before-ref), so a flushAll here is a
    /// best-effort durability nudge, not a correctness requirement.
    if (dropped_part_id)
    {
        try
        {
            ContentAddressed::GcDelta delta;
            delta.op = ContentAddressed::GcDelta::Op::Remove;
            delta.part_id = *dropped_part_id;
            /// CA GC S3 (#6): key the `-` at the generation the `+` SETTLED on, recorded in the sidecar at
            /// commit. A resurrection between commit and drop changes `active`, so re-deriving from the hint
            /// would mis-key the `-` and leave the old generation's count >0 forever. Fall back to g=0 (the
            /// `active` hint, the legacy/common case) only when the sidecar is absent.
            delta.manifest_generation = dropped_sidecar
                ? dropped_sidecar->manifest_generation
                : readActiveGenHint(ContentAddressed::partActiveKey(key_prefix, *dropped_part_id));
            delta.event_id
                = ContentAddressed::GcDelta::computeEventId(*dropped_part_id, delta.op, delta.manifest_generation);
            std::set<ContentAddressed::BlobHash> seen;
            delta.pins.reserve(dropped_manifest->blobs.size());
            delta.pin_generations.reserve(dropped_manifest->blobs.size());
            for (const auto & [file, entry] : dropped_manifest->blobs)
            {
                if (!seen.insert(entry.key).second)
                    continue;
                delta.pins.push_back(entry.key);
                uint64_t g = 0;
                if (dropped_sidecar)
                {
                    if (auto it = dropped_sidecar->pin_generations.find(entry.key.string());
                        it != dropped_sidecar->pin_generations.end())
                        g = it->second;
                }
                else
                    g = readActiveGenHint(ContentAddressed::blobActiveKey(key_prefix, entry.key));
                delta.pin_generations.push_back(g);
            }
            metadata_storage.gcLogWriter()->enqueue(delta);
            metadata_storage.gcLogWriter()->flushAll();
        }
        catch (...)
        {
            tryLogCurrentException(
                getLogger("ContentAddressedTransaction"),
                "CA GC S2: failed to append the - delta for part " + p->part_name + " to gc/log "
                "(the authoritative GC scan is unaffected for S2)");
        }
    }
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
