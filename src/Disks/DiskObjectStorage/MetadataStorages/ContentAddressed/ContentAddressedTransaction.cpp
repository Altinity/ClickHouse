#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <Common/Exception.h>
#include <Common/ObjectStorageKey.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}

ContentAddressedTransaction::ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_, std::string key_prefix_)
    : metadata_storage(metadata_storage_)
    , key_prefix(std::move(key_prefix_))
{
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
    }
    else if (table_uuid != p->table_uuid || part_name != p->part_name)
    {
        throw Exception(
            ErrorCodes::LOGICAL_ERROR,
            "ContentAddressed: a single transaction must write one part, got {}/{} and {}/{}",
            table_uuid, part_name, p->table_uuid, p->part_name);
    }
}

void ContentAddressedTransaction::recordBlob(const std::string & path, ContentAddressed::BlobEntry entry)
{
    auto p = ContentAddressed::parsePartFilePath(path);
    chassert(p && !p->file.empty());
    recorded[p->file] = std::move(entry);
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
    /// Both are written verbatim — no content addressing, no ref, no footer. They are durable on
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

    /// The blob is keyed under the same common key prefix as the read path (key_prefix, taken from
    /// the metadata storage). Spill temp files under the object-storage common key prefix so they
    /// share its disk and are cleaned up alongside it.
    const std::string temp_dir = metadata_storage.object_storage->getCommonKeyPrefix() + "/cas_wbuf_tmp";

    return std::make_unique<ContentAddressed::ContentAddressedWriteBuffer>(
        metadata_storage.object_storage,
        key_prefix,
        temp_dir,
        [this, file](const std::string & blob_hash, size_t size)
        {
            recorded[file] = ContentAddressed::BlobEntry{blob_hash, size, blob_hash};
        });
}

void ContentAddressedTransaction::createHardLink(const std::string & from, const std::string & to)
{
    rememberTarget(to);

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
    /// (ref -> part_id -> footer -> blob). Reached via DiskObjectStorageTransaction.cpp:459.
}

void ContentAddressedTransaction::chmod(const String &, mode_t)
{
    /// No-op: permission bits are not stored for content-addressed objects.
}

void ContentAddressedTransaction::setReadOnly(const std::string &)
{
    /// No-op: content-addressed parts are immutable by construction; a read-only flag is implicit.
}

void ContentAddressedTransaction::moveDirectory(const std::string & from, const std::string & to)
{
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

    /// Nothing staged yet (directory-only move before any file write): adopt the destination.
    if (table_uuid.empty() && part_name.empty())
    {
        table_uuid = dst_part->table_uuid;
        part_name = dst_part->part_name;
        return;
    }

    /// The source must be the part we have been assembling.
    auto src_part = ContentAddressed::parsePartFilePath(from);
    if (src_part && src_part->file.empty()
        && src_part->table_uuid == table_uuid && src_part->part_name == part_name)
    {
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
    /// Rename of a single in-part file: re-key the recorded blob. No object is moved in storage.
    auto src = ContentAddressed::parsePartFilePath(from);
    auto dst = ContentAddressed::parsePartFilePath(to);
    if (!src || src->file.empty() || !dst || dst->file.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile requires two part-file paths: {} -> {}", from, to);

    rememberTarget(to);
    auto it = recorded.find(src->file);
    if (it == recorded.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: moveFile source not recorded: {}", from);

    auto entry = it->second;
    recorded.erase(it);
    recorded[dst->file] = std::move(entry);
}

void ContentAddressedTransaction::unlinkFile(const std::string & path, bool, bool)
{
    /// Part file: drop the staged blob so it is excluded from the footer. The underlying content
    /// blob, if shared, is reclaimed by GC, not here.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && !p->file.empty())
    {
        recorded.erase(p->file);
        return;
    }

    /// Table-level file (e.g. format_version.txt): no CA-resolution-relevant state and no verbatim
    /// removal in M1 — left to GC, matching the existing read/write treatment of such files.
    if (ContentAddressed::parseTableFilePath(path))
        return;

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
    if (recorded.empty())
        return;

    if (table_uuid.empty() || part_name.empty())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "ContentAddressed: commit without a resolved part target");

    ContentAddressed::Footer footer;
    footer.blobs = recorded;

    const std::string part_id = ContentAddressed::computePartId(recorded);

    /// Put-if-absent the footer: identical parts (same deterministic blobs) share one footer object.
    const std::string part_key = ContentAddressed::partKey(key_prefix, part_id);
    if (!metadata_storage.object_storage->tryGetObjectMetadata(part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = footer.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }

    // Publish the ref last: store/{server_id}/{table_uuid}/refs/{part_name} = part_id.
    const std::string ref_key = ContentAddressed::refKey(key_prefix, metadata_storage.server_id, table_uuid, part_name);
    auto ref_out = metadata_storage.object_storage->writeObject(StoredObject(ref_key), WriteMode::Rewrite);
    ref_out->write(part_id.data(), part_id.size());
    ref_out->finalize();
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

void ContentAddressedTransaction::removeDirectory(const std::string &)
{
    /// No-op (see createDirectory).
}

}
