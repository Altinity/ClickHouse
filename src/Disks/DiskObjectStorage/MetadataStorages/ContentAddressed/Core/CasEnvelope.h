#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DB::Cas
{

/// The CHCA object envelope — protocol spec §3.1. Every blob and tree object on storage begins
/// with this fixed 94-byte little-endian core header, optionally followed by TLV extensions up to
/// header_len, then the payload. The header is the "incarnation zone": excluded from logical_hash,
/// it may differ between incarnations of the same logical object. The 4-byte magic encodes the
/// kind: `CABL` for blobs, `CATR` for trees; there is no separate kind byte.

enum class ObjectKind : uint8_t
{
    Blob = 1,
    Tree = 2,
};

enum class ProvenanceOp : uint8_t
{
    Other = 0,
    Insert = 1,
    Merge = 2,
    Mutation = 3,
    Attach = 4,
    Repack = 5,
};

/// PROVENANCE TLV (type 0x0001) — diagnostic only; no protocol decision ever reads it.
struct Provenance
{
    uint64_t created_at_ms = 0;
    UInt128 creator_server_id{};
    uint32_t ch_version = 0;
    ProvenanceOp op = ProvenanceOp::Other;
};

struct EnvelopeHeader
{
    ObjectKind kind = ObjectKind::Blob;
    uint8_t hash_algo = 1;             /// 1 = cityHash128
    uint16_t writer_version = 0;       /// set by decode; encode derives it from `kind` via CasFormat
    uint16_t min_reader_version = 0;   /// set by decode; encode derives it from `kind` via CasFormat
    /// Bytes covered by logical_hash = [header_len, EOF) = object_size - header_len, UNIFORMLY for
    /// all kinds (spec §3.1).
    uint64_t logical_size = 0;
    UInt128 logical_hash{};
    UInt128 domain_id{};
    UInt128 incarnation_tag{};
    UInt128 build_id{};
    std::optional<Provenance> provenance;        /// TLV 0x0001
    std::optional<String> intended_ref;          /// TLV 0x0002 (utf8)
    uint32_t header_len = 0;           /// filled by encode; reported by decode

    /// When set, encode pads the header with a single zero-type TLV so the returned bytes are EXACTLY
    /// this many bytes (and header_len == this). Used for fixed-length blob headers, so the payload of
    /// every blob in a pool starts at a constant offset (blob_header_len) and locate is a constant
    /// shift, no per-object header read. Must be >= the natural header length, <= 16 KiB, and 8-aligned
    /// (else BAD_ARGUMENTS). The pad is in [94, header_len) and is NOT covered by header_hash.
    std::optional<uint32_t> pad_to_header_len;

    /// Test-only knobs to drive the critical-extension fail-closed path. When
    /// unknown_critical_tlv is set, encode emits an unknown TLV type and sets the critical flag bit;
    /// decode of such a header must fail closed (UNKNOWN_FORMAT_VERSION).
    bool flags_has_critical_extension = false;
    bool unknown_critical_tlv = false;
};

/// Computes header_len + header_hash and returns the [0, header_len) header bytes (no payload).
String encodeEnvelopeHeader(EnvelopeHeader & header);

/// Validates and decodes the header. Every validation row of the spec table is one throw-path:
///   bad magic / wrong kind / bad header_len                                  -> CORRUPTED_DATA
///   future min_reader_version / unknown critical extension                   -> UNKNOWN_FORMAT_VERSION
///   size arithmetic mismatch / header_hash
///   mismatch (CityHash64 over the 94 core-header bytes, field zeroed)        -> CORRUPTED_DATA
EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind);

/// Payload starts right after the header.
inline uint64_t payloadOffset(const EnvelopeHeader & header)
{
    return static_cast<uint64_t>(header.header_len);
}

}
