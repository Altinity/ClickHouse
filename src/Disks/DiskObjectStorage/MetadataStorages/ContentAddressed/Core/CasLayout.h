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
///   - root manifests:   POOL/roots/SERVER_ID/TABLE_UUID/SHARD_NUMBER
///   - GC state:         POOL/gc/...
///   - build heartbeats: POOL/builds/S/BUILD_ID
///   - pool metadata:    POOL/_pool_meta
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

    /// Root manifest for a given server + table UUID + shard number.
    String rootShardKey(const String & server_id, const String & table_uuid, uint64_t shard) const
    {
        return prefix + "/roots/" + server_id + "/" + table_uuid + "/" + std::to_string(shard);
    }

    /// Prefix that covers all root manifest shards for a given server + table UUID (for list).
    String rootNamespacePrefix(const String & server_id, const String & table_uuid) const
    {
        return prefix + "/roots/" + server_id + "/" + table_uuid + "/";
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
