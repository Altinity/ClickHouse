#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
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
        case FormatId::Manifest:
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

uint32_t magicFor(FormatId id)
{
    /// Each magic is 4 ASCII bytes stored as little-endian fixed32.
    /// "CABL" = 0x4C424143, "CARS" = 0x53524143, etc.
    switch (id)
    {
        case FormatId::Blob:          return 0x4C424143u; /// "CABL"
        case FormatId::Manifest:      return 0x53524143u; /// "CARS"
        case FormatId::PoolMeta:      return 0x4D504143u; /// "CAPM"
        case FormatId::GcState:       return 0x54474143u; /// "CAGT"
        case FormatId::GcOutcomes:    return 0x4F474143u; /// "CAGO"
        case FormatId::PartManifest:   return 0x54504143u; /// "CAPT" (NOT "CAPM"; that is PoolMeta)
        case FormatId::RunFile:        return 0x4E524143u; /// "CARN"
        case FormatId::FoldSeal:       return 0x53464143u; /// "CAFS"
        case FormatId::Owner:          return 0x574F4143u; /// "CAOW"
        case FormatId::ServerEpoch:    return 0x50454143u; /// "CAEP"
        case FormatId::MountLease:     return 0x4C4D4143u; /// "CAML"
        case FormatId::Roster:
        case FormatId::RefLog:
        case FormatId::RefSnapshot:
        case FormatId::BlobMeta:
        case FormatId::GcHeartbeat:
            break;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR,
        "CasFormat: no magic defined for FormatId {}", static_cast<int>(id));
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

void gateOnRead(uint32_t compatibility_version, std::string_view what)
{
    checkCompatibility(compatibility_version, what);
}

namespace
{
constexpr uint64_t kKiB = 1024;
constexpr uint64_t kMiB = 1024 * 1024;

/// Caps are 100-1000x above realistic sizes (hitting one is corrupt object or protocol bug).
/// RefLog/RefSnapshot caps are provisional until phase 3 re-derives the byte budgets for JSON.
/// Compression policy is per-type and deterministic (no size threshold): Always = the object can
/// grow large and is stored under a `.zst` key suffix; PinnedRaw = deterministic byte-adoption
/// formats; Never = always-small singletons, bare cat-able.
constexpr FormatTraits TRAITS[] =
{
    {FormatId::Blob,         "cas_blob",          TextFamily::PayloadHybrid, KeyStrictness::Tolerant, CompressionPolicy::Never,     256,        256},
    {FormatId::BlobMeta,     "cas_blob_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::PoolMeta,     "cas_pool_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::RefLog,       "cas_ref_log",       TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Always,    64 * kMiB,  64 * kKiB},
    {FormatId::RefSnapshot,  "cas_ref_snap",      TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Always,    64 * kMiB,  64 * kKiB},
    {FormatId::Manifest,     "cas_ref_shard",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     64 * kMiB,  64 * kKiB},
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
