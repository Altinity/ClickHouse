#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DB::Cas
{

/// The standalone `Tree` object kind (Merkle layer) was rejected and excised 2026-07-03 (rev. 15
/// `PartManifest` redesign superseded it). `Blob` is the only surviving kind; the enum is kept as a
/// switch-friendly type at existing call sites rather than force-collapsed into a bare constant.
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

/// PROVENANCE (was TLV type 0x0001) — diagnostic only; no protocol decision ever reads it.
struct Provenance
{
    uint64_t created_at_ms = 0;
    UInt128 creator_server_id{};
    uint32_t ch_version = 0;
    ProvenanceOp op = ProvenanceOp::Other;
};

/// The CHCA blob envelope header — protocol spec §3.1, v3 text form (2026-07-15 codecs-v3). A single
/// JSON object at bytes [0, json_len), ASCII-space pad at [json_len, blob_header_len-1), and '\n' at
/// byte blob_header_len-1. The payload begins at the constant offset `blob_header_len` (256 for blob
/// pools, a `PoolMeta` parameter), so locate is a constant shift with no per-object header read. The
/// header is the "incarnation zone": it may differ between incarnations of the same logical object —
/// the `tag` is a fresh random u128 per upload attempt (W-FRESH-TAG), the exact-token delete primitive.
///
/// Dropped from the pre-v3 70-byte binary core: `hash_algo` (identity lives in the key + manifest ref),
/// `domain_id` (never validated), `header_hash` (no consumer — CityHash64 left the envelope), and
/// `writer_version` (forensics are `ch` + `bld`).
struct EnvelopeHeader
{
    ObjectKind kind = ObjectKind::Blob;
    /// Set by decode from the header `v`; encode stamps `currentCompatibilityVersion()`. A reader
    /// fails closed (UNKNOWN_FORMAT_VERSION) when `v` exceeds what this build understands.
    uint32_t compatibility_version = 0;
    UInt128 incarnation_tag{};              /// `tag`
    UInt128 build_id{};                     /// `bld`
    std::optional<Provenance> provenance;   /// `ts` / `by` / `op` / `ch`
    std::optional<String> intended_ref;     /// `ref` (diagnostic; truncated on encode to fit the header)
    uint32_t header_len = 0;                /// filled by encode/decode = blob_header_len (payload offset)
    /// Test-only knob: emit an unknown `!`-critical key, so decode of the result must fail closed
    /// (UNKNOWN_FORMAT_VERSION). Drives the critical-extension path the TLV critical flag used to test.
    bool emit_unknown_critical_key = false;
};

/// Builds the fixed-length header for a pool whose blob_header_len is `blob_header_len` (256 for blob
/// pools). Sets `header.header_len = blob_header_len` and returns exactly `blob_header_len` bytes. The
/// `ref` is truncated (never dropped) so the header always fits. Built with NO payload dependency
/// (S3-native staging compatible — the header is known before the payload streams).
String encodeEnvelopeHeader(EnvelopeHeader & header, uint32_t blob_header_len);

/// Parses + validates the header. Derives `header_len` from the '\n' terminator's position and verifies
/// the pad zone is all ASCII spaces (no smuggling). bad `type` / pad violation / truncation ->
/// CORRUPTED_DATA; future `v` / unknown `!`-key -> UNKNOWN_FORMAT_VERSION. `object_size` is accepted for
/// call-site symmetry with the read/GC paths (the payload length is derived downstream as
/// `object_size - header_len`); it is not otherwise consulted.
EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind);

/// Payload starts right after the header.
inline uint64_t payloadOffset(const EnvelopeHeader & header)
{
    return static_cast<uint64_t>(header.header_len);
}

}
