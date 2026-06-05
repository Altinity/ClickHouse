#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <cstdint>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// CA GC S2 — a single +/- GC delta (spec §5, §5.1, §9).
///
/// Commit appends a `+` delta and drop/unlink appends a `-` delta to the per-(epoch, shard) `gc/log`
/// prefix. A `+` records the resolved blob pins this part references PLUS the `(part_id) edge` (§9), so
/// the compaction counts MANIFEST references the same way it counts blob references — a manifest whose
/// running count reaches zero is a candidate by the same merge as a blob. The `-` carries the same
/// (part_id, pins) so the fold can net it against the matching `+`.
///
/// `event_id` is a STABLE u128 per `(part_id × op)` (with a generation discriminator reserved for S3,
/// default 0): two appends of the same logical delta — the §5.1 rule-2 re-append into a freshly-opened
/// epoch — carry the SAME `event_id`, so the compaction's dedup-on-fold collapses them to a single count
/// rather than over-counting. The id is the lowercase-hex SipHash-128 of `(op, generation, part_id)`, so
/// it is reproducible without any shared state.
struct GcDelta
{
    enum class Op : uint8_t
    {
        Add = 1, /// `+` : this part now references (pins) the blobs + manifest below
        Remove = 2, /// `-` : this part no longer references them
    };

    Op op = Op::Add;
    /// Stable id per (part_id, op, generation). See computeEventId.
    std::string event_id;
    /// The part whose commit/drop produced this delta. Counted as the `(part_id) edge` (§9).
    PartId part_id;
    /// The resolved bare blob hashes the part's manifest pins (the `(H)` keys the compaction counts).
    std::vector<BlobHash> pins;

    /// Compute the stable `event_id` for a logical delta. Deterministic from (part_id, op, generation):
    /// the same logical delta — including a §5.1 rule-2 re-append into a later epoch — yields the same
    /// id, so the fold dedups it. `generation` is reserved for S3 (manifest/blob generations); pass 0 in
    /// S2. The result is the lowercase-hex SipHash-128 digest (a stable 32-char id, never a raw u128 in
    /// host byte order — the log object is keyed by it and read cross-arch).
    static std::string computeEventId(const PartId & part_id, Op op, uint64_t generation = 0);

    auto operator<=>(const GcDelta &) const = default;
    bool operator==(const GcDelta &) const = default;
};

/// A coalesced batch of deltas (group-commit, §5 "batching is a requirement"). One `GcLogBatch` serializes
/// to ONE `gc/log` object holding multiple deltas, so a burst of N commits collapses into ⌈N/window⌉ log
/// objects rather than N. Serialized on the shared LE/varint codec with MAGIC+version, fail-closed on bad
/// magic / unknown version (B19/B28 discipline).
struct GcLogBatch
{
    std::vector<GcDelta> deltas;

    std::string serialize() const;
    static GcLogBatch deserialize(const std::string & bytes);

    /// 4-byte magic `CAGD` ("Content-Addressed Gc Delta") + a 1-byte version, per the shared codec.
    static constexpr FormatMagic MAGIC = makeMagic("CAGD");
    static constexpr uint8_t VERSION = 1;
};

}
