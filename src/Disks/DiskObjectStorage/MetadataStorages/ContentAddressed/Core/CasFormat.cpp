#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
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
        case FormatId::RetiredSet:
        case FormatId::Watermark:
        case FormatId::PoolMeta:
        case FormatId::Roster:
        case FormatId::GcOutcomes:
        case FormatId::PartManifest:
        case FormatId::RunFile:
        case FormatId::FoldSeal:
        case FormatId::CompletionSeal:
        case FormatId::Owner:
        case FormatId::ServerEpoch:
        case FormatId::MountLease:
            return BASELINE;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: unknown FormatId {}", static_cast<int>(id));
}

uint32_t magicFor(FormatId id)
{
    /// Each magic is 4 ASCII bytes stored as little-endian fixed32.
    /// "CABL" = 0x4C424143, "CATR" = 0x52544143, etc.
    switch (id)
    {
        case FormatId::Blob:          return 0x4C424143u; /// "CABL"
        case FormatId::Manifest:      return 0x53524143u; /// "CARS"
        case FormatId::PoolMeta:      return 0x4D504143u; /// "CAPM"
        case FormatId::Watermark:     return 0x4D574143u; /// "CAWM"
        case FormatId::GcState:       return 0x54474143u; /// "CAGT"
        case FormatId::RetiredSet:    return 0x54524143u; /// "CART"
        case FormatId::GcOutcomes:    return 0x4F474143u; /// "CAGO"
        case FormatId::PartManifest:   return 0x54504143u; /// "CAPT" (NOT "CAPM"; that is PoolMeta)
        case FormatId::RunFile:        return 0x4E524143u; /// "CARN"
        case FormatId::FoldSeal:       return 0x53464143u; /// "CAFS"
        case FormatId::CompletionSeal: return 0x53434143u; /// "CACS"
        case FormatId::Owner:          return 0x574F4143u; /// "CAOW"
        case FormatId::ServerEpoch:    return 0x50454143u; /// "CAEP"
        case FormatId::MountLease:     return 0x4C4D4143u; /// "CAML"
        case FormatId::Roster:
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

}
