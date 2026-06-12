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
/// Relink wire contract (D-W4): the sender transmits {pool_uuid, tree_id}; the legacy `part_id`
/// cookie field carries the tree id hex (replica-internal wire on this branch — no protocol field
/// changes). The receiver adopts by tree id and publishes its own ref; a tree no longer adoptable
/// (reclaimed) makes adoptPart return false and the caller falls back to the byte fetch — exactly
/// where the old 4-step pin protocol fell back.
class IContentAddressedExchange
{
public:
    virtual ~IContentAddressedExchange() = default;

    /// The pool's stable identity (Cas::PoolMeta::pool_id, hex). Two replicas may relink iff equal
    /// — endpoint/prefix string-matching is unsafe (false positives => mis-relink). Empty before
    /// the storage started up.
    virtual const String & getPoolUUID() const = 0;

    /// Sender side: the tree id THIS server's ref names for the given disk-relative part path
    /// (nullopt = no committed ref => no relink offer; the sender streams bytes).
    virtual std::optional<String> getPartTreeId(const String & part_path) const = 0;

    /// Receiver side: adopt-by-id + publish a ref under (table_uuid, part_name) carrying the
    /// transferred mutable per-part files. Returns false — publishing NOTHING — when the tree is
    /// not adoptable (reclaimed meanwhile); the caller falls back to a byte fetch. Retryable
    /// publish failures (ABORTED) propagate: the fetch retries like any retryable fetch error.
    virtual bool adoptPart(
        const String & table_uuid,
        const String & part_name,
        const String & tree_id_hex,
        const std::map<String, String> & mutable_files) = 0;
};

}
