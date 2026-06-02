#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolScan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace ContentAddressed
{

namespace
{

/// Parse a ref object payload into the part id it names. The write path stores the part id verbatim
/// (a 32-char lowercase-hex string, see computePartId). Per B22(c) we tolerate a possible leading
/// version byte and trailing whitespace/newline by extracting the longest run of lowercase-hex
/// characters: an unversioned payload is unchanged, a future versioned one drops its marker byte.
/// An empty hex run is corruption (a published ref must name a part) and throws fail-close.
std::string parseRefPayload(const std::string & payload, const std::string & ref_key)
{
    size_t begin = payload.find_first_of("0123456789abcdef");
    if (begin == std::string::npos)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: ref {} has no part id in its payload", ref_key);
    size_t end = payload.find_first_not_of("0123456789abcdef", begin);
    return payload.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string readSmallObject(const ObjectStoragePtr & object_storage, const std::string & key)
{
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

}

std::vector<std::string> listKeysUnder(const ObjectStoragePtr & object_storage, const std::string & prefix)
{
    RelativePathsWithMetadata children;
    object_storage->listObjects(prefix, children, /*max_keys=*/0);
    std::vector<std::string> keys;
    keys.reserve(children.size());
    for (const auto & child : children)
        keys.push_back(child->relative_path);
    return keys;
}

std::set<std::string> listLivePartIds(const ObjectStoragePtr & object_storage, const std::string & key_prefix)
{
    /// Every server's/table's refs live under refsRootPrefix. The same root also holds verbatim
    /// table-level files under the store/server/uuid/files/ layout, so we keep only keys whose path has a
    /// /refs/ segment (the ref objects); their payload is the live part id.
    static const std::string refs_segment = "/refs/";
    std::set<std::string> live;
    for (const auto & key : listKeysUnder(object_storage, refsRootPrefix(key_prefix)))
    {
        if (key.find(refs_segment) == std::string::npos)
            continue;
        live.insert(parseRefPayload(readSmallObject(object_storage, key), key));
    }
    return live;
}

}

}
