#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/GcDelta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/WriteSession.h>
#include <Disks/WriteMode.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteSettings.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DB
{

// Transaction for the content-addressed metadata storage.
//
// As part files are written, each ContentAddressedWriteBuffer reports its content hash back
// through a finalize callback, and the transaction accumulates a (logical_file -> BlobEntry) map.
// Carried-forward files (mutations / hardlinks) copy their source BlobEntry without re-uploading.
// At commit the transaction derives the part_id from the accumulated blobs, writes the
// parts/<part_id> manifest (put-if-absent) and publishes the ref.
class ContentAddressedTransaction : public IMetadataTransaction
{
protected:
    ContentAddressedMetadataStorage & metadata_storage;

public:
    /// key_prefix_ is the object-storage common key prefix, taken from the metadata storage
    /// (its storage_path_prefix) so the write side keys objects exactly where the read side looks.
    /// Never re-derive it from object_storage->getCommonKeyPrefix() here: a config
    /// key_compatibility_prefix override would otherwise make read and write disagree.
    ///
    /// local_scratch_path_ is a real server-local filesystem directory where the write buffer spills
    /// each part file while hashing it, before upload. It is a local path, NOT an object key prefix:
    /// for a remote object storage (e.g. s3) the key prefix is not a usable local path.
    ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_, std::string key_prefix_, std::string local_scratch_path_);

    /// B52: release any still-held in-flight blob pins. A transaction that was never committed (an
    /// aborted/cancelled insert) would otherwise pin its staged blobs forever, defeating GC.
    ~ContentAddressedTransaction() override;

    bool supportsChmod() const override { return false; }

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

    // B59 in-flight read-your-writes: resolve a part file this transaction has STAGED (uploaded blob in
    // `recorded`, or inline mutable bytes in `recorded_mutable`) but not yet committed. Used by
    // DataPartStorageOnDiskFull when a projection spill-and-merge reads back its own temp blocks before
    // the parent part's single commit. A file this transaction never staged resolves to nullopt/nullptr,
    // so the caller falls through to the committed metadata path (fail-close preserved).
    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;

    // B59 read-your-writes at DIRECTORY granularity: true iff this open transaction has STAGED (hardlinked /
    // written, not yet committed) at least one file under the directory `path` for `path`'s part. Mirrors the
    // file-granularity tryGetInFlight* trio; used by DataPartStorageOnDiskFull::existsDirectory so a carried-
    // forward projection dir is visible to loadProjections during finalize. Bails (false) on a path that is
    // not a part-relative file/dir (empty `p->file`).
    bool hasInFlightDirectory(const std::string & path) const override;

    // B59 read-your-writes directory ENUMERATION: the immediate-child names this transaction has STAGED
    // directly under the directory `path` (one level, the directory prefix stripped). Used by
    // DataPartStorageOnDiskFull::iterate so loadProjections' withPartFormatFromDisk can find the staged
    // projection's mark file (which determines the part format) before the whole-part commit.
    std::vector<std::string> listInFlightDirectory(const std::string & path) const override;

    void createMetadataFile(const std::string & path, const StoredObjects & objects) override;

    // Open a write buffer for a file.
    //
    // For a part file (<uuid[:3]>/<uuid>/<part>/<file>) this returns a content-addressed buffer
    // that hashes + uploads the content on finalize and, via its callback, records the resulting
    // blob under the path's logical file name (manifest + ref are published at commit).
    //
    // For a non-part / table-level file (e.g. <uuid[:3]>/<uuid>/format_version.txt) this returns a
    // plain object-storage write buffer that writes the bytes verbatim to a direct object key
    // (tableFileKey) — no content addressing, no ref, no manifest. Such objects are durable on
    // finalize and are not tracked by commit.
    //
    // The buffer-size and settings arguments mirror the disk-layer writeFile signature; content
    // addressing ignores append mode, so only Rewrite is meaningful here.
    std::unique_ptr<WriteBufferFromFileBase> writeFile(
        const std::string & path,
        size_t buf_size,
        WriteMode mode,
        const WriteSettings & settings);

    // No-op directory operations: object storage has no real directories; existsDirectory derives
    // from the refs/objects prefix. Mirrors how the plain-rewritable transaction treats directories.
    void createDirectory(const std::string & path) override;
    void createDirectoryRecursive(const std::string & path) override;
    void removeDirectory(const std::string & path) override;

    // Recursive removal = pointer-unlink + deferred GC.
    //
    // DROP TABLE and outdated-part cleanup reach this via DiskObjectStorageTransaction's
    // removeSharedRecursive / removeRecursive at commit. For a content-addressed pool, removal
    // deletes only the POINTER objects that the incoming path covers — the per-(server,table) refs
    // and the verbatim table-level / generic disk-level files — and NEVER the shared backing objects
    // (blobs/ content, parts/ manifests). Those are reclaimed later by the Phase-4 reachability GC, so
    // an orphaned blob/manifest leaks until then. That is the known, accepted M1 state (merges already
    // leak this way).
    //
    // The should_remove_objects predicate governs whether the SHARED backing objects (blobs) are
    // deleted. For content addressing the answer is always "no" (deferred GC), and the ref metadata
    // is always removed, so the predicate does not gate ref deletion here. Deletion is scoped
    // strictly by table_uuid / path so dropping one table never touches another table's refs in the
    // shared pool.
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;

    // Carry a file forward from an already-committed (or in-flight) source part without
    // re-uploading: resolve the source blob and record it under the destination logical file.
    //
    // This is the production carry-forward entry point: the disk layer's
    // DiskObjectStorageTransaction::createHardLink delegates to metadata_transaction->createHardLink,
    // so this override is what real mutations / ATTACH reach when hardlinking unchanged files.
    void createHardLink(const std::string & path_from, const std::string & path_to) override;

    // ==== Derived per-file metadata: no-ops for content-addressing ====
    //
    // For a content-addressed disk, per-file timestamps, permission bits, read-only flags and
    // hardlink counts are not stored — they are derived (or simply absent) when a part is resolved
    // via ref -> part_id -> manifest -> blob. None of them participate in CA resolution, so the
    // commit-replay calls that the INSERT / merge code path records for them are no-ops here.
    void setLastModified(const std::string & path, const Poco::Timestamp & timestamp) override;
    void chmod(const String & path, mode_t mode) override;
    void setReadOnly(const std::string & path) override;

    // Move/rename of a part directory. MergeTree writes a part under a temporary directory name
    // (e.g. tmp_insert_<part>) and renames it to the final <part> name at commit. Content
    // addressing keys recorded blobs by their in-part file name only, and the (table_uuid,
    // part_name) target is re-pinned to the destination so the ref is published under the final
    // part name. The blobs are already content-addressed, so no objects are moved in storage.
    void moveDirectory(const std::string & path_from, const std::string & path_to) override;

    // Move/rename of a single file within a part (e.g. column rename during a part write).
    // Re-key the recorded blob from the source in-part file name to the destination one; no object
    // is moved in storage since the blob is content-addressed.
    void moveFile(const std::string & path_from, const std::string & path_to) override;

    // Replace (overwrite-rename) of a single file. replaceFile = moveFile that overwrites the
    // destination. Reached by VersionMetadataOnDisk::storeInfoToDataPartStorage, which atomically
    // installs txn_version.txt via replaceFile(txn_version.txt.tmp, txn_version.txt) — both mutable
    // per-part files. Mutable-aware: the result must land in the per-ref sidecar (recorded_mutable),
    // never the manifest.
    void replaceFile(const std::string & path_from, const std::string & path_to) override;

    // Unlink a logical file. For a part file still being assembled in this commit, drop the
    // recorded blob so it is excluded from the manifest. For files that were never recorded (or
    // non-part files), there is nothing CA-resolution-relevant to remove, so it is a no-op.
    // The underlying content blob, if shared, is reclaimed by GC, not by an unlink here.
    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;

    // Truncate is meaningless for an immutable content-addressed blob; MergeTree does not truncate
    // committed part files, so this is a no-op (it would only ever be reached for a logical file
    // that is not part of CA resolution).
    void truncateFile(const std::string & path, size_t size) override;

    // Backwards-compatible alias used by the direct-call tests; forwards to createHardLink.
    void createHardLinkFrom(const std::string & from, const std::string & to) { createHardLink(from, to); }

private:
    /// Per-part staging. A single transaction may write more than one part — a transactional merge's one
    /// disk transaction spans the merge-output part PLUS the covered source parts' txn_version rewrites
    /// (B67). The single-part case (every INSERT / non-merge write) is exactly one entry.
    struct PartStaging
    {
        std::map<std::string, ContentAddressed::BlobEntry> recorded;     /// content blobs -> manifest
        std::map<std::string, std::string> recorded_mutable;             /// mutable per-part files -> sidecar
        std::set<std::string> recorded_mutable_removed;                  /// mutable files to delete from a committed sidecar
        std::string frozen_backup_name;                                  /// FREEZE target (per-part)
        std::string frozen_table_dir;
    };
    using PartKey = std::pair<std::string /*table_uuid*/, std::string /*part_name*/>;
    std::map<PartKey, PartStaging> parts;

    PartStaging & stagingFor(const std::string & table_uuid_, const std::string & part_name_) { return parts[PartKey{table_uuid_, part_name_}]; }
    PartStaging & stagingForPath(const ContentAddressed::PartFilePath & p) { return stagingFor(p.table_uuid, p.part_name); }
    const PartStaging * findStaging(const std::string & table_uuid_, const std::string & part_name_) const
    {
        auto it = parts.find(PartKey{table_uuid_, part_name_});
        return it == parts.end() ? nullptr : &it->second;
    }

    // Publish one staged part: the per-part body of commit(). Either the mutable-only sidecar update of an
    // already-committed part (no content blobs staged), or the whole-part publish (manifest + sidecar + ref)
    // for new content. An all-empty staging entry is a no-op. Does NOT take the gc_lock or persistSession —
    // commit() does that once around the loop over all parts.
    //
    // CA GC S4 (§5.1 rule 3): appends each `+`-delta's settled `(shard, epoch)` to `settled_delta_epochs`
    // so commit() can record them in this transaction's WriteSession — the session is retained until EVERY
    // such epoch is folded (the session-until-folded reaper gates on the folded watermark, §7.3).
    void commitOnePart(const PartKey & key, PartStaging & st, std::vector<std::pair<ContentAddressed::ShardId, uint64_t>> & settled_delta_epochs);

    // ==== CA GC S3: the writer's tomb re-check + resurrection (spec §6, §7.1 steps 4-5) ====
    //
    // A generic resolve-and-resurrect over a (id, gen)-key family, instantiated for both blobs and the
    // manifest. Resolve the current generation `g` for `id` via the best-effort `active` hint (default 0),
    // ensure `gen_key(id, g)` exists (the fresh-upload path created g=0; a reused/resurrected object exists
    // already), then RE-CHECK `tombstone_key(id, g)`: if the generation has been SEALED, ABANDON it and
    // RESURRECT to g+1 — condCreateIfAbsent the g+1 object (copying the still-present sealed generation's
    // byte-identical content), best-effort advance `active → g+1`, and retry the re-check (bounded). Never
    // wait, never rescue `g`, never delete the tombstone, never re-upload to the sealed key (§6). Returns
    // the RESOLVED, non-tombstoned generation the writer settled on (threaded into the `+` delta). In S3 the
    // gc_lock is still held across commit + sweep, so a seal rarely races the re-check — but the handshake
    // is wired correctly now (S4 drops the lock and relies on it).
    //
    // `make_gen_key` / `make_tomb_key` / `make_active_key` build the per-generation object/tombstone/active
    // keys for the id; `obj_exists` HEADs a key. The content for a resurrected generation is copied from the
    // present (sealed) generation, which the GC keeps until sweep (seal != delete).
    uint64_t resolveAndResurrectGeneration(
        const std::string & id_for_log,
        const std::function<std::string(uint64_t)> & make_gen_key,
        const std::function<std::string(uint64_t)> & make_tomb_key,
        const std::function<std::string()> & make_active_key);

    // Read-only `active` generation hint (default 0). The DROP path resolves the generation its `+` settled
    // on without ever resurrecting (a drop removes a reference, it never attaches one).
    uint64_t readActiveGenHint(const std::string & active_key) const;

    // Record (logical_file -> blob) for the part being written; the part_id is derived from this.
    void recordBlob(const std::string & path, ContentAddressed::BlobEntry entry);
    // Pin/verify the (table_uuid, part_name) all files of one commit must agree on.
    void rememberTarget(const std::string & path);

    // replaceFile safety-net helper: read the bytes of a part file that was just written in THIS
    // transaction as a content blob (`recorded`), so a rename whose destination is mutable can re-stage
    // them inline in the sidecar. Reads the just-uploaded blob object; falls back to the committed sidecar
    // bytes if the source is not staged as a blob here. Rare after Piece-1 .tmp inline recognition.
    std::string readStagedOrCommittedBytes(const std::string & path) const;

    // B52: release all in-flight blob pins this transaction holds (after the ref is published, or on
    // destruction of an uncommitted transaction). Idempotent.
    void releasePinnedBlobs() noexcept;

    // M8 (cross-mounter pin): record blob_hash in this transaction's WriteSession and (re)persist the
    // session object so a GC sweep on ANOTHER mounter treats the hash as reachable. Called under the GC
    // lock, BEFORE the blob is uploaded (the same point the in-process pin is taken), so the pin is
    // durable before the upload. The session is opened lazily on the first recorded blob: it is owned
    // by this transaction at a unique key, so the owner rewrites its OWN object (no CAS needed).
    void recordBlobInSession(const ContentAddressed::BlobHash & blob_hash);

    // M8: persist (and RENEW the lease of) this transaction's WriteSession object. Called per recorded
    // blob and again at commit before the ref publish, so the advisory lease stays live while this write
    // is progressing — a stale lease would let a remote sweep drop the pin and reclaim an
    // about-to-be-referenced blob (cross-process data-loss window).
    void persistSession();

    // M8: best-effort remove this transaction's WriteSession object (after the ref is published, or on
    // destruction of an uncommitted transaction). The ref now keeps the blobs reachable, so the pin is
    // no longer needed; an aborted session would expire by its lease anyway, but clean it up eagerly.
    // Idempotent and never throws (mirrors releasePinnedBlobs).
    //
    // CA GC S4 (§5.1 rule 3): a COMMITTED session is NOT released here at commit (nor in the destructor) —
    // it is retained until its `+` deltas are FOLDED. The session-until-folded reaper (in the GC sweep)
    // deletes it once the folded watermark passes its recorded epochs. commit() drops it eagerly only when
    // every settled epoch is ALREADY folded (allSettledEpochsFolded).
    void releaseSession() noexcept;

    // CA GC S4 (§5.1 rule 3) — the post-commit folded check: true iff EVERY `(shard, epoch)` this commit's
    // `+` deltas settled in is folded into a durable snapshot (GcCompaction::isEpochFolded). A delta-less
    // commit (empty set) is trivially folded; a throw is treated as "not folded" (keep the session for the
    // reaper). Lets commit() drop the session immediately on the common already-folded path while leaving an
    // unfolded one for the watermark reaper — the commit NEVER blocks on folding.
    bool allSettledEpochsFolded(
        const std::vector<std::pair<ContentAddressed::ShardId, uint64_t>> & settled_delta_epochs) const;

    // DETACH PARTITION: re-publish a committed part directory as a detached ref. The detached ref is
    // named "detached" and carries the source part's manifest blob entries and mutable sidecar files
    // re-keyed under the detached part directory component, then the source ref is unlinked. Content
    // blobs and the source manifest are untouched (immutable). See moveDirectory (B36).
    void republishCommittedPartIntoDetached(
        const ContentAddressed::PartFilePath & src_committed, const ContentAddressed::PartFilePath & dst_detached);

    // DETACHED part rename within the detached namespace (detached/<old> -> detached/<new>), used by
    // DROP DETACHED PARTITION ("deleting_" rename before removal) and ATTACH ("attaching_" rename). The
    // detached parts share the one "detached" ref whose keys are <detached_part>/<file> (B36/B46); this
    // re-keys only this part's <old>/ prefix to <new>/ in the shared manifest + sidecar bundle (and the
    // per-file sidecar objects), leaving the other detached parts intact. No content blobs move. Reached
    // only from moveDirectory.
    void rekeyDetachedPartDir(const std::string & table_id, const std::string & old_dir, const std::string & new_dir);

    // ATTACH PARTITION / ATTACH PART: publish an ACTIVE ref from a DETACHED staging directory. ATTACH
    // stages the detached part as detached/attaching_<part> (a single component under the shared
    // "detached" ref), then renames it to the active part dir <uuid[:3]>/<uuid>/<active_part>. This is
    // the inverse of republishCommittedPartIntoDetached: take the staging dir's <staging>/<inner> keys
    // from the shared detached ref, re-key them to the bare <inner> names of a NEW active part manifest,
    // publish the active ref (the commit point), then strip the <staging>/ keys from the detached ref
    // (rewriting it without them, or unlinking it when no detached parts remain). No content blobs move
    // (content-addressed); only the small ref/manifest/sidecar pointer objects are written. Reached only
    // from moveDirectory.
    void republishDetachedStagingIntoActive(
        const ContentAddressed::PartFilePath & src_staging, const ContentAddressed::PartFilePath & dst_active);

    // Projection MATERIALIZE / merge: rename a STAGED projection directory within the part this
    // transaction is assembling, <part>/<X>.tmp_proj -> <part>/<proj>.proj. MergeTree builds the
    // projection part under a temporary <proj>_<n>.tmp_proj subdirectory of the (not-yet-committed)
    // parent part and renames it to the final <proj>.proj before the parent part commits
    // (MergeProjectionPartsTask). Content addressing keys the staged projection files by their in-part
    // logical name (<X>.tmp_proj/<inner>), so this re-keys every recorded blob and inline mutable file
    // under the old projection-dir prefix to the new <proj>.proj/ prefix in this transaction's staged
    // maps. No object is moved in storage (content-addressed), and no ref is published yet — the parent
    // part's ref/manifest is published at commit with the re-keyed <proj>.proj/ keys. Returns true if
    // the move was a staged same-part projection-dir rename and was handled here. Reached only from
    // moveDirectory.
    bool rekeyStagedProjectionDir(
        const ContentAddressed::PartFilePath & src, const ContentAddressed::PartFilePath & dst);

    // RENAME TABLE / cross-engine table move: re-key every ref + per-ref sidecar (and verbatim
    // table-level file) from the source table identifier to the destination table identifier, then
    // unlink the source pointer objects. The table identifier is part of the ref/store object key, so
    // a rename changes it (e.g. Ordinary data/db/mt -> Atomic <uuid>) and the read at the new identity
    // would otherwise find no ref (B40). The shared blobs/manifests are content-addressed and
    // untouched; only the small pointer objects move. Reached only from moveDirectory.
    void republishTableRefs(const std::string & src_table_id, const std::string & dst_table_id);

    // Rename the ref (+ per-ref sidecar objects) of a COMMITTED part directory within one table:
    // <uuid>/<src_part> -> <uuid>/<dst_part>. MergeTree renames a committed part to delete_tmp_<part>
    // before removing it (DataPartStorageOnDiskBase::remove), and renames merged/mutated parts, via
    // disk->moveDirectory with NOTHING staged in this transaction. Content addressing has no rename, so
    // re-key the small pointer objects (ref, bundle <part>.meta, per-file <part>.<file>.meta) from the
    // source name to the destination name; the shared blobs/manifest are content-addressed and
    // untouched. Returns true if a committed source ref was found and re-keyed. Without this the source
    // ref survives the "remove", so the part is rediscovered on the next ATTACH (B45).
    bool renameCommittedPartRef(
        const ContentAddressed::PartFilePath & src, const ContentAddressed::PartFilePath & dst);

    // Authoritative removal of a single part directory <uuid[:3]>/<uuid>/<part>: unlink the part's
    // ref and its per-ref sidecar objects (the bundle <part>.meta and each per-file <part>.<file>.meta),
    // keeping the shared blobs/manifest for deferred GC. Returns true if the path was a regular part
    // directory and was handled here. Shared by removeRecursive (slow path) and removeDirectory (the
    // MergeTree fast removal path ends with removeDirectory(<part>), so this is where a complete part's
    // ref MUST be unlinked or it is rediscovered on the next ATTACH — B45).
    bool unlinkPartDirRefs(const std::string & path);

    /// Object-storage common key prefix; authoritative copy from the metadata storage (see ctor).
    const std::string key_prefix;
    /// Server-local scratch dir for the write-buffer spill (see ctor).
    const std::string local_scratch_path;
    /// The LAST-remembered target (most recent rememberTarget). Many methods reference these to scope a
    /// staging lookup; they no longer mean "the only part".
    std::string table_uuid;
    std::string part_name;
    /// B52: full blob object keys this transaction pinned in the pool's in-flight set (via the write
    /// buffer, under the GC lock). Released once the ref is published or the transaction is destroyed.
    std::set<std::string> pinned_blob_keys;

    /// M8 (cross-mounter pin): this transaction's WriteSession, opened lazily on the first recorded
    /// blob. While open it is persisted at sessionKey(key_prefix, session_id) (a unique key owned by
    /// this transaction) and lists every pending blob hash. Removed once the ref is published (commit)
    /// or the transaction is destroyed uncommitted. session_open is false until the first blob.
    bool session_open = false;
    std::string session_id;
    ContentAddressed::WriteSession session;

    /// CA GC S4 (#2): the `+` deltas whose flush threw during commit (one per failed part in a multi-part
    /// transaction). Serialized as ONE batch into the sticky session's pending_add_delta for the reaper.
    std::vector<ContentAddressed::GcDelta> failed_add_deltas;
};

}
