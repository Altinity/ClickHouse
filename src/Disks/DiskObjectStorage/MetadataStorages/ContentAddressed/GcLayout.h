#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <cstdint>
#include <optional>
#include <string>

namespace DB::ContentAddressed
{

// ==== CA GC S2: log-structured, streaming GC layout (spec §4, §5) ====
//
// The reverse index (blob -> referrers) is log-structured like an LSM tree: a sorted SNAPSHOT run plus
// a tail of small +/- DELTA objects appended on commit/drop, compacted per GC epoch. All three families
// — the snapshot, the log, and (later) the leadership lease — are keyed by HASH-PREFIX SHARD from day
// one, so a shard is the unit of leadership/epoch/compaction. The first implementation runs ONE worker
// across all shards; running N workers (one per shard) is then a configuration flip, NOT a layout change.
//
// `ShardId` is the small integer derived from a content hash's prefix (shardForHash). `kGcShardCount` is
// the SINGLE source of truth for the shard count — change it in one place and the whole layout follows.

using ShardId = uint32_t;

// The number of GC shards. A power of two so shardForHash can mask the top hash-prefix bits cheaply and
// the partition is uniform. This is the one knob that turns "1 worker over N shards" into "N workers".
inline constexpr ShardId kGcShardCount = 16;

// Derive the GC shard for a content hash from its hash prefix (the high bits of H0). The hash is the
// lowercase-hex content digest; the first hex nibbles are uniformly distributed, so mapping them onto
// [0, kGcShardCount) gives a uniform, stable partition. A hash too short to carry a prefix (only the
// short synthetic hashes used in unit tests) maps to shard 0 deterministically.
ShardId shardForHash(const BlobHash & blob_hash);

// Derive the GC shard for a part id. A `PartId` is a lowercase-hex digest (the content hash of the
// manifest body), so its prefix nibbles are uniformly distributed on [0, kGcShardCount) by the same
// argument as `shardForHash`. This is the single canonical shard assignment for a part's sealed-index
// entry; `GcLogWriter::shardForPartId` delegates here (rather than duplicating the nibble-fold logic)
// so the two are always in sync.
ShardId shardForPartId(const PartId & part_id);

// Per-shard epoch counter object key: <key_prefix>/gc/current_epoch/<shard>. The epoch a writer
// currently appends deltas under (per-shard, not global — §5.1 rule 1). A writer reads it (default 0 if
// absent) to stamp its delta; the fenced shard leader closes the epoch by a plain fenced PUT of E+1
// before folding. Single-writer-per-shard (the GC leader), so no CAS is needed.
std::string gcCurrentEpochKey(const std::string & key_prefix, ShardId shard);

// Per-(epoch, shard) delta-log PREFIX: <key_prefix>/gc/log/<epoch>.<shard>/. List this to enumerate the
// epoch's coalesced delta objects for the fold. The epoch is NOT zero-padded here: the log prefix is
// LISTed by exact (epoch, shard), never scanned in lexical order, so numeric formatting is sufficient.
std::string gcLogPrefix(const std::string & key_prefix, uint64_t epoch, ShardId shard);

// A single coalesced delta-log object key: <key_prefix>/gc/log/<epoch>.<shard>/<event_id>. One object
// holds one group-commit batch of deltas; `event_id` is the batch's stable id (so a re-append into a
// later epoch lands an idempotent, dedup-able object — §5.1). Typed (GcLogObjectKey) so a log key can
// never be confused with a blob/part/ref key (B29).
GcLogObjectKey gcLogEventKey(const std::string & key_prefix, uint64_t epoch, ShardId shard, const std::string & event_id);

// Per-(epoch, shard) snapshot object key: <key_prefix>/gc/snap/<padded-epoch>.<shard>. The sorted
// (H)->count run for the shard at this epoch. The epoch is ZERO-PADDED to a fixed width so a LEXICAL
// LIST of the gc/snap prefix yields the snapshots in NUMERIC epoch order (the compaction picks the
// latest by listing — without padding, "10" would sort before "2"). Typed (GcSnapObjectKey) per B29.
GcSnapObjectKey gcSnapKey(const std::string & key_prefix, uint64_t epoch, ShardId shard);

// The LIST prefix for ALL snapshot runs of EVERY epoch and shard: <key_prefix>/gc/snap/. A lexical LIST
// returns the objects in (padded-epoch, shard) order — the compaction's rebuild lists this and filters to
// one shard's `.<shard>` suffix, taking the highest epoch as the latest snapshot for that shard.
std::string gcSnapPrefix(const std::string & key_prefix);

// The LIST prefix for ALL delta-log objects of EVERY epoch and shard: <key_prefix>/gc/log/. The rebuild
// lists this to discover which epochs still carry un-folded deltas for a shard (filtering the
// <epoch>.<shard>/ component). The per-epoch fold uses the tighter gcLogPrefix instead.
std::string gcLogRootPrefix(const std::string & key_prefix);

// ==== CA GC S4 (#4): per-shard sealed-tombstone index ====
//
// A compact set of "open" (sealed-but-unswept) tombstones the sweep re-presents each round, replacing
// the full blobs/+parts/ bucket LIST (Scan A). One tiny object per open tombstone:
//   <prefix>/gc/sealed/<shard>/<identity>.<generation>.<type>
// where <type> is `b` for a blob tombstone and `p` for a part tombstone.
//
// Encoding rationale:
//   - `identity` is a lowercase hex digest (blob hash or part id) — it contains ONLY [0-9a-f], so it
//     never contains a `.`. Splitting the basename on `.` therefore yields exactly 3 fields:
//     `identity`, `generation` (decimal uint64), and `type` (`b` or `p`). The basename is unambiguously
//     parseable without any quoting or escaping.
//   - The shard is NOT encoded in the basename; it is implicit in the LIST prefix the sweep uses when it
//     walks `gcSealedPrefix(p, shard)`. The parser does NOT validate the shard segment — a stray object
//     under gc/sealed/ is ignored (parseSealedIndexKey returns nullopt) rather than misparsed.
//
// Lifecycle (maintained by Task 15):
//   - SEAL path: PUT gcSealedKey(...) atomically after the `.tombstone` object is created (CREATE-then-PUT).
//   - SWEEP/RECOVER path: DELETE gcSealedKey(...) once the tombstone is permanently resolved (sweep or recover).
//
// The index object carries NO payload (an empty body is fine — the key encodes all necessary state).

// LIST prefix for a single shard: <key_prefix>/gc/sealed/<shard>/
std::string gcSealedPrefix(const std::string & key_prefix, ShardId shard);

// Full object key for a single sealed-index entry:
// <key_prefix>/gc/sealed/<shard>/<identity>.<generation>.<b|p>
std::string gcSealedKey(const std::string & key_prefix, ShardId shard, const std::string & identity, uint64_t generation, bool is_blob);

// Inverse of gcSealedKey: parse a sealed-index entry back to (identity, generation, is_blob).
// Returns nullopt on a malformed key (wrong path shape, wrong segment count, non-numeric generation,
// bad type char) so a stray object under gc/sealed/ is silently ignored rather than misparsed.
// Round-trip guarantee: parseSealedIndexKey(p, gcSealedKey(p, s, id, g, t)) == SealedIndexEntry{id, g, t}.
struct SealedIndexEntry
{
    std::string identity;
    uint64_t generation = 0;
    bool is_blob = true;
};
std::optional<SealedIndexEntry> parseSealedIndexKey(const std::string & key_prefix, const std::string & key);

}
