#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DB::Cas
{

/// The CHCA object envelope — protocol spec §3.1. Every blob, tree and pack object on storage begins
/// with this fixed 96-byte little-endian core header, optionally followed by TLV extensions up to
/// header_len, then (packs only) the index, then the payload. The header is the "incarnation zone":
/// excluded from logical_hash, it may differ between incarnations of the same logical object.

enum class ObjectKind : uint8_t
{
    Blob = 1,
    Tree = 2,
    Pack = 3,
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
    /// Bytes covered by logical_hash = [header_len, EOF) = object_size - header_len, UNIFORMLY for
    /// all kinds (spec §3.1). For packs this INCLUDES the index:
    /// logical_size = index_len + payload_region_size, and logical_hash covers index ‖ payload region.
    uint64_t logical_size = 0;
    UInt128 logical_hash{};
    UInt128 domain_id{};
    UInt128 incarnation_tag{};
    UInt128 build_id{};
    std::optional<Provenance> provenance;        /// TLV 0x0001
    std::optional<String> intended_ref;          /// TLV 0x0002 (utf8)
    uint32_t index_len = 0;            /// nonzero only for packs
    uint32_t header_len = 0;           /// filled by encode; reported by decode

    /// Test-only knobs to drive the critical-extension fail-closed path. When
    /// unknown_critical_tlv is set, encode emits an unknown TLV type and sets the critical flag bit;
    /// decode of such a header must fail closed (NOT_IMPLEMENTED).
    bool flags_has_critical_extension = false;
    bool unknown_critical_tlv = false;
};

/// Computes header_len + header_hash and returns the [0, header_len) header bytes (no payload).
String encodeEnvelopeHeader(EnvelopeHeader & header);

/// Validates and decodes the header. Every validation row of the spec table is one throw-path:
///   bad magic / wrong kind / bad header_len                                  -> CORRUPTED_DATA
///   future format_version / unknown critical extension                       -> NOT_IMPLEMENTED
///   index_len rule violation / size arithmetic mismatch / header_hash
///   mismatch (CityHash64 over the 96 core-header bytes, field zeroed)        -> CORRUPTED_DATA
EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind);

/// Payload starts after header and (packs only) the index. Note: for packs this points INTO the
/// hashed+sized region — the payload region is a sub-range of [header_len, EOF), which logical_size
/// and logical_hash cover in full (index included); payloadOffset is NOT the start of the hashed area.
inline uint64_t payloadOffset(const EnvelopeHeader & header)
{
    return static_cast<uint64_t>(header.header_len) + header.index_len;
}

}
