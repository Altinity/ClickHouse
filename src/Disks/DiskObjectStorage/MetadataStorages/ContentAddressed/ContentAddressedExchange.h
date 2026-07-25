#pragma once

#include <base/types.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <optional>
#include <string_view>

namespace DB
{

class ReadPipeline;
struct ReadSettings;

/// The answer of the relink confirm as it crosses the exchange seam (spec §confirm-primitive). Declared
/// here, on the narrow interface, so `DataPartsExchange` can carry the answer without including any
/// content-addressed header; `ContentAddressedMetadataStorage` maps `Cas::ConfirmAnswer` onto it.
///
/// ONLY `Yes` AUTHORIZES ANYTHING. `No` and `Unknown` are one outcome for every caller -- both mean
/// "not proven", both are `SourceProofFailed` in the spec's failure taxonomy, and neither may be used
/// to conclude anything about the source. That is not a simplification, it is a coupling: gate 1
/// evaluates the mount fence LAST (`CasRefLedger::confirmExactRef` rule 6), so a mount that has already
/// lost its fence -- and can therefore no longer speak for the namespace at all -- still answers `No`
/// for a token that does not match its last-known row. Any code that ever treats `No` as authoritative
/// knowledge (say, to skip a retry or to conclude the part is gone) makes that ordering wrong and must
/// hoist rule 6 above the row comparison first.
enum class CasConfirmAnswer : uint8_t
{
    Yes,
    No,
    Unknown,
};

/// The sender's confirm token for one relink offer (spec §wire-protocol). It rides back to the receiver
/// as a response cookie on the offer and returns verbatim as the only argument of the confirm request,
/// so the receiver never has to understand a field of it and the sender never has to remember anything
/// between the two requests. Every field is minted by the sender out of its OWN committed state; when
/// it comes back it is untrusted peer input and is used only as a lookup key -- `resolveContentAddressedConfirm`
/// answers whatever the fields happen to select, and selects nothing at all when they match nothing.
///
/// `pool_uuid` and `server_root_id` route the question to the one mount entitled to answer it (a pool
/// UUID is shared by every server root writing into the pool, so it cannot select a mount on its own);
/// `root_namespace` and `ref_name` name the binding; `manifest_ref_text` is the exact manifest the
/// offer carried; `part_name` is gate 0's key into the sender's parts set.
struct CasRelinkSourceToken
{
    String pool_uuid;
    String server_root_id;
    String root_namespace;
    String ref_name;          /// the ref the sender published the part under (`detached/<name>` for B66b)
    String part_name;         /// the MergeTree part name
    String manifest_ref_text; /// canonical `writer_epoch:build_sequence:manifest_ordinal`
};

/// The token's wire form: `car1|<f1>|…|<f6>`, each field percent-encoded down to the RFC 3986
/// unreserved set. The encoding exists because two of the fields are not character-safe as they stand
/// -- a namespace carries `/` and `@`, and `server_root_id` is whatever the operator configured -- and
/// the token has to survive both an HTTP cookie value and a URL query parameter. Percent-encoding is
/// what makes that true by construction, instead of by a character allowlist that would silently
/// disable relink for a legal-but-unusual `server_root_id`.
///
/// `nullopt` in either direction means "not a token": an empty or over-long field on the way out, and
/// on the way in a wrong version tag, a wrong field count, a malformed escape, a control character or
/// an over-long field. A refusal is never an answer about the source -- the sender simply makes no
/// offer, and the receiver's confirm is simply unproven.
std::optional<String> encodeCasRelinkSourceToken(const CasRelinkSourceToken & token);
std::optional<CasRelinkSourceToken> decodeCasRelinkSourceToken(std::string_view text);

/// Purpose-built seam for `DataPartsExchange`: it exposes everything replication needs from a
/// content-addressed disk and nothing else. `ContentAddressedMetadataStorage` implements it, and
/// the exchange obtains the interface by casting the disk's `IMetadataStorage` to this interface
/// rather than depending on the concrete storage class. Keeping this boundary narrow prevents the
/// replication path from becoming coupled to content-addressed storage internals.
///
/// Relink wire contract: the sender transmits `{pool_uuid, encoded PartManifest body, confirm token}`.
/// The replica-internal `part_id` cookie carries the opaque manifest bytes and a response cookie carries
/// the token, so this exchange adds no protocol field of its own. The manifest's sender-specific identity (`ManifestRef`, `root_namespace_id`,
/// and `payload_digest`) is not authoritative: the receiver uses only the entries, adopts references
/// to blobs in the shared pool by hash without reading blob bodies from the sender, and publishes a
/// fresh manifest in its own namespace. Every per-part file is an ordinary manifest entry, so the
/// manifest is self-contained and there is no separate sidecar or metadata-version wire field.
/// When the local manifest cannot be committed — for example a required blob body is absent at
/// precommit — adoption publishes nothing and the caller falls back to fetching the part bytes.
class IContentAddressedExchange
{
public:
    virtual ~IContentAddressedExchange() = default;

    /// The pool's stable identity (Cas::PoolMeta::pool_id, hex). Two replicas may relink iff equal
    /// — endpoint/prefix string-matching is unsafe (false positives => mis-relink). Empty before
    /// the storage started up.
    virtual const String & getPoolUUID() const = 0;

    /// Sender side, routing predicate for the confirm action (spec §wire-protocol "Routing contract"):
    /// does THIS instance own `root_namespace` under `server_root_id`? A pool UUID is shared by every
    /// server root writing into the pool, so `getPoolUUID` alone cannot select the mount that is
    /// entitled to answer for a namespace; the caller pairs the two and requires EXACTLY one match
    /// (zero or several are both `Unknown`). I/O-free and never throws in any lifecycle state -- a
    /// routing predicate that could fail would turn a misrouted question into an error instead of an
    /// unproven answer.
    virtual bool ownsNamespace(const String & server_root_id, const String & root_namespace) const = 0;

    /// Sender side, gate 1 of the relink confirm (spec §confirm-primitive), forwarded to the ledger:
    /// does `ref_name` in `root_namespace` still name EXACTLY the manifest rendered by
    /// `manifest_ref_text` (the canonical `writer_epoch:build_sequence:manifest_ordinal` form) in this
    /// writer's committed view? Read-only, performs ZERO object-store I/O, and creates nothing: a cold,
    /// evicted, recovering, busy, wedged, poisoned or unfenced table answers `Unknown` rather than
    /// doing work, because a remote peer drives this query. Never throws: an unparsable token and a
    /// disk that is not started or has reached a terminal lifecycle are `Unknown` too.
    virtual CasConfirmAnswer confirmExactRef(const String & root_namespace, const String & ref_name,
                                             const String & manifest_ref_text) const = 0;

    /// Sender side: everything one relink offer puts on the wire. The manifest body is opaque to the
    /// exchange caller and is decoded by the receiver; the token is what the receiver hands back to
    /// confirm the offer before it promotes.
    struct RelinkOffer
    {
        String manifest_bytes;
        String confirm_token;
    };

    /// Sender side: build the relink offer for this server's committed part at the given disk-relative
    /// path. `nullopt` means the path is not a committed content-addressed part here, or the token
    /// could not be minted, so the sender must make no offer and streams the part bytes instead.
    ///
    /// The manifest body and the token come out of ONE resolution of the part, and that is the point of
    /// returning them together rather than as two calls: a repoint between them would hand the receiver
    /// a token naming a manifest whose entries it never adopted, and a `Yes` for that manifest would
    /// protect the wrong blobs.
    virtual std::optional<RelinkOffer> getRelinkOffer(const String & part_path) const = 0;

    /// Receiver side: decodes the transferred manifest, performs a normal local build from shared-
    /// pool blob references without reading blob bodies from the sender, and stages a fresh manifest
    /// in the receiver namespace derived from `table_uuid`. The sender's `root_namespace_id` is
    /// ignored. The build calls `precommitAdd` and then `promote`. Promotion trusts the adopted
    /// references through the durable manifest edge — it does not re-read each blob body — the same
    /// interserver trust as an ordinary `ReplicatedMergeTree` fetch. Returns `true` only after the
    /// local manifest is committed and its ref is live. Returns `false` without publishing anything on
    /// a manifest decode failure, on a retryable promote failure (a body-absent precommit, a precommit
    /// that is no longer the live owner, or a ref conflict, reported as `ABORTED` or `NETWORK_ERROR`),
    /// or on any other error; the caller then falls back to fetching the part bytes. A blob that
    /// becomes absent or condemned after adoption is not caught here; it is an fsck-detectable
    /// invariant violation.
    virtual bool adoptPartFromManifest(
        const String & table_uuid,
        const String & part_name,
        const String & manifest_bytes) = 0;

    /// ==== `DiskObjectStorage::prepareRead` hooks ====
    /// The two CA-only reads `DiskObjectStorage::prepareRead` needs before it composes the standard
    /// object-storage pipeline. Exposed on this narrow seam (rather than only on the concrete
    /// `ContentAddressedMetadataStorage`) so `prepareRead` casts to the interface instead of coupling
    /// to the concrete storage class.

    /// The CA read entry called by `DiskObjectStorage::prepareRead` before the generic
    /// storage-objects path: serves in-manifest bytes (mutable per-part files, inline entries,
    /// verbatim namespace files) from memory. Returns false when the path is not in-manifest.
    virtual bool prepareInManifestRead(const std::string & path, const ReadSettings & settings, ReadPipeline & pipeline) const = 0;

    /// Translates a blob-backed part file to the physical blob
    /// object plus the payload WINDOW inside it (the CHCA envelope header occupies
    /// [0, payload_offset)). DiskObjectStorage::prepareRead composes the STANDARD object-storage
    /// pipeline over `object` (gather/caches/async prefetch — the same chain plain s3 disks get,
    /// so `MergeTreeReaderStream` right-mark bounds reach the object reader and its range
    /// requests stay drainable) and bounds it with the pipeline's FileView stage.
    /// nullopt = the path is not a blob-backed part file (caller falls through; absent paths
    /// then fail in getStorageObjects exactly as before).
    struct BlobViewPlan
    {
        StoredObject object;        /// physical blob key; logical path; readable extent (envelope + payload)
        size_t payload_offset = 0;  /// view left bound inside the blob
        size_t payload_end = 0;     /// view right bound (payload_offset + payload length)
    };
    /// Resolves a blob-backed path to its physical object and payload window. Returns nullopt for
    /// in-manifest, loose, directory, or otherwise unresolved paths.
    virtual std::optional<BlobViewPlan> getBlobViewPlan(const std::string & path) const = 0;
};

}
