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

ContentAddressedTransaction::ContentAddressedTransaction(ContentAddressedMetadataStorage & metadata_storage_)
    : metadata_storage(metadata_storage_)
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

std::unique_ptr<ContentAddressed::ContentAddressedWriteBuffer> ContentAddressedTransaction::writeFile(
    const std::string & path,
    size_t /*buf_size*/,
    WriteMode /*mode*/,
    const WriteSettings & /*settings*/)
{
    rememberTarget(path);

    auto p = ContentAddressed::parsePartFilePath(path);
    const std::string file = p->file;

    /// The content pool root mirrors the read path: bare keys for now (Phase 3 TODO honors the
    /// common key prefix). Spill temp files under the object-storage common key prefix so they
    /// share its disk and are cleaned up alongside it.
    const std::string temp_dir = metadata_storage.object_storage->getCommonKeyPrefix() + "/cas_wbuf_tmp";

    return std::make_unique<ContentAddressed::ContentAddressedWriteBuffer>(
        metadata_storage.object_storage,
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
    const std::string part_key = ContentAddressed::partKey(part_id);
    if (!metadata_storage.object_storage->tryGetObjectMetadata(part_key, /*with_tags=*/false).has_value())
    {
        const std::string bytes = footer.serialize();
        auto out = metadata_storage.object_storage->writeObject(StoredObject(part_key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    }

    // Publish the ref last: store/{server_id}/{table_uuid}/refs/{part_name} = part_id.
    const std::string ref_key = ContentAddressed::refKey(metadata_storage.server_id, table_uuid, part_name);
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

}
