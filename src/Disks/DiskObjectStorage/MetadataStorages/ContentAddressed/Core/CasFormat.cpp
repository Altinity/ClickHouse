#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
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

void writeFramingHeader(WriteBuffer & out, std::string_view magic, WriterStamp stamp)
{
    if (magic.size() != 4)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CasFormat: framing magic must be 4 bytes, got {}", magic.size());
    out.write(magic.data(), 4);
    writeBinaryLittleEndian(stamp.writer_version, out);
    writeBinaryLittleEndian(stamp.min_reader_version, out);
}

FramingHeader readFramingHeader(ReadBuffer & in, std::string_view expected_magic, std::string_view what)
{
    if (expected_magic.size() != 4)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CasFormat: expected magic must be 4 bytes, got {}", expected_magic.size());

    char got[4];
    in.readStrict(got, 4);
    if (std::string_view(got, 4) != expected_magic)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: bad framing magic (expected '{}')", what, expected_magic);

    FramingHeader h{};
    readBinaryLittleEndian(h.writer_version, in);
    readBinaryLittleEndian(h.min_reader_version, in);
    gateOnRead(h.min_reader_version, what);
    return h;
}

bool tolerateUnknownKeys(uint16_t writer_version)
{
    return writer_version > G_BUILD;
}

}
