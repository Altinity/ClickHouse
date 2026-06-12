#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

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
/// The format is on the shared codec: `MAGIC(4) + version(1)` then a varint count and (name, bytes)
/// length-prefixed string pairs, all explicitly little-endian and fail-closed on an unknown version.
struct RefSidecar
{
    std::map<std::string, std::string> files;

    /// CA GC S3 (#6) — the resolved generations the commit's `+` settled on, recorded per-part so the
    /// DROP path emits its `-` at the SAME generation the `+` used (re-deriving from the racy `active`
    /// hint would mis-key after an intervening resurrection, leaving the old generation's count >0
    /// forever). `manifest_generation` is the part manifest's `mg`; `pin_generations` maps each pinned
    /// bare blob-hash string to its resolved `g`. Empty on a mutable-only/legacy sidecar (every g=0).
    uint64_t manifest_generation = 0;
    std::map<std::string, uint64_t> pin_generations;

    std::string serialize() const;
    static RefSidecar deserialize(const std::string & bytes);

    /// 4-byte magic `CASC` ("Content-Addressed SideCar") + a 1-byte version, per the shared codec.
    static constexpr FormatMagic MAGIC = makeMagic("CASC");
    /// Version 2 (CA GC S3 #6) appends manifest_generation + the (blob-hash -> g) map. A v3 pool is
    /// created fresh (PoolMeta v3, no back-compat), so no v1 sidecar can exist in it — reading only v2
    /// is correct and fail-closed.
    static constexpr uint8_t VERSION = 2;
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
