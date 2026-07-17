#pragma once

#include <base/types.h>
#include <optional>

namespace DB
{

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
};

}
