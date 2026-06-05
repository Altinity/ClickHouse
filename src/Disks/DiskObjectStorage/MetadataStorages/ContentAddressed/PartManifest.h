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
///
/// The match is on the LAST path component (basename), not the whole string: a part-relative file is
/// usually a bare name (e.g. `metadata_version.txt`), but a DETACHED part carries its files under a
/// `<detached_part>/` prefix inside the shared `detached` ref (e.g. `attaching_all_0_0_0/metadata_version.txt`,
/// B36/B46). Both must route to the per-ref sidecar; matching only the bare string left the prefixed
/// detached form unrecognized, so loading the detached part's `metadata_version.txt` (the on-fly RENAME
/// COLUMN reconciliation on ATTACH) fell back to the table's current version and skipped the rename (B62).
constexpr bool isMutablePerPartFile(std::string_view file)
{
    const auto slash = file.rfind('/');
    const std::string_view basename = (slash == std::string_view::npos) ? file : file.substr(slash + 1);
    /// The atomic write of a mutable per-part file goes via a sibling tmp (e.g. txn_version.txt.tmp,
    /// VersionMetadataOnDisk). Treat that tmp as mutable too, so it stages inline in the per-ref sidecar
    /// instead of content-addressing into the manifest — otherwise a standalone autocommit write of the
    /// tmp on an already-committed part would republish the part with a one-file manifest (data loss).
    std::string_view stem = basename;
    if (stem.ends_with(".tmp"))
        stem = stem.substr(0, stem.size() - 4);
    for (const auto & name : kMutablePerPartFiles)
        if (basename == name || stem == name)
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
