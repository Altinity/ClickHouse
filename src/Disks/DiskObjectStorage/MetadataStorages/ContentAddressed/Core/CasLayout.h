#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/Exception.h>
#include <base/types.h>
#include <optional>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
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
///   - GC snapshots:     POOL/gc/snap/GENERATION/SNAP_SHARD
///   - other GC state:   POOL/gc/...
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

    /// The namespace registry (design §5.3): authoritative namespace universe, CAS-appended on
    /// W-REGISTER, fenced by GC, the source of GC discovery (never LIST). Relocated from
    /// `roots/_registry` to `gc/registry` — `roots/` is now data only; discovery is infrastructure.
    /// The CAS-append + fence MECHANISM is unchanged (N5): only the key moves, and it is read/written
    /// strictly by this computed key.
    String rootsRegistryKey() const
    {
        return prefix + "/gc/registry";
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

    /// Verbatim (non-content-addressed) file stored under a namespace. Names may be NESTED
    /// (relative sub-paths — the wiring stores table-level subdirectory files such as
    /// deduplication_logs/deduplication_log_1.txt verbatim, M-W T2); empty segments, leading or
    /// trailing '/', and '..' segments are rejected (no escaping the namespace's files prefix).
    String namespaceFileKey(const RootNamespace & ns, const String & file_name) const
    {
        checkNamespace(ns);
        const bool bad_shape = file_name.empty() || file_name.front() == '/' || file_name.back() == '/'
            || file_name.find("//") != String::npos || file_name == ".." || file_name.starts_with("../")
            || file_name.ends_with("/..") || file_name.find("/../") != String::npos;
        if (bad_shape)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: namespace file name must be a clean relative path, got '{}'", file_name);
        return prefix + "/roots/" + ns.string() + "/_files/" + file_name;
    }

    /// Prefix that covers all verbatim files of a namespace (for list).
    String namespaceFilesPrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/roots/" + ns.string() + "/_files/";
    }

    /// A PLAIN mountpoint object (design §5.2): a loose, non-content-addressed file mirrored at its
    /// ClickHouse path under `roots/`, with NO namespace and NO `_files` wrapper. `key` is the
    /// server-prefixed mirrored path (e.g. `srv1/clickhouse_access_check_abc`). It must NOT end in a
    /// reserved area. Shard discovery is via the registry (`listNamespaces`) + static `[0, root_shards)`
    /// fan-out — not by key classification. The `_files`/`_pool_meta` reservations still apply to its
    /// segments via the path itself (these never appear in a real ClickHouse loose-file path).
    String mountpointObjectKey(const String & key) const
    {
        if (key.empty() || key.front() == '/' || key.back() == '/' || key.find("//") != String::npos)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: mountpoint object key must be a clean relative path, got '{}'", key);
        return prefix + "/roots/" + key;
    }

    /// GC keys.
    String gcStateKey() const
    {
        return prefix + "/gc/state";
    }

    /// GC heartbeat (advisory liveness pulse; B160): <prefix>/gc/hb.
    String gcHbKey() const
    {
        return prefix + "/gc/hb";
    }

    /// In-degree snapshot object: <prefix>/gc/snap/<generation>/<snap_shard>.
    /// Sharded by the TARGET content-hash prefix (spec §4, decision 2026-06-11): every edge
    /// targeting a node lands in the node's own snap shard, so in-degree is intra-shard.
    String gcSnapKey(uint64_t generation, uint64_t snap_shard) const
    {
        return prefix + "/gc/snap/" + std::to_string(generation) + "/" + std::to_string(snap_shard);
    }

    /// Prefix that covers all snap shards of one generation (for list).
    String gcSnapShardPrefix(uint64_t generation) const
    {
        return prefix + "/gc/snap/" + std::to_string(generation) + "/";
    }

    /// Prefix that covers every root-shard manifest and namespace file (GC round discovery).
    String rootsPrefix() const
    {
        return prefix + "/roots/";
    }

    /// Prefixes that cover every content object of one kind (raw object listing for fsck).
    String blobsPrefix() const { return prefix + "/blobs/"; }
    String treesPrefix() const { return prefix + "/trees/"; }
    String packsPrefix() const { return prefix + "/packs/"; }

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

    /// Per-server build watermark key.
    String serverWatermarkKey(const String & server_id_hex) const
    {
        return shardedKey("servers", server_id_hex);
    }

    /// Pool-level metadata object.
    String poolMetaKey() const
    {
        return prefix + "/_pool_meta";
    }

    /// The build-root namespace (B171): `_builds/<server_hex>`, one shard per in-flight build keyed by
    /// `build_seq`. A precommit publishes the build's manifest tree as a ref in that shard, so GC's
    /// fold lifts the in-degree of every reachable object — replacing the revocable `cas_owner` hint.
    /// It is an ordinary namespace key-wise (`rootShardKey` works unchanged); only behavioral branches
    /// (fold pending-tolerance, precommit reclaim) key off `isBuildRootNamespace`.
    static bool isBuildRootNamespace(const RootNamespace & ns)
    {
        return ns.string().starts_with("_builds/");
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
            /// Reserved for the build-root namespace (B171: `_builds/<server_hex>`). A user namespace
            /// must not use the `_builds` segment, but the build-root namespace itself is legal — it is
            /// exactly `_builds/<server_hex>`, recognized by `isBuildRootNamespace`.
            if (segment == "_builds" && !isBuildRootNamespace(ns))
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_builds'", s);
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

/// The object key for a (kind, hash) — the single kind→key-shape mapping (blobs/trees/packs).
/// Shared by `Build` (the publish gate's observe/resurrect) and `Gc` (the retire HEAD and the
/// exact-token delete): both sides MUST address the same object the same way.
inline String objectKey(const Layout & layout, ObjectKind kind, const UInt128 & hash)
{
    const String id = u128ToHex(hash);
    switch (kind)
    {
        case ObjectKind::Blob:
            return layout.blobKey(BlobId(id));
        case ObjectKind::Tree:
            return layout.treeKey(TreeId(id));
        case ObjectKind::Pack:
            return layout.packKey(PackId(id));
    }
    throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "objectKey: unknown ObjectKind {}", static_cast<uint8_t>(kind));
}

}
