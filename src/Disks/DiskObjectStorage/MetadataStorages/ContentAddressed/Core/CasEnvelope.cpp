#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/unaligned.h>
#include <array>
#include <cstring>

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
    bool critical = header.flags_has_critical_extension;
    {
        WriteBufferFromString ext_buf(ext);

        if (header.provenance)
        {
            String body;
            {
                WriteBufferFromString body_buf(body);
                writeBinaryLittleEndian(header.provenance->created_at_ms, body_buf);
                writeBinaryLittleEndian(header.provenance->creator_server_id, body_buf);
                writeBinaryLittleEndian(header.provenance->ch_version, body_buf);
                writeBinaryLittleEndian(static_cast<uint8_t>(header.provenance->op), body_buf);
                body_buf.finalize();
            }

            writeBinaryLittleEndian(TLV_PROVENANCE, ext_buf);
            writeBinaryLittleEndian(static_cast<uint16_t>(body.size()), ext_buf);
            writeString(body, ext_buf);
        }

        if (header.intended_ref)
        {
            writeBinaryLittleEndian(TLV_INTENDED_REF, ext_buf);
            writeBinaryLittleEndian(static_cast<uint16_t>(header.intended_ref->size()), ext_buf);
            writeString(*header.intended_ref, ext_buf);
        }

        if (header.unknown_critical_tlv)
        {
            /// Emit an unknown TLV with the critical flag set, to drive the fail-closed decode path.
            const String body = "x";
            writeBinaryLittleEndian(TLV_UNKNOWN_CRITICAL, ext_buf);
            writeBinaryLittleEndian(static_cast<uint16_t>(body.size()), ext_buf);
            writeString(body, ext_buf);
            critical = true;
        }

        ext_buf.finalize();
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
    {
        WriteBufferFromString out_buf(out);

        writeString("CHCA", out_buf);                                       /// [0,4) magic
        writeBinaryLittleEndian(FORMAT_VERSION, out_buf);                   /// [4]   format_version
        writeBinaryLittleEndian(static_cast<uint8_t>(header.kind), out_buf); /// [5]  kind
        writeBinaryLittleEndian(header.hash_algo, out_buf);                 /// [6]   hash_algo
        writeBinaryLittleEndian(
            static_cast<uint8_t>(critical ? FLAG_HAS_CRITICAL_EXTENSION : 0), out_buf); /// [7] flags
        writeBinaryLittleEndian(header_len, out_buf);                       /// [8,12)  header_len
        writeBinaryLittleEndian(header.index_len, out_buf);                 /// [12,16) index_len
        writeBinaryLittleEndian(header.logical_size, out_buf);              /// [16,24) logical_size
        writeBinaryLittleEndian(header.logical_hash, out_buf);              /// [24,40)
        writeBinaryLittleEndian(header.domain_id, out_buf);                 /// [40,56)
        writeBinaryLittleEndian(header.incarnation_tag, out_buf);           /// [56,72)
        writeBinaryLittleEndian(header.build_id, out_buf);                  /// [72,88)
        writeBinaryLittleEndian(static_cast<uint32_t>(0), out_buf);         /// [88,92) crc32c placeholder (zeroed)
        writeBinaryLittleEndian(static_cast<uint32_t>(0), out_buf);         /// [92,96) reserved0

        writeString(ext, out_buf);                                          /// [96, header_len) TLV extensions
        out_buf.finalize();
    }

    /// Compute crc over [0,96) with the crc field zeroed (it already is), then patch it in.
    const uint32_t crc = crc32c(out.data(), CORE_HEADER_LEN);
    unalignedStoreLittleEndian<uint32_t>(out.data() + CRC_OFFSET, crc);

    return out;
}

EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind)
{
    if (head_bytes.size() < CORE_HEADER_LEN)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
            "CHCA envelope: header too short ({} bytes, need at least {})", head_bytes.size(), CORE_HEADER_LEN);

    return decodeGuarded("envelope", [&]
    {
        ReadBufferFromMemory in(head_bytes.data(), head_bytes.size());

        /// [0,4) magic
        if (readFixedBytes(in, 4) != "CHCA")
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: bad magic");

        /// [4] format_version
        uint8_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        if (format_version > FORMAT_VERSION)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CHCA envelope: unsupported format_version {}", format_version);

        EnvelopeHeader h;

        /// [5] kind
        uint8_t kind_byte = 0;
        readBinaryLittleEndian(kind_byte, in);
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
        readBinaryLittleEndian(h.hash_algo, in);

        /// [7] flags
        uint8_t flags = 0;
        readBinaryLittleEndian(flags, in);
        const bool has_critical = (flags & FLAG_HAS_CRITICAL_EXTENSION) != 0;
        h.flags_has_critical_extension = has_critical;

        /// [8,12) header_len
        uint32_t header_len = 0;
        readBinaryLittleEndian(header_len, in);
        if (header_len < CORE_HEADER_LEN || header_len > MAX_HEADER_LEN || (header_len % 8) != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: invalid header_len {}", header_len);
        if (head_bytes.size() < header_len)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: header_len {} exceeds provided bytes {}", header_len, head_bytes.size());
        h.header_len = header_len;

        /// [12,16) index_len
        readBinaryLittleEndian(h.index_len, in);
        if (h.kind != ObjectKind::Pack && h.index_len != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: index_len must be 0 for kind {}", static_cast<int>(kind_byte));

        /// [16,24) logical_size
        readBinaryLittleEndian(h.logical_size, in);

        /// [24,40) [40,56) [56,72) [72,88)
        readBinaryLittleEndian(h.logical_hash, in);
        readBinaryLittleEndian(h.domain_id, in);
        readBinaryLittleEndian(h.incarnation_tag, in);
        readBinaryLittleEndian(h.build_id, in);

        /// [88,92) crc32c
        uint32_t stored_crc = 0;
        readBinaryLittleEndian(stored_crc, in);
        /// [92,96) reserved0
        uint32_t reserved0 = 0;
        readBinaryLittleEndian(reserved0, in);
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
        while (header_len - in.count() >= 4)
        {
            uint16_t type = 0;
            readBinaryLittleEndian(type, in);
            if (type == 0)
            {
                /// The 2 type bytes we just consumed are the first padding bytes (already zero); the
                /// rest of the area is verified zero by the trailing check below.
                break;
            }
            uint16_t len = 0;
            readBinaryLittleEndian(len, in);
            if (in.count() + len > header_len)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: TLV length overruns header");

            if (type == TLV_PROVENANCE)
            {
                /// Bounded sub-reader over the TLV body: a body shorter than the fixed fields is
                /// truncation (CORRUPTED_DATA via the decode guard); extra trailing body bytes
                /// (future fields) are skipped by the ignore below.
                ReadBufferFromMemory body(head_bytes.data() + in.count(), len);
                Provenance p;
                readBinaryLittleEndian(p.created_at_ms, body);
                readBinaryLittleEndian(p.creator_server_id, body);
                readBinaryLittleEndian(p.ch_version, body);
                uint8_t op = 0;
                readBinaryLittleEndian(op, body);
                p.op = static_cast<ProvenanceOp>(op);
                h.provenance = p;
                in.ignore(len);
            }
            else if (type == TLV_INTENDED_REF)
            {
                h.intended_ref = readFixedBytes(in, len);
            }
            else
            {
                /// Unknown TLV. Fail closed only if the critical flag is set; otherwise skip.
                if (has_critical)
                    saw_unknown_critical = true;
                in.ignore(len);
            }
        }

        /// Any bytes left (either after hitting padding, or the < 4 trailing bytes) must be zero padding.
        while (in.count() < header_len)
        {
            uint8_t pad = 0;
            readBinaryLittleEndian(pad, in);
            if (pad != 0)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: nonzero TLV padding");
        }

        if (saw_unknown_critical)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED,
                "CHCA envelope: unknown critical extension present (fail closed)");

        return h;
    });
}

}
