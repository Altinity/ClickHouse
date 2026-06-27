#pragma once
#include <base/types.h>
#include <map>
#include <optional>

namespace DB
{

/// Purpose-built seam for DataPartsExchange (M-W design section 4): everything replication needs
/// from a content-addressed disk, nothing else. Implemented by ContentAddressedMetadataStorage;
/// obtained by dynamic_cast on the disk's IMetadataStorage — to THIS interface, never to the
/// concrete class (the design's "small purpose-built facade instead of dynamic_cast into the
/// concrete class").
///
/// Relink wire contract (B7 part-manifest model, maintainer-approved part_manifest_v1): the sender
/// transmits {pool_uuid, encoded PartManifest body}; the legacy `part_id` cookie field carries the
/// opaque manifest bytes (replica-internal wire on this branch — no protocol field changes). Sender
/// identity in the body (ManifestRef, root_namespace_id, payload_digest) is NON-AUTHORITATIVE — the
/// receiver uses ONLY the entries, runs a normal LOCAL build over the SHARED-pool blobs (adopted by
/// hash; no blob body is transferred), and publishes its OWN fresh receiver-local ManifestId. A blob
/// no longer present/condemned makes adoptPartFromManifest return false and the caller falls back to
/// the byte fetch — exactly where the old 4-step pin protocol fell back.
class IContentAddressedExchange
{
public:
    virtual ~IContentAddressedExchange() = default;

    /// The pool's stable identity (Cas::PoolMeta::pool_id, hex). Two replicas may relink iff equal
    /// — endpoint/prefix string-matching is unsafe (false positives => mis-relink). Empty before
    /// the storage started up.
    virtual const String & getPoolUUID() const = 0;

    /// Sender side: THIS server's committed part's encoded `PartManifest` body (the opaque payload
    /// the receiver decodes) for the given disk-relative part path. nullopt = the part is not a
    /// committed content-addressed part here => no relink offer; the sender streams bytes.
    virtual std::optional<String> getPartManifestBytes(const String & part_path) const = 0;

    /// Receiver side: decode the transferred manifest body, run a normal LOCAL build over the
    /// shared-pool blobs (adopt-by-hash; NO blob body read from the sender), stage a FRESH
    /// receiver-local manifest in the RECEIVER namespace (derived from table_uuid; the sender's
    /// root_namespace_id is ignored), precommitAdd + promote it (fail-closed blob revalidation),
    /// carrying the transferred mutable per-part files. Returns true iff a local manifest was
    /// committed and the ref is live. Returns false — publishing NOTHING — on ANY retryable failure
    /// (decode/validation/stage/precommit/promote, including a condemned/absent blob: ABORTED); the
    /// caller falls back to a byte fetch.
    virtual bool adoptPartFromManifest(
        const String & table_uuid,
        const String & part_name,
        const String & manifest_bytes,
        const std::map<String, String> & mutable_files) = 0;
};

}
