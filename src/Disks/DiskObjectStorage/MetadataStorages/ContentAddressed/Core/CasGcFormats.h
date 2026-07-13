#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// GC-surface formats — the part of the GC state the WRITER consumes (the publish gate reads
/// gc/state and the retired sets to decide whether a reused object is condemned, spec §4).
///
/// OWNERSHIP: these codecs are owned by GC (milestone M-C3). CAGS v3 carries the full GC
/// controller state: the lease, the snap config, and the fence versions. The fold cursor is NOT here —
/// it lives in the write-once fold seal (`CasFoldSeal::per_ns_shard`), see the `gc/state` note below.
/// Keep writer-side code forward-compatible by treating a future version as NOT_IMPLEMENTED
/// (fail closed), never as corruption. The format is unreleased, so v1 (the M-C2 minimal CAGS)
/// is NOT accepted — no compat shims.
///
/// Both formats are non-hashed metadata objects => STRICT JSON (spec §4 encoding split,
/// decision 2026-06-11): a top-level object with `format` + `version`, fail-closed decode
/// (wrong format / unknown key / missing key / wrong type / bad enum string / bad hash hex /
/// malformed document => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).

/// gc/state (proto `GcStateProto`, magic CAGT): the GC lease, snap config, and the ack-floor
/// `retired_refs` (gc-shard -> current retired-list object key). NOTE: the fold cursor (folded_cursor)
/// lives in the write-once fold seal (`CasFoldSeal::per_ns_shard`), not gc/state, so (edges, cursor)
/// are one write-once unit.
struct GcLease
{
    UInt128 owner{};          /// gc leader id (random u128); 0 = never held
    uint64_t seq = 0;         /// renewal counter; a steal observes a stalled (owner, seq)
};

struct GcState
{
    uint64_t round = 0;            /// the highest GC round whose retire sets are durable
    uint64_t gc_shards = 1;        /// GC blob-target-shard count (blob-hash-prefix sharding); set once, immutable
    uint64_t snap_generation = 0;  /// monotone; the authoritative snap objects' generation
    uint64_t snap_pruned_through = 0;   /// B174: highest snap generation fully pruned (retention cursor)
    uint64_t snap_attempt = 0;     /// adopted attempt id (folding leader's lease.seq) for snap_generation
    String manifest_sweep_cursor;  /// best-effort orphan part-manifest cleanup cursor; reachability ignores it
    GcLease lease;
    /// (retired-in-snapshot 2026-07-10) the former `retired_refs` map is gone: condemned state now rides
    /// the source-edge runs as `kCondemned` rows and the fold seal's `condemned_summary`. Proto field
    /// number reserved — never reuse.
};

/// One condemned object. `condemn_round` is the round that condemned the incarnation, used by GC's
/// round-paced graduation gate. IN-MEMORY ONLY (retired-in-snapshot 2026-07-10): the durable `RetiredSet`
/// object family is gone; this struct now lives solely as the element type of `RetiredMergeResult`
/// (`CasBlobInDegree.h`), populated from the `kCondemned` rows decoded out of the source-edge runs.
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    BlobRef ref{};
    Token token;          /// the exact incarnation token GC observed (exact-token delete)
    uint64_t size = 0;
    uint64_t condemn_round = 0;   /// the GC round that condemned this incarnation (round-paced
                                  /// graduation: an entry graduates only once condemn_round < the
                                  /// current round). Consulted by GC only; the writer never reads it.
    bool delete_pending = false;  /// two-phase graduation (spec Task-9 amendment): floor-passed and
                                  /// published for deletion; the NEXT pass executes the exact-token
                                  /// delete (pre-CAS, safe at any leader staleness) and drops the entry.
                                  /// Terminal: a pending entry is never un-pended (writers keep seeing
                                  /// it condemned and recreate).
};

String encodeGcState(const GcState & state);
GcState decodeGcState(std::string_view data);

/// Advisory GC liveness pulse (B160). A leader bumps `hb_seq` on a fast cadence independent of round
/// progress; a follower's lease steal backs off if it sees this advance, so a slow-but-alive leader
/// (its lease.seq frozen for the round) is never falsely stolen from. Fixed 24-byte binary:
/// 16-byte big-endian owner + 8-byte big-endian hb_seq.
struct GcHeartbeat
{
    UInt128 owner{};
    uint64_t hb_seq = 0;
};
String encodeGcHeartbeat(const GcHeartbeat & hb);
GcHeartbeat decodeGcHeartbeat(std::string_view data);

}
