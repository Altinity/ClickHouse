#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DB::Cas
{

/// The CHCA object envelope — protocol spec §3.1. Every blob object on storage begins with this
/// fixed 70-byte little-endian core header, optionally followed by TLV extensions up to header_len,
/// then the payload. The header is the "incarnation zone": it may differ between incarnations of the
/// same logical object. The 4-byte magic encodes the kind: `CABL` for blobs (the standalone tree
/// object kind was excised in the rev. 15 `PartManifest` redesign; see `ObjectKind` below).
///
/// The binary core layout (all little-endian):
///   magic[4] + writer_version[2] + compatibility_version[2] + hash_algo[1] + flags[1] + header_len[4]
///   + three u128s[48] + header_hash[8] = 70 bytes.
/// The former `logical_size` / `logical_hash` core fields were dropped (S3-native staging fix
/// 2026-07-11): they were the only two fields not known until AFTER the payload was streamed, which
/// blocked building the header BEFORE streaming the S3 staging blob. Nothing load-bearing read them —
/// the read path uses the fixed payload offset `blob_header_len` and GC derives the payload size as
/// `object_size - blob_header_len`.
/// compatibility_version (formerly named min_reader_version) encodes the write-down-to-floor for
/// the payload format: a reader must fail-closed if compatibility_version > G_BUILD.
/// (Aligned with the converged header model 2026-06-25.)

/// The standalone `Tree` object kind (Merkle layer) was rejected and excised 2026-07-03 (rev. 15
/// `PartManifest` redesign superseded it). `Blob` is the only surviving kind; the enum is kept as a
/// switch-friendly type at existing call sites rather than force-collapsed into a bare constant
/// (see `docs/superpowers/cas/ROADMAP.md` for a follow-up note on whether it still earns its keep).
enum class ObjectKind : uint8_t
{
    Blob = 1,
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
    uint8_t hash_algo = 1;                  /// 1 = cityHash128
    uint16_t writer_version = 0;            /// set by decode; encode derives it from `kind` via CasFormat
    uint16_t compatibility_version = 0;     /// set by decode; encode derives it from `kind` via CasFormat
                                            /// (write-down-to-floor: blobs carry raw payload and do not
                                            ///  branch on this — always G_BUILD for now.)
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
///   future compatibility_version / unknown critical extension                -> UNKNOWN_FORMAT_VERSION
///   header_hash mismatch (CityHash64 over the 70 core-header bytes, field zeroed) -> CORRUPTED_DATA
/// `object_size` is accepted for call-site symmetry with the read/GC paths but is no longer validated
/// against a core `logical_size` (that field was dropped 2026-07-11); the payload length is derived
/// downstream as `object_size - header_len`.
EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind);

/// Payload starts right after the header.
inline uint64_t payloadOffset(const EnvelopeHeader & header)
{
    return static_cast<uint64_t>(header.header_len);
}

}
