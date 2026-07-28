#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
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

/// Which immutable ref-object kind a `_cleanup`, `_log`, or `_snap` key names. The declaration order
/// follows the lexical order of the directory segments, which is useful when listed keys are grouped.
enum class RefObjectKind : uint8_t
{
    Cleanup,
    Log,
    Snap,
};

/// The result of `Layout::parseRefObjectKey`: the namespace, ref-object kind, and canonical
/// transaction identifier recovered from a listed key. The namespace is syntactically parsed here;
/// callers that will use it for an operation must call `validateNamespace` before doing so.
struct ParsedRefObjectKey
{
    RootNamespace ns;
    RefObjectKind kind;
    RefTxnId txn_id;

    bool operator==(const ParsedRefObjectKey &) const = default;
};

/// The result of `Layout::parseBlobTargetRunKey`: the (generation, attempt, shard, seq) coordinates
/// recovered from a listed blob-target in-degree/delta run segment key.
struct ParsedBlobTargetRunKey
{
    uint64_t generation = 0;
    uint64_t attempt = 0;
    uint64_t shard = 0;
    uint64_t seq = 0;

    bool operator==(const ParsedBlobTargetRunKey &) const = default;
};

/// Builds and parses object-storage keys for one content-addressed pool.
///
/// `Layout` owns only the pool prefix; it does not own storage, cache state, or a pool-wide blob hash
/// algorithm. Callers use its pure methods to derive keys for blobs, manifests, refs, verbatim files,
/// and GC control data, and to classify listed keys before acting on them. Namespace-bearing key
/// builders validate namespaces, while parsers deliberately return `std::nullopt` for foreign or
/// malformed listed keys so sweeps can treat those keys as debris without exceptions.
///
/// Every key is built from a pool prefix and a stable path subtree. The main families are:
///   - content objects:  POOL/blobs/ALGO/S/HEX
///   - part manifests:   POOL/cas/manifests/NAMESPACE/BUILD/ORDINAL.zst
///   - ref objects:      POOL/cas/refs/NAMESPACE/_log|_snap|_cleanup/ID
///   - verbatim files:   POOL/roots/NAMESPACE/_files/FILE_NAME
///   - GC state:         POOL/gc/...
///   - pool metadata:    POOL/_pool_meta
///
/// NAMESPACE is opaque to the core: the wiring composes strings like "srv1/<table_uuid>" or
/// "shadow/<backup>/<table_uuid>". The reserved "_files" and "_manifests" segments cannot collide
/// with root shard keys because shard names are numeric, and `checkNamespace` rejects both as
/// namespace segments.
///
/// The 2-char shard is always the first two characters of the id string.
/// This matches the protocol's fixed two-character shard layout.
///
/// Blob bodies carry their own `BlobHashAlgo` as a path segment:
/// `POOL/blobs/<algo>/S/<hex>`, where `<algo>` is `blobHashAlgoName(ref.algo)` (`"ch128"`, `"xxh3"`,
/// `"sha256"`) and `<hex>`/`S` are taken from `ref.digest` at the algo's own width. This applies to
/// ALL algos, including the pool's default, so blob keys are uniformly self-describing and a pool may
/// hold blobs under several algos at once. Trees/manifests/refs/gc keys are unaffected -- only
/// blob-body keys (`blobKey`/`blobMetaKey`) gain the segment. `Layout` itself carries NO
/// algo -- there is no pool-wide "the" algo anymore; every blob key is built from a `BlobRef` alone.
class Layout
{
public:
    explicit Layout(String prefix_) : prefix(std::move(prefix_)) {}

    /// Content objects: POOL/blobs/<algo>/S/<hex>, with `<algo>`/`<hex>` taken from `ref` itself.
    /// Defined out-of-line in `CasLayout.cpp`, where the key construction and parsing helpers remain
    /// together.
    String blobKey(const BlobRef & ref) const;
    /// The per-hash meta descriptor sibling of the blob body.
    String blobMetaKey(const BlobRef & ref) const;

    /// Inverse of `blobKey`/`blobMetaKey`: parses a listed object key of the shape
    /// `<prefix>/blobs/<algoName>/<shard2>/<hex>` (the `.meta` sibling is accepted identically -- its
    /// trailing `.meta` is stripped first, so a body and its meta parse to the SAME `BlobRef`).
    /// Returns `std::nullopt` for anything that is not one of OUR blob keys: a foreign top-level
    /// prefix, a missing shard/hex segment, an `<algoName>` this build does not recognize
    /// (`blobHashAlgoName` never rendered it), a hex payload of the wrong width for a KNOWN algo, or
    /// non-hex characters -- every case is "debris, not ours", never an exception (callers classify
    /// it as foreign/unaccounted, mirroring the LIST sweep's existing `catch (...) continue`
    /// contract). Defined out-of-line in `CasLayout.cpp` alongside `blobKey` and `blobMetaKey`.
    std::optional<BlobRef> parseBlobKey(std::string_view key) const;

    /// Namespace-scoped ref-object prefix: `<prefix>/cas/refs/<ns>/` — the parent of a table's
    /// `_log`/`_snap`/`_cleanup` objects. One LIST of it enumerates the table's present ref objects
    /// (recovery, orphan-sweep tail read).
    String refsNamespacePrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/cas/refs/" + ns.string() + "/";
    }

    /// Pool-wide ref-object prefix: `<prefix>/cas/refs/`. The base of every ref object, used
    /// by GC's one global discovery LIST and the strip-to-namespace step.
    String casRefsPrefix() const
    {
        return prefix + "/cas/refs/";
    }

    /// Immutable transaction log object at
    /// `<prefix>/cas/refs/<ns>/_log/<render>.zst`. The log is the text `cas_ref_log` stored with the
    /// format's always-compressed `.zst` suffix; readers construct the one canonical key and do not
    /// try an uncompressed variant.
    String refLogKey(const RootNamespace & ns, const RefTxnId & id) const
    {
        return refsNamespacePrefix(ns) + "_log/" + renderRefTxnId(id) + String(storedSuffix(FormatId::RefLog));
    }

    /// Writer-published table snapshot at `.../_snap/<render>.zst`. The snapshot
    /// is the text `cas_ref_snap` stored with the format's always-compressed `.zst` suffix. Snapshot
    /// `X` reuses the `RefTxnId` of the last log it covers.
    String refSnapshotKey(const RootNamespace & ns, const RefTxnId & id) const
    {
        return refsNamespacePrefix(ns) + "_snap/" + renderRefTxnId(id) + String(storedSuffix(FormatId::RefSnapshot));
    }

    /// The namespace's checkpoint object (spec INV-4) at `<prefix>/cas/refs/<ns>/_ckpt`. UNLIKE every
    /// other ref object it is MUTABLE (token-CAS), carries no transaction id, and therefore lives
    /// directly under the namespace prefix rather than in a `_log`/`_snap`/`_cleanup` kind directory --
    /// which is also why `parseRefObjectKey` does not recognize it and `parseRefCkptKey` exists.
    /// Stage A shape; Stage B re-keys it under the namespace's incarnation.
    String refCkptKey(const RootNamespace & ns) const
    {
        return refsNamespacePrefix(ns) + "_ckpt" + String(storedSuffix(FormatId::RefCkpt));
    }

    /// Inverse of `refCkptKey`: returns the namespace when `key` is exactly one of OUR `_ckpt` keys,
    /// and `std::nullopt` for anything else -- never throws, for the same reason `parseRefObjectKey`
    /// does not: classifying an untrusted listed key is an ordinary "is this ours" question. The
    /// namespace is syntactically parsed here; a caller that will USE it must `validateNamespace` first.
    ///
    /// Every sweep over `casRefsPrefix()` must consult BOTH this and `parseRefObjectKey` before calling
    /// a key unrecognized: a `_ckpt` is a legitimate ref object, and `groupRefKeys` treats an
    /// unrecognized one as corruption that aborts ref folding for the whole round.
    std::optional<RootNamespace> parseRefCkptKey(std::string_view key) const;

    /// Namespace-removal completion marker: a zero-byte object at
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
    /// `.zst` suffix (both are always-compressed text), a `_cleanup` id carrying any
    /// extension, trailing garbage after the id, or a non-canonical `RefTxnId` render (delegates to
    /// `parseRefTxnId`). Defined out-of-line in `CasLayout.cpp`, where the key construction and
    /// parsing helpers remain together.
    std::optional<ParsedRefObjectKey> parseRefObjectKey(std::string_view key) const;

    /// Prefix that covers all part manifests of a namespace: `<prefix>/cas/manifests/<ns>/`.
    String manifestNamespacePrefix(const RootNamespace & ns) const
    {
        checkNamespace(ns);
        return prefix + "/cas/manifests/" + ns.string() + "/";
    }

    /// Pool-wide part-manifest prefix: `<prefix>/cas/manifests/`.
    String casManifestsPrefix() const
    {
        return prefix + "/cas/manifests/";
    }

    /// Verbatim (non-content-addressed) file stored under a namespace. Names may be nested
    /// (relative sub-paths — the wiring stores table-level subdirectory files such as
    /// deduplication_logs/deduplication_log_1.txt verbatim); empty segments, leading or
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

    /// Part manifest body key, in canonical hex form:
    ///   <prefix>/cas/manifests/<ns>/<epoch-hex>-<build-seq-hex>/<000001>.zst
    /// The build-scoped directory reuses `RefTxnId`'s hex rendering for `{writer_epoch,
    /// build_sequence}` (same durable-epoch fence and hex width as a ref transaction id -- a different
    /// counter with different semantics, not the same identifier). `manifest_ordinal` is a
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
    /// Defined out-of-line in `CasLayout.cpp`, where the key construction and parsing helpers remain
    /// together.
    std::optional<ManifestId> parseManifestKey(std::string_view key) const;

    /// A plain mountpoint object is a loose, non-content-addressed file mirrored at its
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

    /// GC heartbeat (advisory liveness pulse): `<prefix>/gc/hb`.
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

    /// Inverse of `blobTargetRunKey`: parses
    /// `<prefix>/gc/gen/<generation>/attempt/<attempt>/blob_target/<shard>/<seq>`. Strict: rejects a
    /// missing/foreign top-level prefix, a missing `attempt`/`blob_target` literal segment, a missing
    /// generation/attempt/shard/seq segment, a non-canonical decimal component (empty, non-digit, a
    /// leading zero other than a bare "0", or a value that overflows `uint64_t`), or trailing garbage.
    /// Foreign/malformed keys return `std::nullopt`, never throw -- callers (`ca-inspect`) classify by
    /// key shape, not by validity. Defined out-of-line in `CasLayout.cpp`, where the key construction
    /// and parsing helpers remain together.
    std::optional<ParsedBlobTargetRunKey> parseBlobTargetRunKey(std::string_view key) const;

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
    /// The S3-staging area lives under `<prefix>/staging/<mount_id>/` — a distinct top-level sibling of
    /// `blobs/`, `cas/refs/`, and
    /// `cas/manifests/`, never a sub-path of any of them. Every GC blob-discovery LIST (`CasGc.cpp`,
    /// `CasFsck.cpp`) enumerates ONLY this `blobsPrefix()`, so a `staging/` object can never be listed,
    /// HEAD'd, or condemned as an orphan blob — `Cas::sweepOwnMountStaging` (`CasStagingSweeper.h`) is
    /// the sole reclaimer of `staging/` debris.
    String blobsPrefix() const { return prefix + "/blobs/"; }

    /// Per-server-root control subtree, keyed by the configured `server_root_id`
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

    /// The data subtree owned by a server root: `<prefix>/roots/<srid>/`. The mount-safety empty-root
    /// precondition lists this prefix before data, ref, or manifest writes are admitted.
    String serverRootDataPrefix(const String & server_root_id) const
    {
        return prefix + "/roots/" + server_root_id + "/";
    }

    /// Per-server-root content-addressed ref subtree: `<prefix>/cas/refs/<srid>/`. It is included in
    /// the mount-safety empty-root check even before the first ref is written.
    String casRefsServerPrefix(const String & server_root_id) const
    {
        return prefix + "/cas/refs/" + server_root_id + "/";
    }

    /// Per-server-root content-addressed manifest subtree: `<prefix>/cas/manifests/<srid>/`. It is
    /// included in the mount-safety empty-root check even before the first manifest is written.
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

    /// The pool's root key prefix, i.e. the constructor's `prefix_` verbatim. Exposed for diagnostics
    /// only (e.g. scoping a free-function's log line to the pool it operates on when no LoggerPtr is
    /// threaded that deep) -- no key-building method needs this, they already have `prefix` in scope.
    const String & poolPrefix() const { return prefix; }

private:
    String prefix;

    /// A namespace must be non-empty, with no leading/trailing '/', no empty segment ("//"),
    /// and no segment equal to the reserved "_files". Defined out-of-line in `CasLayout.cpp`, where
    /// the key construction and parsing helpers remain together.
    void checkNamespace(const RootNamespace & ns) const;

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
