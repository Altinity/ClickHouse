#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// GC-surface formats — the part of the GC state the WRITER consumes (the publish gate reads
/// gc/state and the retired sets to decide whether a reused object is condemned, spec §4).
///
/// OWNERSHIP: these codecs are owned by GC (milestone M-C3), which will extend them — CAGS v2
/// adds cursors, the lease, and fence versions. M-C2 defines only the minimal v1 the writer
/// needs; keep writer-side code forward-compatible by treating a future version as
/// NOT_IMPLEMENTED (fail closed), never as corruption.
///
/// Both formats are non-hashed metadata objects => STRICT JSON (spec §4 encoding split,
/// decision 2026-06-11): a top-level object with `format` + `version`, fail-closed decode
/// (wrong format / unknown key / missing key / wrong type / bad enum string / bad hash hex /
/// malformed document => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).

/// gc/state ("cas_gc_state" v1):
///   {"format":"cas_gc_state","version":1,"round":7,"fence_seq":3}
struct GcState
{
    uint64_t round = 0;       /// the highest GC round whose retire sets are durable
    uint64_t fence_seq = 0;   /// leadership-epoch component of retired-set paths (spec §4)
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

String encodeRetiredSet(const RetiredSet & set);
RetiredSet decodeRetiredSet(std::string_view data);

}
