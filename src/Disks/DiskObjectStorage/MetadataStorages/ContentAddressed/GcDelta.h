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

    /// CA GC S3 — the resolved GENERATIONS the writer settled on after its tomb re-check / resurrection
    /// (spec §6, §7.1). `manifest_generation` is the `mg` of the manifest object this delta references
    /// (`parts/<part_id>/<mg>`); `pin_generations` is parallel to `pins` and carries each pinned blob's
    /// resolved `g` (`blobs/<H>/<g>`). The common case is all-zero (g=0/mg=0). The generation lives ONLY
    /// here and in the physical key — never in `part_id`/manifest identity (dedup is unchanged). When
    /// `pin_generations` is empty (an S2 delta, or a delta read from an older log object) every pin is
    /// taken as g=0. The manifest generation is also folded into `event_id` via computeEventId's
    /// `generation` argument, so two re-appends of the SAME resolved delta still dedup.
    uint64_t manifest_generation = 0;
    std::vector<uint64_t> pin_generations;

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
    ///
    /// Version 2 (CA GC S3) appends the resolved per-delta `manifest_generation` and the parallel
    /// `pin_generations` vector to each delta's body (spec §6). A v3 pool is created fresh (PoolMeta v3,
    /// no back-compat), so no v1 log object can exist in it — reading only v2 is correct and fail-closed.
    static constexpr FormatMagic MAGIC = makeMagic("CAGD");
    static constexpr uint8_t VERSION = 2;
};

/// CA GC S4 (#2): serialize/parse a single GcDelta for durable storage in a WriteSession (the sticky
/// fail-closed `pending_add_delta`). Mirrors the per-delta codec used inside GcLogBatch.
std::string serializeGcDeltaForSession(const GcDelta & delta);
GcDelta deserializeGcDeltaFromSession(const std::string & bytes);

}
