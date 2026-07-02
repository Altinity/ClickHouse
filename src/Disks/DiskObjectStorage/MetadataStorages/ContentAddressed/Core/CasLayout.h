#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
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
///   - root manifests:   POOL/roots/NAMESPACE/SHARD_NUMBER
///   - verbatim files:   POOL/roots/NAMESPACE/_files/FILE_NAME
///   - GC snapshots:     POOL/gc/snap/GENERATION/SNAP_SHARD
///   - other GC state:   POOL/gc/...
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

    /// Root manifest for a given namespace + shard number.
    /// Phase 1: relocated out of the shared `roots/` tree to `cas/refs/` (hot/cold split). The
    /// namespace fan-out (`<ns>/<shard>`) is unchanged — identity-preserving. GC discovery LISTs
    /// `casRefsPrefix()` (only ref shards, no manifest backlog or verbatim files interleaved).
    String rootShardKey(const RootNamespace & ns, uint64_t shard) const
    {
        checkNamespace(ns);
        return prefix + "/cas/refs/" + ns.string() + "/" + std::to_string(shard);
    }

    /// Pool-wide ref-shard prefix (Phase 1): `<prefix>/cas/refs/`. The base of every `rootShardKey`,
    /// used by GC discovery for the LIST sweep and the strip-to-cursor-key step.
    String casRefsPrefix() const
    {
        return prefix + "/cas/refs/";
    }

    /// Prefix that covers all part-manifests of a namespace (Phase 1): `<prefix>/cas/manifests/<ns>/`.
    /// Replaces the old `rootNamespacePrefix(ns) + "_manifests/"` enumeration (sweep + fsck).
    String manifestNamespacePrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/cas/manifests/" + ns.string() + "/";
    }

    /// Pool-wide part-manifest prefix (Phase 1): `<prefix>/cas/manifests/`.
    String casManifestsPrefix() const
    {
        return prefix + "/cas/manifests/";
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

    /// Part manifest body key (spec §S3 Layout; Phase 3 identity reshape):
    ///   <prefix>/cas/manifests/<ns>/<writer_epoch>/<build_sequence>/<000001>.proto
    /// `writer_epoch` is the durable monotone mount epoch; `manifest_ordinal` is a per-build ordinal
    /// rendered as a six-digit filename. `root_namespace_id` comes from the owning context
    /// (the `ManifestId`), never from the journal ref.
    String manifestKey(const ManifestId & id) const
    {
        checkNamespace(id.root_namespace);
        return prefix + "/cas/manifests/" + id.root_namespace.string() + "/"
             + std::to_string(id.ref.writer_epoch) + "/" + std::to_string(id.ref.build_sequence) + "/"
             + manifestOrdinalFileName(id.ref.manifest_ordinal);
    }

    /// A PLAIN mountpoint object (design §5.2): a loose, non-content-addressed file mirrored at its
    /// ClickHouse path under `roots/`, with NO namespace and NO `_files` wrapper. `key` is the
    /// server-prefixed mirrored path (e.g. `srv1/clickhouse_access_check_abc`). It must NOT end in a
    /// reserved area. Shard discovery is via `LIST(cas/refs/)` — not by key classification or a registry.
    /// The `_files`/`_pool_meta` reservations still apply to its segments via the path itself
    /// (these never appear in a real ClickHouse loose-file path).
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

    /// Prefix that covers EVERY per-round artifact of one generation (all attempts): <prefix>/gc/gen/<generation>/
    /// The wholesale retention prune reclaims a whole generation (every attempt's debris) by this prefix.
    String gcGenPrefix(uint64_t generation) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/";
    }

    /// Prefix that covers one (generation, attempt)'s artifacts: <prefix>/gc/gen/<generation>/attempt/<attempt>/
    /// `attempt` is the folding leader's monotonic per-round id; only the adopted attempt is reader-visible.
    String gcGenAttemptPrefix(uint64_t generation, uint64_t attempt) const
    {
        return gcGenPrefix(generation) + "attempt/" + std::to_string(attempt) + "/";
    }

    /// Per-(generation, attempt) FOLD seal (write-once): <prefix>/gc/gen/<generation>/attempt/<attempt>/fold_seal.
    String foldSealKey(uint64_t generation, uint64_t attempt) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "fold_seal";
    }

    /// One blob-target in-degree/delta run segment:
    ///   <prefix>/gc/gen/<generation>/attempt/<attempt>/blob_target/<shard>/<seq>
    String blobTargetRunKey(uint64_t generation, uint64_t attempt, uint64_t shard, uint64_t seq) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "blob_target/"
               + std::to_string(shard) + "/" + std::to_string(seq);
    }

    /// One part-manifest cleanup bundle:
    ///   <prefix>/gc/gen/<generation>/attempt/<attempt>/part_manifest_cleanup/<owner_shard>/<seq>
    String partManifestCleanupKey(uint64_t generation, uint64_t attempt, uint64_t owner_shard, uint64_t seq) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "part_manifest_cleanup/"
               + std::to_string(owner_shard) + "/" + std::to_string(seq);
    }

    /// Prefix that covers all retired-set objects of one (generation, attempt) (for list):
    ///   <prefix>/gc/gen/<generation>/attempt/<attempt>/retired/
    String gcGenAttemptRetiredPrefix(uint64_t generation, uint64_t attempt) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "retired/";
    }

    /// Retired-set key: <prefix>/gc/gen/<generation>/attempt/<attempt>/retired/<round>/<shard>
    String retiredKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const
    {
        return gcGenAttemptRetiredPrefix(generation, attempt) + std::to_string(round) + "/" + std::to_string(shard);
    }

    /// Outcomes key: <prefix>/gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>
    String outcomesKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "outcomes/" + std::to_string(round) + "/" + std::to_string(shard);
    }

    /// Prefix that covers every root-shard manifest and namespace file (GC round discovery).
    String rootsPrefix() const
    {
        return prefix + "/roots/";
    }

    /// Prefixes that cover every content object of one kind (raw object listing for fsck).
    String blobsPrefix() const { return prefix + "/blobs/"; }
    String treesPrefix() const { return prefix + "/trees/"; }

    /// Checkpoint key: <prefix>/gc/checkpoint/<version>
    String checkpointKey(uint64_t version) const
    {
        return prefix + "/gc/checkpoint/" + std::to_string(version);
    }

    /// Phase 0 (mount safety): per-server-root control subtree, keyed by the configured `server_root_id`
    /// (validated by `DB::Cas::validateServerRootId`). All four control objects live together under
    /// `<prefix>/gc/server-roots/<server_root_id>/` so a server's mount-safety state is one subtree.
    String serverRootPrefix(const String & server_root_id) const
    {
        return prefix + "/gc/server-roots/" + server_root_id + "/";
    }

    /// Pool-wide server-roots prefix: `<prefix>/gc/server-roots/`. The base of every
    /// `serverRootPrefix`; the GC heartbeat gate LISTs it to enumerate all mount objects (it must
    /// filter to keys ending in `/mount`, since `/owner` and `/epoch` objects share the subtree).
    String serverRootsPrefix() const
    {
        return prefix + "/gc/server-roots/";
    }

    /// Owner anchor: `<prefix>/gc/server-roots/<srid>/owner`.
    String ownerKey(const String & server_root_id) const
    {
        return serverRootPrefix(server_root_id) + "owner";
    }

    /// Writer-epoch fence: `<prefix>/gc/server-roots/<srid>/epoch`.
    String epochKey(const String & server_root_id) const
    {
        return serverRootPrefix(server_root_id) + "epoch";
    }

    /// Mount lease: `<prefix>/gc/server-roots/<srid>/mount`.
    String mountKey(const String & server_root_id) const
    {
        return serverRootPrefix(server_root_id) + "mount";
    }

    /// The data subtree owned by a server root: `<prefix>/roots/<srid>/`. The mount-safety
    /// empty-root precondition (Phase 0) lists this prefix; data/ref/manifest writes (Phase 1)
    /// will relocate under it.
    String serverRootDataPrefix(const String & server_root_id) const
    {
        return prefix + "/roots/" + server_root_id + "/";
    }

    /// Per-server-root content-addressed ref subtree (Phase 1 relocation target):
    /// `<prefix>/cas/refs/<srid>/`. Constructed now so the empty-root precondition (Phase 0)
    /// stays correct once Phase 1 populates it.
    String casRefsServerPrefix(const String & server_root_id) const
    {
        return prefix + "/cas/refs/" + server_root_id + "/";
    }

    /// Per-server-root content-addressed manifest subtree (Phase 1 relocation target):
    /// `<prefix>/cas/manifests/<srid>/`. Constructed now so the empty-root precondition (Phase 0)
    /// stays correct once Phase 1 populates it.
    String casManifestsServerPrefix(const String & server_root_id) const
    {
        return prefix + "/cas/manifests/" + server_root_id + "/";
    }

    /// Pool-level metadata object.
    String poolMetaKey() const
    {
        return prefix + "/_pool_meta";
    }

    /// The precommit namespace (B171): `<server-hex>/_precommits`, one shard per in-flight build keyed by
    /// `build_seq`. A precommit publishes the build's manifest tree as a ref in that shard, so GC's
    /// fold lifts the in-degree of every reachable object — replacing the revocable `cas_owner` hint.
    /// It is an ordinary namespace key-wise (`rootShardKey` works unchanged); only behavioral branches
    /// (fold pending-tolerance, precommit reclaim) key off `isPrecommitNamespace`. Recognized by its
    /// LAST segment being `_precommits` (Phase 6: relocated under the server's `roots/<server-hex>/`).
    static bool isPrecommitNamespace(const RootNamespace & ns)
    {
        return ns.string().ends_with("/_precommits");
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
            if (segment == "_manifests")
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_manifests'", s);
            /// Reserved for the precommit namespace (B171: `<server-hex>/_precommits`). A user namespace
            /// must not use the `_precommits` segment, but the precommit namespace itself is legal — it is
            /// exactly `<server-hex>/_precommits`, recognized by `isPrecommitNamespace`.
            if (segment == "_precommits" && !isPrecommitNamespace(ns))
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_precommits'", s);
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

/// The object key for a (kind, hash) — the single kind→key-shape mapping (blobs/trees).
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
    }
    throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "objectKey: unknown ObjectKind {}", static_cast<uint8_t>(kind));
}

}
