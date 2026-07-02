#pragma once
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
/// controller state: the lease, the snap config, and the fence versions. The fold cursor was
/// moved into the snap (GcSnap::folded_cursor) in B140-dangle fix (v3).
/// Keep writer-side code forward-compatible by treating a future version as NOT_IMPLEMENTED
/// (fail closed), never as corruption. The format is unreleased, so v1 (the M-C2 minimal CAGS)
/// is NOT accepted — no compat shims.
///
/// Both formats are non-hashed metadata objects => STRICT JSON (spec §4 encoding split,
/// decision 2026-06-11): a top-level object with `format` + `version`, fail-closed decode
/// (wrong format / unknown key / missing key / wrong type / bad enum string / bad hash hex /
/// malformed document => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).

/// gc/state ("cas_gc_state" v3):
///   {"format":"cas_gc_state","version":3,
///    "round":7,"fence_seq":3,"gc_shards":1,"snap_generation":12,
///    "lease":{"owner":"<32hex>","seq":5},
///    "fence_version":{"<round>":{"<ns>/<root_shard>":4}}}
/// fence_version is indexed by ROOT shard ("ns/shard" strings — the journal sources); the snap /
/// retired / outcome OBJECTS are indexed by target-hash-prefix snap shard. Two distinct sharding
/// axes (spec §4). fence_version outer keys are the round as a decimal string (JSON object keys
/// are strings). NOTE: the fold cursor (folded_cursor) moved from gc/state into the snap
/// (GcSnap::folded_cursor, B140-dangle fix) so (edges, cursor) are one write-once unit.
struct GcLease
{
    UInt128 owner{};          /// gc leader id (random u128); 0 = never held
    uint64_t seq = 0;         /// renewal counter; a steal observes a stalled (owner, seq)
};

struct GcState
{
    uint64_t round = 0;            /// the highest GC round whose retire sets are durable
    uint64_t fence_seq = 0;        /// leadership-epoch counter, bumped on every lease steal (recorded in GC
                                   /// events). NOT a key component anymore: retired/outcome objects are now
                                   /// keyed under the adopted (snap_generation, snap_attempt), not fence_seq.
    uint64_t gc_shards = 1;        /// GC blob-target-shard count (blob-hash-prefix sharding); set once, immutable
    uint64_t snap_generation = 0;  /// monotone; the authoritative snap objects' generation
    uint64_t snap_pruned_through = 0;   /// B174: highest snap generation fully pruned (retention cursor)
    uint64_t snap_attempt = 0;     /// adopted attempt id (folding leader's lease.seq) for snap_generation
    String manifest_sweep_cursor;  /// best-effort orphan part-manifest cleanup cursor; reachability ignores it
    GcLease lease;
    std::map<uint64_t, std::map<String, uint64_t>> fence_version;   /// round -> ("ns/shard" -> version)
    std::map<uint64_t, String> retired_refs;   /// gc-shard -> object key of the current retired list
                                               /// (ack-floor redesign). The writer publish gate loads
                                               /// the current retired list by dereferencing these keys;
                                               /// RetireView reads them straight out of this same body,
                                               /// so a retired set can never be older than the round it
                                               /// is installed for.
};

/// One condemned object inside a retired set. `condemn_round` is the ack-floor addition (spec
/// 2026-07-02): the round that condemned the incarnation, used by GC's graduation gate.
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    UInt128 hash{};
    Token token;          /// the exact incarnation token GC observed (exact-token delete)
    uint64_t size = 0;
    uint64_t condemn_round = 0;   /// the GC round that condemned this incarnation (ack-floor graduation:
                                  /// an entry graduates only when condemn_round < min_ack). Consulted by
                                  /// GC only; the writer publish gate ignores it.
    bool delete_pending = false;  /// two-phase graduation (spec Task-9 amendment): floor-passed and
                                  /// published for deletion; the NEXT pass executes the exact-token
                                  /// delete (pre-CAS, safe at any leader staleness) and drops the entry.
                                  /// Terminal: a pending entry is never un-pended (writers keep seeing
                                  /// it condemned and recreate).
};

/// Retired set (proto `RetiredSetProto`, magic CART, one object per gc-shard referenced from
/// `GcState::retired_refs`). Each entry carries (kind, hash, token, size, condemn_round). Entries are
/// sorted by (kind, hash) before serialization for byte-deterministic bytes (ack-floor spec).
struct RetiredSet
{
    std::vector<RetiredEntry> entries;
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

String encodeRetiredSet(const RetiredSet & set);
RetiredSet decodeRetiredSet(std::string_view data);

}
