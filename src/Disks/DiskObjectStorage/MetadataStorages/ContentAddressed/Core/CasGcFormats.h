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
///    "round":7,"fence_seq":3,"snap_shards":1,"snap_generation":12,
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
    uint64_t fence_seq = 0;        /// leadership-epoch component of retired/outcome paths (spec §4)
    uint64_t snap_shards = 1;      /// GC constant (target-hash-prefix sharding); set once, immutable
    uint64_t snap_generation = 0;  /// monotone; the authoritative snap objects' generation
    GcLease lease;
    std::map<uint64_t, std::map<String, uint64_t>> fence_version;   /// round -> ("ns/shard" -> version)
};

/// One condemned object inside a retired set.
struct RetiredEntry
{
    ObjectKind kind = ObjectKind::Blob;
    UInt128 hash{};
    Token token;          /// the exact incarnation token GC observed (exact-token delete)
    uint64_t size = 0;
};

/// Retired set ("cas_retired_set" v1), one object per gc/retired/<round>.<fence_seq>/<shard>:
///   {"format":"cas_retired_set","version":1,
///    "entries":[{"kind":"blob","hash":"<32 lowercase hex>","token":"etag-1",
///                "token_type":"etag","size":1234}]}
/// kind: "blob" | "tree" | "pack"; token_type: "etag" | "generation" | "emulated".
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
