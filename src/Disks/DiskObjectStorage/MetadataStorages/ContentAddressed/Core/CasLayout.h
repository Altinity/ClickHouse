#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/Exception.h>
#include <base/types.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// Pure key-construction functions for a content-addressed pool.
///
/// Every key is built from a pool prefix and a stable path sub-tree (POOL = pool prefix, S = 2-char shard, ID = full id):
///   - content objects:  POOL/blobs/S/ID
///   - tree objects:     POOL/trees/S/ID
///   - pack objects:     POOL/packs/S/ID
///   - root manifests:   POOL/roots/NAMESPACE/SHARD_NUMBER
///   - verbatim files:   POOL/roots/NAMESPACE/_files/FILE_NAME
///   - GC state:         POOL/gc/...
///   - build heartbeats: POOL/builds/S/BUILD_ID
///   - pool metadata:    POOL/_pool_meta
///
/// NAMESPACE is opaque to the core: the wiring composes strings like "srv1/<table_uuid>" or
/// "shadow/<backup>/<table_uuid>". The reserved "_files" segment cannot collide with root shard
/// keys because shard names are numeric, and checkNamespace rejects "_files" as a namespace
/// segment.
///
/// The 2-char shard is always the first two characters of the id string.
/// This matches the protocol spec §4 layout exactly.
class Layout
{
public:
    explicit Layout(String prefix_) : prefix(std::move(prefix_)) {}

    /// Content objects.
    String blobKey(const BlobId & id) const
    {
        return shardedKey("blobs", id.string());
    }

    String treeKey(const TreeId & id) const
    {
        return shardedKey("trees", id.string());
    }

    String packKey(const PackId & id) const
    {
        return shardedKey("packs", id.string());
    }

    /// Root manifest for a given namespace + shard number.
    String rootShardKey(const RootNamespace & ns, uint64_t shard) const
    {
        checkNamespace(ns);
        return prefix + "/roots/" + ns.string() + "/" + std::to_string(shard);
    }

    /// Prefix that covers all root manifest shards of a namespace (for list).
    /// Note: also covers the "_files/" sub-prefix; listers must skip it.
    String rootNamespacePrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/roots/" + ns.string() + "/";
    }

    /// Verbatim (non-content-addressed) file stored under a namespace.
    /// File names are flat: no '/' allowed.
    String namespaceFileKey(const RootNamespace & ns, const String & file_name) const
    {
        checkNamespace(ns);
        if (file_name.empty() || file_name.find('/') != String::npos)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: namespace file name must be non-empty and flat (no '/'), got '{}'", file_name);
        return prefix + "/roots/" + ns.string() + "/_files/" + file_name;
    }

    /// Prefix that covers all verbatim files of a namespace (for list).
    String namespaceFilesPrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/roots/" + ns.string() + "/_files/";
    }

    /// GC keys.
    String gcStateKey() const
    {
        return prefix + "/gc/state";
    }

    /// Snapshot key: <prefix>/gc/snap/<round>/<fence_seq>
    String gcSnapKey(uint64_t round, uint64_t fence_seq) const
    {
        return prefix + "/gc/snap/" + std::to_string(round) + "/" + std::to_string(fence_seq);
    }

    /// Retired-set key: <prefix>/gc/retired/<round>.<fence_seq>/<shard>
    String retiredKey(uint64_t round, uint64_t fence_seq, uint64_t shard) const
    {
        return prefix + "/gc/retired/" + std::to_string(round) + "." + std::to_string(fence_seq)
               + "/" + std::to_string(shard);
    }

    /// Prefix that covers all retired-set objects (for list).
    String gcRetiredPrefix() const
    {
        return prefix + "/gc/retired/";
    }

    /// Outcomes key: <prefix>/gc/outcomes/<round>.<fence_seq>/<shard>
    String outcomesKey(uint64_t round, uint64_t fence_seq, uint64_t shard) const
    {
        return prefix + "/gc/outcomes/" + std::to_string(round) + "." + std::to_string(fence_seq)
               + "/" + std::to_string(shard);
    }

    /// Checkpoint key: <prefix>/gc/checkpoint/<version>
    String checkpointKey(uint64_t version) const
    {
        return prefix + "/gc/checkpoint/" + std::to_string(version);
    }

    /// Build heartbeat key.
    String buildHeartbeatKey(const String & build_id) const
    {
        return shardedKey("builds", build_id);
    }

    /// Pool-level metadata object.
    String poolMetaKey() const
    {
        return prefix + "/_pool_meta";
    }

private:
    String prefix;

    /// A namespace must be non-empty, with no leading/trailing '/', no empty segment ("//"),
    /// and no segment equal to the reserved "_files".
    void checkNamespace(const RootNamespace & ns) const
    {
        const String & s = ns.string();
        if (s.empty())
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS, "CasLayout: namespace must be non-empty");

        size_t start = 0;
        while (true)
        {
            size_t end = s.find('/', start);
            const String segment = s.substr(start, end == String::npos ? String::npos : end - start);
            if (segment.empty())
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' has an empty segment (leading/trailing or doubled '/')", s);
            if (segment == "_files")
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_files'", s);
            if (end == String::npos)
                break;
            start = end + 1;
        }
    }

    /// Build <prefix>/<namespace>/<first2chars>/<id>.
    /// Throws BAD_ARGUMENTS if id is shorter than 2 characters.
    String shardedKey(const String & ns, const String & id) const
    {
        if (id.size() < 2)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: id must be at least 2 characters, got '{}'", id);
        return prefix + "/" + ns + "/" + id.substr(0, 2) + "/" + id;
    }
};

}
