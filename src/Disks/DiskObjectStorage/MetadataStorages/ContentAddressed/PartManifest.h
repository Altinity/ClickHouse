#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace DB::ContentAddressed
{

/// The canonical set of MUTABLE per-part files: files whose bytes differ between two parts that are
/// otherwise byte-identical in their column data (so they MUST NOT contribute to the part identity,
/// and MUST NOT be shared through the content-addressed manifest). This is the single source of
/// truth: computePartId excludes exactly this set from the part_id, and ContentAddressedTransaction
/// stores exactly this set in the per-ref sidecar instead of the manifest. Keeping one constant here
/// makes "excluded from identity" and "stored per-ref" the same concept by construction (B23).
inline constexpr std::array<std::string_view, 3> kMutablePerPartFiles{
    "uuid.txt", "txn_version.txt", "metadata_version.txt"};

/// The canonical mutable-per-part-file set (see kMutablePerPartFiles).
constexpr const std::array<std::string_view, 3> & mutablePerPartFiles()
{
    return kMutablePerPartFiles;
}

/// True iff file is a mutable per-part file (see kMutablePerPartFiles). The ONE predicate shared by
/// computePartId (exclude from identity) and the transaction/read path (route to the per-ref sidecar).
constexpr bool isMutablePerPartFile(std::string_view file)
{
    for (const auto & name : kMutablePerPartFiles)
        if (file == name)
            return true;
    return false;
}

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

    /// 4-byte magic `CAMF` ("Content-Addressed ManiFest") + a 1-byte version, per the shared codec.
    static constexpr FormatMagic MAGIC = makeMagic("CAMF");
    static constexpr uint8_t VERSION = 1;
};

/// Per-ref sidecar: a tiny versioned {filename -> raw bytes} blob holding a single part's MUTABLE
/// per-part files (kMutablePerPartFiles). It is ref-scoped, NOT content-addressed: two parts with
/// identical column data share ONE manifest (dedup), but each keeps its OWN sidecar so its uuid /
/// txn_version / metadata_version are private and overlaid on read. The bytes are tiny, so storing
/// them inline (not as separate blobs) is correct and keeps each part's copy distinct.
///
/// The format is versioned now so Step 4 (formal little-endian formats) only formalizes it: a 5-byte
/// magic, a u64 version, then a u64 count and (name, bytes) string pairs in the same on-object layout
/// PartManifest uses (host byte order, little-endian targets only).
struct RefSidecar
{
    std::map<std::string, std::string> files;

    std::string serialize() const;
    static RefSidecar deserialize(const std::string & bytes);

    static constexpr char MAGIC[5] = {'C', 'A', 'S', 'C', '1'};
    static constexpr uint64_t VERSION = 1;
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
