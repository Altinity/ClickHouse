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
        case FormatId::Tree:
        case FormatId::Manifest:
        case FormatId::GcSnap:
        case FormatId::GcState:
        case FormatId::RetiredSet:
        case FormatId::Watermark:
        case FormatId::PoolMeta:
        case FormatId::Roster:
            return BASELINE;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: unknown FormatId {}", static_cast<int>(id));
}

WriterStamp currentWriterVersion(FormatId id, uint16_t floor)
{
    const auto cps = changePoints(id);
    /// Newest change-point with generation <= floor (cps is oldest-first, non-empty, gen[0] == 1).
    const FormatChangePoint * chosen = &cps.front();
    for (const auto & cp : cps)
    {
        if (cp.generation <= floor)
            chosen = &cp;
        else
            break;
    }
    return {chosen->generation, chosen->min_reader};
}

void gateOnRead(uint16_t min_reader_version, std::string_view what)
{
    if (min_reader_version > G_BUILD)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: object requires reader generation {} but this build supports at most {}",
            what, min_reader_version, G_BUILD);
}

}
