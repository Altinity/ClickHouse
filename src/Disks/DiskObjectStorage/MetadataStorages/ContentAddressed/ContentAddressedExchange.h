#pragma once

#include <base/types.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <optional>

namespace DB
{

class ReadPipeline;
struct ReadSettings;

/// Purpose-built seam for `DataPartsExchange`: it exposes everything replication needs from a
/// content-addressed disk and nothing else. `ContentAddressedMetadataStorage` implements it, and
/// the exchange obtains the interface by casting the disk's `IMetadataStorage` to this interface
/// rather than depending on the concrete storage class. Keeping this boundary narrow prevents the
/// replication path from becoming coupled to content-addressed storage internals.
///
/// Relink wire contract: the sender transmits `{pool_uuid, encoded PartManifest body}`. The
/// replica-internal `part_id` cookie carries the opaque manifest bytes, so this exchange does not
/// add a protocol field. The manifest's sender-specific identity (`ManifestRef`, `root_namespace_id`,
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

    /// Sender side: returns the encoded `PartManifest` body for this server's committed part at the
    /// given disk-relative path. The body is opaque to the exchange caller and is decoded by the
    /// receiver. `nullopt` means that the path is not a committed content-addressed part here, so
    /// the sender must make no relink offer and streams the part bytes instead.
    virtual std::optional<String> getPartManifestBytes(const String & part_path) const = 0;

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
