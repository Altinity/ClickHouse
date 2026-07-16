#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// gc/state control object (spec §GC State): the GC lease, snap config, and cursors. The fold cursor
/// lives in the write-once fold seal, not here. v3 text: header line + one JSON body object.
struct GcLease
{
    UInt128 owner{};          /// gc leader id (random u128); 0 = never held
    uint64_t seq = 0;         /// renewal counter; a steal observes a stalled (owner, seq)
};

struct GcState
{
    uint64_t round = 0;            /// the highest GC round whose retire sets are durable
    uint64_t gc_shards = 1;        /// GC blob-target-shard count; set once, immutable; must be >= 1
    uint64_t snap_generation = 0;  /// monotone; the authoritative snap objects' generation
    uint64_t snap_pruned_through = 0;   /// highest snap generation fully pruned (retention cursor)
    uint64_t snap_attempt = 0;     /// adopted attempt id (folding leader's lease.seq) for snap_generation
    String manifest_sweep_cursor;  /// best-effort orphan part-manifest cleanup cursor; reachability ignores it
    GcLease lease;
};

String encodeGcState(const GcState & state);
GcState decodeGcState(std::string_view data);

/// Advisory GC liveness pulse (B160). A leader bumps `hb_seq` on a fast cadence independent of round
/// progress; a follower's lease steal backs off if it sees this advance. v3 text: header line +
/// {"by":"<owner hex>","seq":"<hb_seq>"} — the former 24-byte unversioned binary is gone (the last
/// unversioned object; spec §control-plane).
struct GcHeartbeat
{
    UInt128 owner{};
    uint64_t hb_seq = 0;
};

String encodeGcHeartbeat(const GcHeartbeat & hb);
GcHeartbeat decodeGcHeartbeat(std::string_view data);

}
