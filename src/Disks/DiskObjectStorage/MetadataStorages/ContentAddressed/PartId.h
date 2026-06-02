#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <map>
#include <string>

namespace DB::ContentAddressed
{

/// Compute the deterministic content-addressed part identifier from a part's blob map.
///
/// SipHash-128 (lowercase hex) over the sorted (logical_file, blob.checksum) pairs, mirroring
/// the `MergeTreeDataPartChecksums::getTotalChecksumUInt128` semantics (SipHash over name + hash)
/// but over our string map and only the deterministic subset of files.
///
/// The non-deterministic/mutable files uuid.txt, txn_version.txt and metadata_version.txt are
/// excluded so that two parts with identical column data but a different UUID or mutation/txn
/// version still resolve to the same part_id (and thus deduplicate).
std::string computePartId(const std::map<std::string, BlobEntry> & blobs);

}
