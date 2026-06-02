#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/MetadataStorages/StaticDirectoryIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <unordered_set>

#include <filesystem>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>

namespace fs = std::filesystem;

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
}

ContentAddressedMetadataStorage::ContentAddressedMetadataStorage(ObjectStoragePtr object_storage_, String storage_path_prefix_, String server_id_)
    : object_storage(std::move(object_storage_))
    , storage_path_prefix(std::move(storage_path_prefix_))
    , storage_path_full(fs::path(object_storage->getRootPrefix()) / storage_path_prefix)
    , server_id(std::move(server_id_))
{
}

MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    return std::make_shared<ContentAddressedTransaction>(*this);
}

std::optional<std::string> ContentAddressedMetadataStorage::readSmallObjectIfExists(const std::string & key) const
{
    // TODO(phase3): honor object_storage->getCommonKeyPrefix() (bare keys for now).
    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
        return std::nullopt;

    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

std::optional<std::string> ContentAddressedMetadataStorage::readRefPartId(const std::string & table_uuid, const std::string & part_name) const
{
    return readSmallObjectIfExists(ContentAddressed::refKey(server_id, table_uuid, part_name));
}

ContentAddressed::Footer ContentAddressedMetadataStorage::loadFooterOrThrow(const std::string & part_id) const
{
    auto bytes = readSmallObjectIfExists(ContentAddressed::partKey(part_id));
    if (!bytes)
        throw Exception(
            ErrorCodes::CORRUPTED_DATA,
            "ContentAddressed: live ref points at missing footer parts/{}",
            part_id);
    return ContentAddressed::Footer::deserialize(*bytes);
}

ContentAddressed::BlobEntry ContentAddressedMetadataStorage::resolveBlobEntry(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto footer = loadFooterOrThrow(*pid);
    auto it = footer.blobs.find(p->file);
    if (it == footer.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in footer of {}", p->file, path);
    return it->second;
}

bool ContentAddressedMetadataStorage::existsFile(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return false;
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        return false;
    auto footer = loadFooterOrThrow(*pid);
    return footer.blobs.contains(p->file);
}

bool ContentAddressedMetadataStorage::existsDirectory(const std::string & path) const
{
    if (auto uuid = ContentAddressed::parseTableUuid(path))
    {
        // Table dir exists iff it has at least one ref (part).
        RelativePathsWithMetadata files;
        object_storage->listObjects(ContentAddressed::refsPrefix(server_id, *uuid), files, 0);
        return !files.empty();
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
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto footer = loadFooterOrThrow(*pid);
    auto it = footer.blobs.find(p->file);
    if (it == footer.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in footer of {}", p->file, path);
    return it->second.size;
}

Poco::Timestamp ContentAddressedMetadataStorage::getLastModified(const std::string & path) const
{
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
        std::string prefix = ContentAddressed::refsPrefix(server_id, *uuid);
        RelativePathsWithMetadata files;
        object_storage->listObjects(prefix, files, 0);

        std::unordered_set<std::string> result;
        for (const auto & elem : files)
        {
            const auto & p = elem->relative_path;
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

    // Part dir <uuid[:3]>/<uuid>/<part>[/]: list the logical file names from the footer.
    if (auto p = ContentAddressed::parsePartFilePath(path); p && p->file.empty())
    {
        auto pid = readRefPartId(p->table_uuid, p->part_name);
        if (!pid)
            return {}; // absent ref => empty listing
        auto footer = loadFooterOrThrow(*pid); // missing footer for a present ref => CORRUPTED_DATA
        std::vector<std::string> result;
        result.reserve(footer.blobs.size());
        for (const auto & [file, _] : footer.blobs)
            result.push_back(file);
        return result;
    }

    // Root or unrecognized path.
    return {};
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
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto pid = readRefPartId(p->table_uuid, p->part_name);
    if (!pid)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    auto footer = loadFooterOrThrow(*pid);
    auto it = footer.blobs.find(p->file);
    if (it == footer.blobs.end())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in footer of {}", p->file, path);

    const auto & e = it->second;
    // BARE key (no common-prefix prepend); must match what the gtest seeds and what blobs are stored under.
    // TODO(phase3): honor object_storage->getCommonKeyPrefix().
    return {StoredObject(ContentAddressed::blobKey(e.key), path, e.size)};
}

}
