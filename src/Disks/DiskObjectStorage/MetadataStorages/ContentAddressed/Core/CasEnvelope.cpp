#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/unaligned.h>
#include <city.h>
#include <array>
#include <cstring>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{

/// 94 is the EXACT packed size of the v1 core fields (hole-free; the kind byte and the old index_len
/// pad word are gone — kind is encoded by the magic):
///   magic[4] + writer_version[2] + min_reader_version[2] + hash_algo[1] + flags[1] + header_len[4]
///   + logical_size[8] + four u128s[64] + header_hash[8] = 94.
/// The core is never grown: new fields go into the [94, header_len) TLV extensions. header_len is
/// padded up to an 8-byte multiple; a fixed per-pool header length is set via pad_to_header_len.
constexpr uint32_t CORE_HEADER_LEN = 94;
constexpr uint32_t MAX_HEADER_LEN = 16384;
constexpr size_t HEADER_HASH_OFFSET = 86;
constexpr size_t HEADER_HASH_LEN = 8;

constexpr std::string_view MAGIC_BLOB = "CABL";
constexpr std::string_view MAGIC_TREE = "CATR";

std::string_view magicFor(ObjectKind kind)
{
    return kind == ObjectKind::Blob ? MAGIC_BLOB : MAGIC_TREE;
}

FormatId formatIdFor(ObjectKind kind)
{
    return kind == ObjectKind::Blob ? FormatId::Blob : FormatId::Tree;
}

constexpr uint8_t FLAG_HAS_CRITICAL_EXTENSION = 0x01;

constexpr uint16_t TLV_PROVENANCE = 0x0001;
constexpr uint16_t TLV_INTENDED_REF = 0x0002;
/// An arbitrary unknown type used (test-only) to exercise the critical fail-closed path.
constexpr uint16_t TLV_UNKNOWN_CRITICAL = 0x7f01;

/// header_hash = CityHash64 over the 96 core-header bytes with the [88, 96) field itself zeroed —
/// the hash family the rest of ClickHouse uses for checksums. A diagnostics-quality check, not a
/// safety dependency: every load-bearing header field is independently re-verified (spec §3.1).
uint64_t computeHeaderHash(const char * core_header_with_hash_zeroed)
{
    return CityHash_v1_0_2::CityHash64(core_header_with_hash_zeroed, CORE_HEADER_LEN);
}

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
                writeU128LE(body_buf, header.provenance->creator_server_id);
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

    /// Optional fixed-length headers: pad with a single zero-type TLV up to the requested length, so the
    /// payload starts at a constant offset for every object in the pool (constant-shift locate). The pad
    /// is appended AFTER the 8-alignment above, and pad_to_header_len is itself 8-aligned, so the gap is
    /// a non-negative multiple of 8 — either 0 or at least 8 (always room for the 4-byte TLV header).
    if (header.pad_to_header_len)
    {
        const uint32_t target = *header.pad_to_header_len;
        if (target < header_len)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CHCA envelope: pad_to_header_len {} is below the natural header length {}", target, header_len);
        if (target > MAX_HEADER_LEN)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CHCA envelope: pad_to_header_len {} exceeds the maximum header length {}", target, MAX_HEADER_LEN);
        if ((target % 8) != 0)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CHCA envelope: pad_to_header_len {} must be 8-aligned", target);

        const uint32_t gap = target - header_len;
        /// The decoder treats a zero type word as the start of all-zero alignment padding (a valid TLV
        /// type is never 0), so the pad is simply `gap` zero bytes — NOT a length-prefixed TLV. With both
        /// sides 8-aligned the gap is a multiple of 8, so it is 0 or >= 8: the 2-byte zero type marker
        /// always fits. (A 1-3 byte gap can't carry the marker; 8-alignment rules it out, but we surface
        /// it rather than silently mis-size.)
        if (gap == 1 || gap == 2 || gap == 3)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CHCA envelope: cannot pad to header_len {} from {}", target, header_len);
        ext.append(gap, '\0');
        header_len = target;
    }

    header.header_len = header_len;

    String out;
    {
        WriteBufferFromString out_buf(out);

        const WriterStamp stamp = currentWriterVersion(formatIdFor(header.kind));
        writeString(magicFor(header.kind), out_buf);                        /// [0,4)  magic (encodes kind)
        writeBinaryLittleEndian(stamp.writer_version, out_buf);             /// [4,6)  writer_version
        writeBinaryLittleEndian(stamp.min_reader_version, out_buf);         /// [6,8)  min_reader_version
        writeBinaryLittleEndian(header.hash_algo, out_buf);                 /// [8]    hash_algo
        writeBinaryLittleEndian(
            static_cast<uint8_t>(critical ? FLAG_HAS_CRITICAL_EXTENSION : 0), out_buf); /// [9] flags
        writeBinaryLittleEndian(header_len, out_buf);                       /// [10,14) header_len
        writeBinaryLittleEndian(header.logical_size, out_buf);              /// [14,22) logical_size
        writeU128LE(out_buf, header.logical_hash);                          /// [22,38)
        writeU128LE(out_buf, header.domain_id);                             /// [38,54)
        writeU128LE(out_buf, header.incarnation_tag);                       /// [54,70)
        writeU128LE(out_buf, header.build_id);                              /// [70,86)
        writeBinaryLittleEndian(static_cast<uint64_t>(0), out_buf);         /// [86,94) header_hash (zeroed)

        writeString(ext, out_buf);                                          /// [94, header_len) TLV extensions
        out_buf.finalize();
    }

    /// Compute the hash over [0,96) with the header_hash field zeroed (it already is), then patch it in.
    const uint64_t header_hash = computeHeaderHash(out.data());
    unalignedStoreLittleEndian<uint64_t>(out.data() + HEADER_HASH_OFFSET, header_hash);

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

        EnvelopeHeader h;

        /// [0,4) magic encodes the kind.
        const String magic = readFixedBytes(in, 4);
        if (magic == MAGIC_BLOB)
            h.kind = ObjectKind::Blob;
        else if (magic == MAGIC_TREE)
            h.kind = ObjectKind::Tree;
        else
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: bad magic");
        if (h.kind != expected_kind)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: kind {} does not match expected {}",
                static_cast<int>(h.kind), static_cast<int>(expected_kind));

        /// [4,6) writer_version, [6,8) min_reader_version — fail closed on a future object.
        readBinaryLittleEndian(h.writer_version, in);
        readBinaryLittleEndian(h.min_reader_version, in);
        gateOnRead(h.min_reader_version, "CHCA envelope");

        /// [8] hash_algo
        readBinaryLittleEndian(h.hash_algo, in);

        /// [9] flags
        uint8_t flags = 0;
        readBinaryLittleEndian(flags, in);
        const bool has_critical = (flags & FLAG_HAS_CRITICAL_EXTENSION) != 0;
        h.flags_has_critical_extension = has_critical;

        /// [10,14) header_len
        uint32_t header_len = 0;
        readBinaryLittleEndian(header_len, in);
        if (header_len < CORE_HEADER_LEN || header_len > MAX_HEADER_LEN || (header_len % 8) != 0)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: invalid header_len {}", header_len);
        if (head_bytes.size() < header_len)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: header_len {} exceeds provided bytes {}", header_len, head_bytes.size());
        h.header_len = header_len;

        /// [14,22) logical_size
        readBinaryLittleEndian(h.logical_size, in);

        /// [22,38) [38,54) [54,70) [70,86)
        h.logical_hash = readU128LE(in);
        h.domain_id = readU128LE(in);
        h.incarnation_tag = readU128LE(in);
        h.build_id = readU128LE(in);

        /// [86,94) header_hash
        uint64_t stored_header_hash = 0;
        readBinaryLittleEndian(stored_header_hash, in);

        /// Verify the hash over [0,96) with the header_hash field zeroed.
        {
            std::array<char, CORE_HEADER_LEN> core{};
            std::memcpy(core.data(), head_bytes.data(), CORE_HEADER_LEN);
            std::memset(core.data() + HEADER_HASH_OFFSET, 0, HEADER_HASH_LEN);
            const uint64_t computed = computeHeaderHash(core.data());
            if (computed != stored_header_hash)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                    "CHCA envelope: header hash mismatch (computed {:016x}, stored {:016x})",
                    computed, stored_header_hash);
        }

        /// Size arithmetic (uniform for ALL kinds, spec §3.1): header_len + logical_size == object_size.
        /// logical_size covers [header_len, EOF).
        const uint64_t expected_object_size = static_cast<uint64_t>(header_len) + h.logical_size;
        if (expected_object_size != object_size)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: size mismatch (header_len {} + logical_size {} = {}, object_size {})",
                header_len, h.logical_size, expected_object_size, object_size);

        /// [94, header_len) TLV extensions. Encode pads the area to 8-alignment with zero bytes; a valid
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
                p.creator_server_id = readU128LE(body);
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
            throw DB::Exception(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
                "CHCA envelope: unknown critical extension present (fail closed)");

        return h;
    });
}

}
