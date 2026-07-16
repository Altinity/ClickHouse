#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

/// Generation-1 baseline for every class. Future format changes APPEND to the matching array (and bump
/// G_BUILD): additive => {gen, prior_min_reader}; breaking => {gen, gen}. Never edit an existing entry.
constexpr FormatChangePoint BASELINE[] = {{1, 1}};

}

std::span<const FormatChangePoint> changePoints(FormatId id)
{
    /// Today every class shares the gen-1 baseline. As classes diverge, give each its own static array
    /// and switch on `id` here.
    switch (id)
    {
        case FormatId::Blob:
        case FormatId::GcState:
        case FormatId::PoolMeta:
        case FormatId::Roster:
        case FormatId::GcOutcomes:
        case FormatId::PartManifest:
        case FormatId::RunFile:
        case FormatId::FoldSeal:
        case FormatId::Owner:
        case FormatId::ServerEpoch:
        case FormatId::MountLease:
        case FormatId::RefLog:
        case FormatId::RefSnapshot:
        case FormatId::BlobMeta:
        case FormatId::GcHeartbeat:
            return BASELINE;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: unknown FormatId {}", static_cast<int>(id));
}

uint32_t currentWriterVersion()
{
    return G_BUILD;
}

uint32_t currentCompatibilityVersion()
{
    /// Pre-roster: stamp G_BUILD as the compatibility floor on every object written.
    return G_BUILD;
}

void checkCompatibility(uint32_t compatibility_version, std::string_view what)
{
    if (compatibility_version > G_BUILD)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: object requires reader generation {} but this build supports at most {}",
            what, compatibility_version, G_BUILD);
}

namespace
{
constexpr uint64_t kKiB = 1024;
constexpr uint64_t kMiB = 1024 * 1024;

/// Caps are 100-1000x above realistic sizes (hitting one is corrupt object or protocol bug).
/// RefLog/RefSnapshot caps were re-derived for JSON in phase 3: object_cap 64 MiB (decompressed)
/// comfortably covers the JSON-inflated removal-class txn / full snapshot -- the codec self-checks
/// `ref_txn_max_bytes` = 1 MiB / `ref_removal_max_bytes` = `ref_snapshot_max_bytes` = 64 MiB over the
/// text before sealing. line_cap == object_cap here (not a smaller streaming-style cap): these are
/// WHOLE-READ formats (the object is fully materialized under object_cap), so a per-line cap adds no
/// memory protection of its own -- line_cap exists to bound O(line) STREAMING formats. A line_cap
/// below the object budget would create a write/read split: `admits` measures whole-txn text against
/// the object budget with no per-line check, so a single ref payload near the budget would be
/// ACCEPTED on write and then REJECTED on decode -- a persisted, undecodable, self-inflicted wedge of
/// the ref lane. The binary codec these replace had no per-record cap either; matching that capacity
/// is a re-encode, not a new restriction.
/// Compression policy is per-type and deterministic (no size threshold): Always = the object can
/// grow large and is stored under a `.zst` key suffix; PinnedRaw = deterministic byte-adoption
/// formats; Never = always-small singletons, bare cat-able.
constexpr FormatTraits TRAITS[] =
{
    {FormatId::Blob,         "cas_blob",          TextFamily::PayloadHybrid, KeyStrictness::Tolerant, CompressionPolicy::Never,     256,        256},
    {FormatId::BlobMeta,     "cas_blob_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::PoolMeta,     "cas_pool_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::RefLog,       "cas_ref_log",       TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Always,    64 * kMiB,  64 * kMiB},
    {FormatId::RefSnapshot,  "cas_ref_snap",      TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Always,    64 * kMiB,  64 * kMiB},
    {FormatId::PartManifest, "cas_part_manifest", TextFamily::PayloadHybrid, KeyStrictness::Tolerant, CompressionPolicy::Always,    256 * kMiB, 64 * kKiB},
    {FormatId::RunFile,      "cas_run",           TextFamily::RecordStream,  KeyStrictness::Strict,   CompressionPolicy::PinnedRaw, 0,          4 * kKiB},
    {FormatId::FoldSeal,     "cas_fold_seal",     TextFamily::Control,       KeyStrictness::Strict,   CompressionPolicy::PinnedRaw, 256 * kMiB, 64 * kKiB},
    {FormatId::GcState,      "cas_gc_state",      TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::GcHeartbeat,  "cas_gc_hb",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::GcOutcomes,   "cas_gc_outcomes",   TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Always,    256 * kMiB, 64 * kKiB},
    {FormatId::Owner,        "cas_owner",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::ServerEpoch,  "cas_epoch",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::MountLease,   "cas_mount_lease",   TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
};
}

const FormatTraits & traitsFor(FormatId id)
{
    for (const FormatTraits & t : TRAITS)
        if (t.id == id)
            return t;
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: no traits for FormatId {} (reserved?)", static_cast<uint16_t>(id));
}

const FormatTraits * traitsForType(std::string_view type)
{
    for (const FormatTraits & t : TRAITS)
        if (t.type == type)
            return &t;
    return nullptr;
}

std::string_view storedSuffix(FormatId id)
{
    return traitsFor(id).compression == CompressionPolicy::Always ? ".zst" : "";
}

}
