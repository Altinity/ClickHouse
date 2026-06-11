#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Common/Exception.h>
#include <array>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint8_t FORMAT_VERSION = 1;
constexpr uint32_t CORE_HEADER_LEN = 96;
constexpr uint32_t MAX_HEADER_LEN = 16384;
constexpr size_t CRC_OFFSET = 88;

constexpr uint8_t FLAG_HAS_CRITICAL_EXTENSION = 0x01;

constexpr uint16_t TLV_PROVENANCE = 0x0001;
constexpr uint16_t TLV_INTENDED_REF = 0x0002;
/// An arbitrary unknown type used (test-only) to exercise the critical fail-closed path.
constexpr uint16_t TLV_UNKNOWN_CRITICAL = 0x7f01;

/// CRC-32C (Castagnoli) lookup table, reflected polynomial 0x82F63B78. Built once at startup.
struct Crc32cTable
{
    std::array<uint32_t, 256> table;

    Crc32cTable()
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for (int k = 0; k < 8; ++k)
                crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : (crc >> 1);
            table[i] = crc;
        }
    }
};

const Crc32cTable crc32c_table;

}

uint32_t crc32c(const char * data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        const uint8_t byte = static_cast<uint8_t>(data[i]);
        crc = crc32c_table.table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

String encodeEnvelopeHeader(EnvelopeHeader & header)
{
    /// First build the TLV extension area, so we know header_len before writing the core header.
    String ext;

    if (header.provenance)
    {
        String body;
        writeLE64(body, header.provenance->created_at_ms);
        writeU128LE(body, header.provenance->creator_server_id);
        writeLE32(body, header.provenance->ch_version);
        writeU8(body, static_cast<uint8_t>(header.provenance->op));

        writeLE16(ext, TLV_PROVENANCE);
        writeLE16(ext, static_cast<uint16_t>(body.size()));
        ext += body;
    }

    if (header.intended_ref)
    {
        writeLE16(ext, TLV_INTENDED_REF);
        writeLE16(ext, static_cast<uint16_t>(header.intended_ref->size()));
        ext += *header.intended_ref;
    }

    bool critical = header.flags_has_critical_extension;
    if (header.unknown_critical_tlv)
    {
        /// Emit an unknown TLV with the critical flag set, to drive the fail-closed decode path.
        const String body = "x";
        writeLE16(ext, TLV_UNKNOWN_CRITICAL);
        writeLE16(ext, static_cast<uint16_t>(body.size()));
        ext += body;
        critical = true;
    }

    /// header_len must be 8-aligned; pad the extension area with zero bytes.
    uint32_t header_len = CORE_HEADER_LEN + static_cast<uint32_t>(ext.size());
    while (header_len % 8 != 0)
    {
        ext.push_back('\0');
        ++header_len;
    }
    header.header_len = header_len;

    String out;
    out.reserve(header_len);

    out += "CHCA";                                   /// [0,4) magic
    writeU8(out, FORMAT_VERSION);                     /// [4]   format_version
    writeU8(out, static_cast<uint8_t>(header.kind));  /// [5]   kind
    writeU8(out, header.hash_algo);                   /// [6]   hash_algo
    writeU8(out, critical ? FLAG_HAS_CRITICAL_EXTENSION : 0); /// [7] flags
    writeLE32(out, header_len);                       /// [8,12)  header_len
    writeLE32(out, header.index_len);                 /// [12,16) index_len
    writeLE64(out, header.logical_size);              /// [16,24) logical_size
    writeU128LE(out, header.logical_hash);            /// [24,40)
    writeU128LE(out, header.domain_id);               /// [40,56)
    writeU128LE(out, header.incarnation_tag);         /// [56,72)
    writeU128LE(out, header.build_id);                /// [72,88)
    writeLE32(out, 0);                                /// [88,92) crc32c placeholder (zeroed)
    writeLE32(out, 0);                                /// [92,96) reserved0

    out += ext;                                       /// [96, header_len) TLV extensions

    /// Compute crc over [0,96) with the crc field zeroed (it already is), then patch it in.
    const uint32_t crc = crc32c(out.data(), CORE_HEADER_LEN);
    out[CRC_OFFSET + 0] = static_cast<char>(crc & 0xff);
    out[CRC_OFFSET + 1] = static_cast<char>((crc >> 8) & 0xff);
    out[CRC_OFFSET + 2] = static_cast<char>((crc >> 16) & 0xff);
    out[CRC_OFFSET + 3] = static_cast<char>((crc >> 24) & 0xff);

    return out;
}

EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind)
{
    if (head_bytes.size() < CORE_HEADER_LEN)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: header too short ({} bytes, need at least {})", head_bytes.size(), CORE_HEADER_LEN);

    ByteReader r(head_bytes);

    /// [0,4) magic
    const String magic = r.readBytes(4);
    if (magic != "CHCA")
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: bad magic");

    /// [4] format_version
    const uint8_t format_version = r.readU8();
    if (format_version > FORMAT_VERSION)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
            "CHCA envelope: unsupported format_version {}", format_version);

    EnvelopeHeader h;

    /// [5] kind
    const uint8_t kind_byte = r.readU8();
    if (kind_byte != static_cast<uint8_t>(ObjectKind::Blob)
        && kind_byte != static_cast<uint8_t>(ObjectKind::Tree)
        && kind_byte != static_cast<uint8_t>(ObjectKind::Pack))
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: unknown kind {}", kind_byte);
    h.kind = static_cast<ObjectKind>(kind_byte);
    if (h.kind != expected_kind)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: kind {} does not match expected {}",
            static_cast<int>(kind_byte), static_cast<int>(expected_kind));

    /// [6] hash_algo
    h.hash_algo = r.readU8();

    /// [7] flags
    const uint8_t flags = r.readU8();
    const bool has_critical = (flags & FLAG_HAS_CRITICAL_EXTENSION) != 0;
    h.flags_has_critical_extension = has_critical;

    /// [8,12) header_len
    const uint32_t header_len = r.readLE32();
    if (header_len < CORE_HEADER_LEN || header_len > MAX_HEADER_LEN || (header_len % 8) != 0)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: invalid header_len {}", header_len);
    if (head_bytes.size() < header_len)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: header_len {} exceeds provided bytes {}", header_len, head_bytes.size());
    h.header_len = header_len;

    /// [12,16) index_len
    h.index_len = r.readLE32();
    if (h.kind != ObjectKind::Pack && h.index_len != 0)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: index_len must be 0 for kind {}", static_cast<int>(kind_byte));

    /// [16,24) logical_size
    h.logical_size = r.readLE64();

    /// [24,40) [40,56) [56,72) [72,88)
    h.logical_hash = r.readU128LE();
    h.domain_id = r.readU128LE();
    h.incarnation_tag = r.readU128LE();
    h.build_id = r.readU128LE();

    /// [88,92) crc32c
    const uint32_t stored_crc = r.readLE32();
    /// [92,96) reserved0
    const uint32_t reserved0 = r.readLE32();
    if (reserved0 != 0)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: reserved0 must be 0");

    /// Verify crc over [0,96) with the crc field zeroed.
    {
        std::array<char, CORE_HEADER_LEN> core{};
        std::memcpy(core.data(), head_bytes.data(), CORE_HEADER_LEN);
        core[CRC_OFFSET + 0] = 0;
        core[CRC_OFFSET + 1] = 0;
        core[CRC_OFFSET + 2] = 0;
        core[CRC_OFFSET + 3] = 0;
        const uint32_t computed = crc32c(core.data(), CORE_HEADER_LEN);
        if (computed != stored_crc)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: header_crc32c mismatch (computed {:08x}, stored {:08x})", computed, stored_crc);
    }

    /// Size arithmetic (uniform for ALL kinds, spec §3.1): header_len + logical_size == object_size.
    /// logical_size covers [header_len, EOF); for packs that INCLUDES the index
    /// (logical_size = index_len + payload_region_size).
    const uint64_t expected_object_size = static_cast<uint64_t>(header_len) + h.logical_size;
    if (expected_object_size != object_size)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: size mismatch (header_len {} + logical_size {} = {}, object_size {})",
            header_len, h.logical_size, expected_object_size, object_size);
    if (h.index_len > h.logical_size)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: index_len {} exceeds logical_size {}", h.index_len, h.logical_size);

    /// [96, header_len) TLV extensions. Encode pads the area to 8-alignment with zero bytes; a valid
    /// TLV type is never 0, so a zero type marks the start of alignment padding (which must be all
    /// zero and shorter than a minimal 4-byte TLV header).
    bool saw_unknown_critical = false;
    while (header_len - r.position() >= 4)
    {
        const uint16_t type = r.readLE16();
        if (type == 0)
        {
            /// The 2 type bytes we just consumed are the first padding bytes (already zero); the
            /// rest of the area is verified zero by the trailing check below.
            break;
        }
        const uint16_t len = r.readLE16();
        if (r.position() + len > header_len)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: TLV length overruns header");

        if (type == TLV_PROVENANCE)
        {
            ByteReader pr(head_bytes.substr(r.position(), len));
            Provenance p;
            p.created_at_ms = pr.readLE64();
            p.creator_server_id = pr.readU128LE();
            p.ch_version = pr.readLE32();
            p.op = static_cast<ProvenanceOp>(pr.readU8());
            h.provenance = p;
            for (uint16_t i = 0; i < len; ++i)
                r.readU8();
        }
        else if (type == TLV_INTENDED_REF)
        {
            h.intended_ref = r.readBytes(len);
        }
        else
        {
            /// Unknown TLV. Fail closed only if the critical flag is set; otherwise skip.
            if (has_critical)
                saw_unknown_critical = true;
            for (uint16_t i = 0; i < len; ++i)
                r.readU8();
        }
    }

    /// Any bytes left (either after hitting padding, or the < 4 trailing bytes) must be zero padding.
    while (r.position() < header_len)
    {
        if (r.readU8() != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: nonzero TLV padding");
    }

    if (saw_unknown_critical)
        throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
            "CHCA envelope: unknown critical extension present (fail closed)");

    return h;
}

}
