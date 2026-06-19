#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/Exception.h>
#include <base/types.h>
#include <charconv>
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

    /// The namespace registry (spec §4, decision 2026-06-12): the authoritative namespace
    /// universe, CAS-appended by a writer's first publish into a namespace (W-REGISTER), fenced by
    /// GC like a shard, and the source of GC discovery (never LIST). The `_registry` tail is
    /// non-numeric, so tryParseRootShardKey never classifies it as a shard manifest, and
    /// checkNamespace reserves the segment so no namespace can collide under it.
    String rootsRegistryKey() const
    {
        return prefix + "/roots/_registry";
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
    /// reserved area and must not look like a shard — the `@cas@`-gated `tryParseRootShardKey`
    /// guarantees a numeric tail here is never mis-classified. The `_files`/`_pool_meta`/`_registry`
    /// reservations still apply to its segments via the path itself (these never appear in a real
    /// ClickHouse loose-file path).
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

    /// Classifies a LISTed key as a root-shard manifest: <prefix>/roots/<namespace...>/<shard_number>.
    /// Returns nullopt for verbatim files (a `_files` segment), non-numeric tails, or foreign keys.
    /// A classifier over list output, not a validator — never throws.
    std::optional<std::pair<RootNamespace, uint64_t>> tryParseRootShardKey(const String & key) const
    {
        const String roots = rootsPrefix();
        if (!key.starts_with(roots))
            return std::nullopt;
        const std::string_view rest(key.data() + roots.size(), key.size() - roots.size());

        const size_t last_slash = rest.rfind('/');
        if (last_slash == std::string_view::npos || last_slash == 0 || last_slash + 1 == rest.size())
            return std::nullopt;                       /// need "<ns>/<tail>" with both parts non-empty

        const std::string_view tail = rest.substr(last_slash + 1);
        uint64_t shard = 0;
        const auto [end, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), shard);
        if (ec != std::errc() || end != tail.data() + tail.size())
            return std::nullopt;                       /// non-numeric (or overflow) tail is not a shard

        const std::string_view ns = rest.substr(0, last_slash);
        /// the segment immediately before the tail must not be the reserved verbatim-file area
        const size_t prev_slash = ns.rfind('/');
        const std::string_view last_ns_segment = prev_slash == std::string_view::npos ? ns : ns.substr(prev_slash + 1);
        if (last_ns_segment == "_files")
            return std::nullopt;
        if (last_ns_segment.empty())
            return std::nullopt;                       /// empty ns segment ("a//7", "//7") — Layout never writes these

        /// @cas@-scoped shard parsing (design §5.1/§5.2): a key is a root-shard manifest ONLY when its
        /// namespace's last segment is a content-addressed archive directory (`…@cas@`), or it is a
        /// legacy build-root namespace (`_builds/…`, relocated in Phase 6). A plain mountpoint object
        /// with a numeric tail and no `@cas@` ancestor (`roots/srv1/foo/7`) is an opaque ordinary file
        /// and is NEVER classified as a shard.
        const bool is_cas_archive = last_ns_segment.ends_with("@cas@");
        const bool is_legacy_build_root = ns.starts_with("_builds/");   /// removed in Phase 6
        if (!is_cas_archive && !is_legacy_build_root && !isPrecommitNamespaceSegment(last_ns_segment))
            return std::nullopt;

        return std::make_pair(RootNamespace{String(ns)}, shard);
    }

    /// True iff a namespace's last segment is the per-server precommit area (`_precommits`). Phase 1
    /// stub: returns false until Phase 6 relocates precommits under `roots/<server>/_precommits`.
    /// Until then build-root shards live at `_builds/<server_hex>/<N>`, admitted by `is_legacy_build_root`.
    static bool isPrecommitNamespaceSegment(std::string_view) { return false; }

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
            /// Reserved for the namespace registry object (roots/_registry); a namespace starting
            /// with it would nest shard manifests under the registry's own key.
            if (segment == "_registry")
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_registry'", s);
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
