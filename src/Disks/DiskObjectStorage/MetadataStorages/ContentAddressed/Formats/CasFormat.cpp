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

/// Generation-1 baseline for every class. A future format change appends to that class's array and
/// bumps `G_BUILD`: additive changes use the previous reader floor, while breaking changes use the
/// new generation as the floor. Existing entries are immutable history.
constexpr FormatChangePoint BASELINE[] = {{1, 1}};

/// The two ref classes changed at generation 4 (INV-1, per-namespace contiguous ids). The change is
/// BREAKING even though not one byte of the encoding moved: a generation-3 stream's ids came from a
/// pool-wide counter and legitimately skip, which a generation-4 reader reports as corruption. The
/// floor is therefore the change generation itself.
constexpr FormatChangePoint REF_STREAM[] = {{1, 1}, {kContiguousRefStreamsGeneration, kContiguousRefStreamsGeneration}};

}

std::span<const FormatChangePoint> changePoints(FormatId id)
{
    switch (id)
    {
        case FormatId::RefLog:
        case FormatId::RefSnapshot:
            return REF_STREAM;
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
    /// Until roster-based write-down is implemented, every object carries the current build as its
    /// compatibility floor.
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

/// Caps are 100-1000x above realistic sizes; hitting one indicates a corrupt object or protocol bug.
/// `RefLog` and `RefSnapshot` objects are read whole, so their 64 MiB decompressed object cap
/// accommodates the JSON-inflated removal-class transaction and full snapshot. Their codecs
/// independently enforce the existing `ref_txn_max_bytes` (20 MiB) and 64 MiB removal/snapshot budgets
/// before sealing.
///
/// Their `line_cap` intentionally equals `object_cap`. A smaller per-line limit would add no memory
/// protection to a whole-read format, while creating a write/read split: admission measures the whole
/// transaction against the object budget, so a large individual ref payload could be accepted on
/// write and rejected on decode, leaving a persisted ref object that cannot be read. The line cap is
/// instead meaningful for streaming formats, where it bounds the resident O(line) record. Matching
/// `line_cap` to `object_cap` lets any individually valid record consume the available object budget.
///
/// Compression is per type and deterministic, with no size threshold. `Always` types can grow large
/// and use a `.zst` key suffix; `PinnedRaw` types need stable bytes for adoption; `Never` types are
/// small raw singletons.
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
