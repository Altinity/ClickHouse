#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSourceEdgeMarkers.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <IO/HashingReadBuffer.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <memory>
#include <string_view>

namespace DB::Cas
{

/// The v3 `cas_run` codec (codecs-v3 phase 5): the GC source-edge in-degree data plane as sorted
/// NDJSON, replacing the `CARN` block-framed binary run. This is the `RecordStream` family
/// (`FormatId::RunFile`): unbounded-cardinality sorted records, `object_cap = 0` (NEVER materialized
/// whole — streamed one line at a time over a `ReadBuffer`), `line_cap = 4 KiB`, `PinnedRaw` (no
/// compression) + `Strict` (byte-deterministic for `putDeterministicArtifact` adoption).
///
/// PHYSICAL LAYERING (spec §code-placement): this file is backend-free — it takes a `ReadBuffer &` /
/// `WriteBuffer &` and never includes `CasBackend.h`/`CasStore.h`/`CasBlobInDegree.h`. `Core/` owns the
/// stream and the packed-key/condemned-row bridge (`CasBlobInDegree`'s `openSourceEdgeRun` +
/// `SourceEdgeRunView`/`SourceEdgeRunBuilder`), because `SourceEdgeKeyCodec` and `encodeCondemnedRow`
/// live in `Core/`.
///
/// File shape:
///   {"type":"cas_run","v":3,"kind":"source_edge"}                      header line (type + v + kind gate)
///   {"b":"01<digest-hex>","s":"<32hex>","m":"edge"}                    an active-edge / zero-marker row
///   {"b":"01<digest-hex>","s":"00000000000000000000000000000000","m":"condemned","pend":false,"tt":"etag","tv":"...","sz":123,"cr":"7"}
///   {"n":184267}                                                       trailer: record count
///
/// The record key `b` is the algo BYTE as two lowercase hex chars followed by the digest hex at the
/// algo's width; `s` is the 32-hex source id. String-sorting records by (b, s) reproduces the current
/// binary (algo, digest, source_id) BYTE order (lowercase hex preserves unsigned byte order and the
/// algo byte is emitted first) — the invariant the fold's two-cursor merge depends on. The row-tag word
/// `m` maps to the `kEdgeActive`/`kZeroMarker`/`kCondemned` bytes; a `condemned` row additionally
/// carries the retired incarnation (`pend`/`tt`/`tv`/`sz`/`cr`).

/// One decoded source-edge row. All fields are identifier-layer types so the codec stays backend-free.
/// The condemned-only fields (`delete_pending`/`token`/`size`/`condemn_round`) are meaningful only when
/// `marker == kCondemned`.
struct SourceEdgeRecord
{
    BlobRef ref{};
    UInt128 source_id{};
    char marker = kEdgeActive;
    bool delete_pending = false;
    Token token{};
    uint64_t size = 0;
    uint64_t condemn_round = 0;
};

/// The header-line `kind` word for the only live `cas_run` kind.
inline constexpr std::string_view kSourceEdgeKindWord = "source_edge";

/// Write the typed header line `{"type":"cas_run","v":G_BUILD,"kind":"<kind>"}\n` (fixed key order for
/// byte-determinism). `writeHeaderLine` (phase 1) writes only type+v; `cas_run` carries the extra `kind`.
void writeRunHeaderLine(WriteBuffer & out, std::string_view kind);

/// Read + gate the typed header line: `type` must be `cas_run`, `v` is gated by `checkCompatibility`
/// (future `v` -> `UNKNOWN_FORMAT_VERSION`), and `kind` must equal `expected_kind` (else
/// `CORRUPTED_DATA`, "unknown run kind"). This is the typed-open — all three are validated before any
/// record is interpreted.
void expectRunHeaderLine(ReadBuffer & in, std::string_view expected_kind);

/// Sorted NDJSON writer over a caller-owned `WriteBuffer` (backend-free; writes plainly — the whole-
/// object checksum is `sourceEdgeRunChecksum` over the finished bytes, which keeps this writer free of a
/// HashingWriteBuffer finalize-ordering hazard). `append` asserts records arrive in non-decreasing
/// (ref, source_id) order and throws on a regression (this replaces the old `prev_key` monotonicity
/// check). `finish` writes the `{"n":count}` trailer.
class SourceEdgeRunWriter
{
public:
    explicit SourceEdgeRunWriter(WriteBuffer & out_);
    void append(const SourceEdgeRecord & rec);
    void finish();

private:
    WriteBuffer & out;
    uint64_t count = 0;
    bool have_prev = false;
    BlobRef prev_ref{};
    UInt128 prev_source_id{};
    bool finished = false;
};

/// The whole-object seal-checksum (`RunRef.checksum`) of a stored `cas_run`: the chained CityHash128 a
/// `HashingReadBuffer` computes over ALL the object bytes. The reader accumulates the IDENTICAL hash as
/// it streams (`SourceEdgeRunReader::verifyAgainst`), so a run PUT by the producer and later read by the
/// fold agree byte-for-byte. Computed over the finished bytes on the write side (the producer already
/// holds them to PUT); streamed on the read side (the run is never materialized whole to verify).
UInt128 sourceEdgeRunChecksum(std::string_view stored_bytes);

/// Sequential streaming reader over a caller-owned `ReadBuffer` (backend-free, O(one 4 KiB line)
/// resident). The ctor reads + gates the typed header line. `next` yields records in stored order and
/// returns false once the `{"n"}` trailer is consumed (the count is verified there — the line-truncation
/// guard). Every byte read is fed through a chained CityHash128; after the trailer, `verifyAgainst`
/// compares the accumulated whole-object hash to the seal's `RunRef.checksum` and throws `CORRUPTED_DATA`
/// on a mismatch — the caller calls it after draining and BEFORE acting on the records (the deletion
/// decision). Non-movable/non-copyable (owns a `HashingReadBuffer`) — construct in place.
class SourceEdgeRunReader
{
public:
    explicit SourceEdgeRunReader(ReadBuffer & in_);
    SourceEdgeRunReader(const SourceEdgeRunReader &) = delete;
    SourceEdgeRunReader & operator=(const SourceEdgeRunReader &) = delete;

    bool next(SourceEdgeRecord & rec);
    void verifyAgainst(const UInt128 & expected);
    /// The whole-object hash accumulated so far (meaningful once the trailer has been consumed).
    UInt128 accumulatedChecksum();

private:
    HashingReadBuffer hashing;
    uint64_t seen = 0;
    bool done = false;
};

}
