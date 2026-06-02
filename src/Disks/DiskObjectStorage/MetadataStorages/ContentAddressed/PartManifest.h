#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <cstdint>
#include <map>
#include <string>

namespace DB::ContentAddressed
{

struct BlobEntry
{
    /// The BARE content hash of the blob (NOT the full object key). The full key is derived on the
    /// boundary via blobKey(prefix, BlobHash); typing it as BlobHash makes that distinction a
    /// compile error instead of a data-loss bug.
    BlobHash key;
    uint64_t size = 0;
    std::string checksum;
    auto operator<=>(const BlobEntry &) const = default;
};

struct PartManifest
{
    std::map<std::string, BlobEntry> blobs;
    std::map<std::string, std::string> inlined;

    std::string serialize() const;
    static PartManifest deserialize(const std::string & bytes);

    static constexpr char MAGIC[5] = {'C', 'A', 'M', '0', '1'};
};

/// Compute the deterministic content-addressed part identifier from a part's blob map.
///
/// SipHash-128 (lowercase hex) over the sorted (logical_file, blob.checksum) pairs, mirroring
/// the `MergeTreeDataPartChecksums::getTotalChecksumUInt128` semantics (SipHash over name + hash)
/// but over our string map and only the deterministic subset of files.
///
/// The non-deterministic/mutable files uuid.txt, txn_version.txt and metadata_version.txt are
/// excluded so that two parts with identical column data but a different UUID or mutation/txn
/// version still resolve to the same part_id (and thus deduplicate). part_id is derived from the
/// manifest's content, so the two live here as one concept.
PartId computePartId(const std::map<std::string, BlobEntry> & blobs);

}
