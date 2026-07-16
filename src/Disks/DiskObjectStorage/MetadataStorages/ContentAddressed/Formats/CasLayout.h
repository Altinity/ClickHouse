#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
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

/// Forward-declared only: `CasBlobDigest.h` (`BlobDigest`, and via it `BlobRef` in `CasBlobRef.h`)
/// already depends back on this header (`CasBlobDigest.h` -> `CasPoolMeta.h` -> `CasLayout.h`) --
/// including it here would cycle. The `blobKey(const BlobRef &)` overload below needs it only by
/// const reference, so the forward declaration is enough for the header; its definition (which needs
/// the complete type) lives in `CasLayout.cpp`, which includes `CasBlobRef.h` directly -- a `.cpp`
/// has no such cycle.
struct BlobRef;

/// Which of the three immutable ref-object kinds a `_cleanup`/`_log`/`_snap` key names (spec §Object
/// Layout). Lexical order of the directory segments matches enum declaration order: `_cleanup` <
/// `_log` < `_snap`.
enum class RefObjectKind : uint8_t
{
    Cleanup,
    Log,
    Snap,
};

/// The result of `Layout::parseRefObjectKey`: which table (`ns`), which of the three ref-object kinds,
/// and its `RefTxnId`.
struct ParsedRefObjectKey
{
    RootNamespace ns;
    RefObjectKind kind;
    RefTxnId txn_id;

    bool operator==(const ParsedRefObjectKey &) const = default;
};

/// Pure key-construction functions for a content-addressed pool.
///
/// Every key is built from a pool prefix and a stable path sub-tree (POOL = pool prefix, S = 2-char shard, ID = full id):
///   - content objects:  POOL/blobs/S/ID
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
///
/// Blob bodies carry their OWN `BlobHashAlgo` as a path segment (mixed-algo pools, Phase 3 T2/T3):
/// `POOL/blobs/<algo>/S/<hex>`, where `<algo>` is `blobHashAlgoName(ref.algo)` (`"ch128"`, `"xxh3"`,
/// `"sha256"`) and `<hex>`/`S` are taken from `ref.digest` at the algo's own width. This applies to
/// ALL algos, including the pool's default, so blob keys are uniformly self-describing and a pool may
/// hold blobs under several algos at once. Trees/manifests/refs/gc keys are unaffected -- only
/// blob-body keys (`blobKey`/`blobMetaKey`/`objectKey`) gain the segment. `Layout` itself carries NO
/// algo -- there is no pool-wide "the" algo anymore; every blob key is built from a `BlobRef` alone.
class Layout
{
public:
    explicit Layout(String prefix_) : prefix(std::move(prefix_)) {}

    /// Content objects: POOL/blobs/<algo>/S/<hex>, with `<algo>`/`<hex>` taken from `ref` itself.
    /// Defined out-of-line in CasLayout.cpp: `BlobRef`'s complete type comes through
    /// `CasBlobDigest.h` -> `CasPoolMeta.h` -> `CasLayout.h`, so this header cannot include it
    /// directly without cycling (mirrors the pre-existing `objectKey` cyclic-include workaround).
    String blobKey(const BlobRef & ref) const;
    /// The per-hash meta descriptor sibling of the blob body (spec §raw-body-refinement).
    String blobMetaKey(const BlobRef & ref) const;

    /// Inverse of `blobKey`/`blobMetaKey` (Phase 3 T5): parses a LISTED object key of the shape
    /// `<prefix>/blobs/<algoName>/<shard2>/<hex>` (the `.meta` sibling is accepted identically -- its
    /// trailing `.meta` is stripped first, so a body and its meta parse to the SAME `BlobRef`).
    /// Returns `std::nullopt` for anything that is not one of OUR blob keys: a foreign top-level
    /// prefix, a missing shard/hex segment, an `<algoName>` this build does not recognize
    /// (`blobHashAlgoName` never rendered it), a hex payload of the wrong width for a KNOWN algo, or
    /// non-hex characters -- every case is "debris, not ours", never an exception (callers classify
    /// it as foreign/unaccounted, mirroring the LIST sweep's existing `catch (...) continue`
    /// contract). Defined out-of-line in `CasLayout.cpp` for the same cyclic-include reason as
    /// `blobKey`/`blobMetaKey` above.
    std::optional<BlobRef> parseBlobKey(std::string_view key) const;

    /// Namespace-scoped ref-object prefix: `<prefix>/cas/refs/<ns>/` — the parent of a table's
    /// `_log`/`_snap`/`_cleanup` objects. One LIST of it enumerates the table's present ref objects
    /// (recovery, orphan-sweep tail read).
    String refsNamespacePrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/cas/refs/" + ns.string() + "/";
    }

    /// Pool-wide ref-object prefix (Phase 1): `<prefix>/cas/refs/`. The base of every ref object, used
    /// by GC's one global discovery LIST and the strip-to-namespace step.
    String casRefsPrefix() const
    {
        return prefix + "/cas/refs/";
    }

    /// Immutable transaction log object (spec §Object Layout):
    /// `<prefix>/cas/refs/<ns>/_log/<render>.zst`. codecs-v3 phase 3: the log is the text `cas_ref_log`
    /// under the Always/`.zst` `storedSuffix` (the point-GET constructs the key with the suffix; no try-both).
    String refLogKey(const RootNamespace & ns, const RefTxnId & id) const
    {
        return refsNamespacePrefix(ns) + "_log/" + renderRefTxnId(id) + String(storedSuffix(FormatId::RefLog));
    }

    /// Writer-published table snapshot (spec §Object Layout): `.../_snap/<render>.zst`. codecs-v3
    /// phase 3: the pre-v3 `.proto` suffix is gone — the snapshot is the text `cas_ref_snap` under the
    /// Always/`.zst` `storedSuffix`. Snapshot `X` reuses the `RefTxnId` of the last log it covers.
    String refSnapshotKey(const RootNamespace & ns, const RefTxnId & id) const
    {
        return refsNamespacePrefix(ns) + "_snap/" + renderRefTxnId(id) + String(storedSuffix(FormatId::RefSnapshot));
    }

    /// Namespace-removal completion marker (spec §Object Layout): a zero-byte object at
    /// `.../_cleanup/<render>` that GC publishes once the exact removal durably reaches `Completed`.
    String refCleanupMarkerKey(const RootNamespace & ns, const RefTxnId & id) const
    {
        return refsNamespacePrefix(ns) + "_cleanup/" + renderRefTxnId(id);
    }

    /// Inverse of `refLogKey`/`refSnapshotKey`/`refCleanupMarkerKey`: classifies a LISTED key under
    /// `casRefsPrefix()` by its kind directory (`_cleanup`, `_log`, `_snap`) and parses the trailing
    /// `RefTxnId`. Strict: returns `std::nullopt` -- never throws -- for anything that is not one of
    /// OUR ref-object keys: a foreign top-level prefix, a missing namespace/kind/id segment, an
    /// unrecognized kind directory (this also excludes a bare numeric-shard ref-key shape,
    /// `cas/refs/<ns>/<shard>`, which has no kind directory at all), a `_log`/`_snap` id missing its
    /// `.zst` suffix (codecs-v3 phase 3: both are Always-compressed text), a `_cleanup` id carrying any
    /// extension, trailing garbage after the id, or a non-canonical `RefTxnId` render (delegates to
    /// `parseRefTxnId`).
    std::optional<ParsedRefObjectKey> parseRefObjectKey(std::string_view key) const
    {
        const String base = casRefsPrefix();
        if (!key.starts_with(base))
            return std::nullopt;
        std::string_view rest = key;
        rest.remove_prefix(base.size());

        const size_t id_sep = rest.rfind('/');
        if (id_sep == std::string_view::npos)
            return std::nullopt;
        std::string_view id_part = rest.substr(id_sep + 1);
        std::string_view before_id = rest.substr(0, id_sep);

        const size_t kind_sep = before_id.rfind('/');
        if (kind_sep == std::string_view::npos)
            return std::nullopt;
        const std::string_view kind_seg = before_id.substr(kind_sep + 1);
        const std::string_view ns_part = before_id.substr(0, kind_sep);
        if (ns_part.empty())
            return std::nullopt;

        RefObjectKind kind;
        if (kind_seg == "_cleanup")
            kind = RefObjectKind::Cleanup;
        else if (kind_seg == "_log")
            kind = RefObjectKind::Log;
        else if (kind_seg == "_snap")
            kind = RefObjectKind::Snap;
        else
            return std::nullopt;

        std::string_view render = id_part;
        if (kind == RefObjectKind::Snap || kind == RefObjectKind::Log)
        {
            /// codecs-v3 phase 3: `_log` and `_snap` are Always-compressed text stored under a `.zst`
            /// suffix (the pre-v3 `_snap` `.proto` suffix is gone; `_log` used to carry none). `_cleanup`
            /// stays a bare zero-byte marker (non-family, uncompressed).
            constexpr std::string_view kZstSuffix = ".zst";
            if (!render.ends_with(kZstSuffix))
                return std::nullopt;
            render.remove_suffix(kZstSuffix.size());
        }
        else if (render.find('.') != std::string_view::npos)
        {
            return std::nullopt;   /// `_cleanup` ids never carry an extension
        }

        const auto txn_id = parseRefTxnId(render);
        if (!txn_id)
            return std::nullopt;

        return ParsedRefObjectKey{RootNamespace{String(ns_part)}, kind, *txn_id};
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

    /// Part manifest body key (spec §Manifest Identifier, canonical hex form):
    ///   <prefix>/cas/manifests/<ns>/<epoch-hex>-<build-seq-hex>/<000001>.zst
    /// The build-scoped directory reuses `RefTxnId`'s hex rendering for `{writer_epoch,
    /// build_sequence}` (same durable-epoch fence and hex width as a ref transaction id, per spec --
    /// a different counter with different semantics, not the same identifier). `manifest_ordinal` is a
    /// per-build ordinal rendered as a six-digit filename. `root_namespace_id` comes from the owning
    /// context (the `ManifestId`), never from the journal ref.
    String manifestKey(const ManifestId & id) const
    {
        checkNamespace(id.root_namespace);
        return prefix + "/cas/manifests/" + id.root_namespace.string() + "/"
             + renderRefTxnId(RefTxnId{id.ref.writer_epoch, id.ref.build_sequence}) + "/"
             + manifestOrdinalFileName(id.ref.manifest_ordinal);
    }

    /// Inverse of `manifestKey`: parses `<prefix>/cas/manifests/<ns>/<epoch-hex>-<seq-hex>/<NNNNNN>.zst`.
    /// Strict: rejects the old decimal directory shape (it is not two fixed-width hex fields joined by
    /// '-'), a missing namespace/build/ordinal segment, trailing garbage, a file not ending in the
    /// registered suffix or of the wrong width, and an out-of-range or non-canonical ordinal.
    /// Foreign/malformed keys return `std::nullopt`, never throw -- LIST sweep / fsck classify by key
    /// shape, not by validity. All manifest-path parsing (sweep, fsck) routes through this one function.
    std::optional<ManifestId> parseManifestKey(std::string_view key) const
    {
        const String base = casManifestsPrefix();
        if (!key.starts_with(base))
            return std::nullopt;
        std::string_view rest = key;
        rest.remove_prefix(base.size());

        const size_t file_sep = rest.rfind('/');
        if (file_sep == std::string_view::npos)
            return std::nullopt;
        const std::string_view file = rest.substr(file_sep + 1);
        const std::string_view before_file = rest.substr(0, file_sep);

        const size_t build_sep = before_file.rfind('/');
        if (build_sep == std::string_view::npos)
            return std::nullopt;
        const std::string_view build_seg = before_file.substr(build_sep + 1);
        const std::string_view ns_part = before_file.substr(0, build_sep);
        if (ns_part.empty())
            return std::nullopt;

        const auto build = parseRefTxnId(build_seg);
        if (!build)
            return std::nullopt;

        const std::string_view kManifestSuffix = storedSuffix(FormatId::PartManifest);
        constexpr size_t kOrdinalDigits = 6;
        if (file.size() != kOrdinalDigits + kManifestSuffix.size() || !file.ends_with(kManifestSuffix))
            return std::nullopt;
        const std::string_view ordinal_str = file.substr(0, kOrdinalDigits);
        uint32_t ordinal = 0;
        for (char c : ordinal_str)
        {
            if (c < '0' || c > '9')
                return std::nullopt;
            ordinal = ordinal * 10 + static_cast<uint32_t>(c - '0');
        }
        if (ordinal == 0 || ordinal > kMaxManifestOrdinal)
            return std::nullopt;

        ManifestId parsed;
        parsed.root_namespace = RootNamespace{String(ns_part)};
        parsed.ref.writer_epoch = build->writer_epoch;
        parsed.ref.build_sequence = build->ref_sequence;
        parsed.ref.manifest_ordinal = ordinal;
        return parsed;
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

    /// (partManifestCleanupKey removed in codecs-v3 phase 5 with the part-manifest cleanup RUN: the run
    /// object had no reader — manifest cleanups execute inline from the in-memory `mf_cleanup` map — so
    /// the durable bundle + its key were dead weight. The `part_manifest_cleanup/` subtree is gone.)

    /// (retiredKey removed 2026-07-10 with the retired-in-snapshot refactor — condemned state rides the
    /// source-edge runs as kCondemned rows + the fold seal's condemned_summary, so there is no separate
    /// retired-list object key. The `retired/` subtree is never written or read.)

    /// Outcomes key: <prefix>/gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>.zst
    /// The `.zst` suffix comes from the traits table (cas_gc_outcomes is the one Always-compressed
    /// control object): a constructed key names the compressed object deterministically, no body sniff.
    String outcomesKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard) const
    {
        return gcGenAttemptPrefix(generation, attempt) + "outcomes/" + std::to_string(round) + "/" + std::to_string(shard)
            + String(storedSuffix(FormatId::GcOutcomes));
    }

    /// Prefix that covers every root-shard manifest and namespace file (GC round discovery).
    String rootsPrefix() const
    {
        return prefix + "/roots/";
    }

    /// Prefix that covers every content blob (raw object listing for fsck). Deliberately stays
    /// `<prefix>/blobs/` (no algo segment) even though `blobKey` nests one level deeper under
    /// `blobs/<algo>/S/ID`: a recursive LIST of this prefix still returns every blob object across all
    /// algos in one sweep. Any code that PARSES a listed key back to a hash must take the LAST path
    /// component (the hex digest), which stays correct regardless of the algo segment (`CasGc.cpp` /
    /// `CasFsck.cpp` already do this via `rfind('/')`).
    ///
    /// S3-native staging Task 6 (verified, design §6 "GC exclusion"): the S3-staging area lives under
    /// `<prefix>/staging/<mount_id>/` — a distinct top-level sibling of `blobs/`, `cas/refs/`, and
    /// `cas/manifests/`, never a sub-path of any of them. Every GC blob-discovery LIST (`CasGc.cpp`,
    /// `CasFsck.cpp`) enumerates ONLY this `blobsPrefix()`, so a `staging/` object can never be listed,
    /// HEAD'd, or condemned as an orphan blob — `Cas::sweepOwnMountStaging` (`CasStagingSweeper.h`) is
    /// the sole reclaimer of `staging/` debris.
    String blobsPrefix() const { return prefix + "/blobs/"; }

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

    /// Public validator for a namespace reconstructed from an untrusted listed key (GC ref intake):
    /// `parseRefObjectKey` returns the namespace without checking its shape, so a
    /// consumer that will act on it must re-validate. Throws BAD_ARGUMENTS on a malformed namespace,
    /// exactly as every key-building method does.
    void validateNamespace(const RootNamespace & ns) const { checkNamespace(ns); }

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
