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
/// Both formats follow the M-C1 codec conventions: little-endian, magic + version + reserved
/// header, strict decode (bad magic / truncation / trailing bytes / nonzero reserved / invalid
/// enum values => CORRUPTED_DATA; future version => NOT_IMPLEMENTED).

/// gc/state ("CAGS" v1): char[4]="CAGS" u8 version=1 u8[3] reserved=0 u64 round u64 fence_seq.
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

/// Retired set ("CART" v1), one object per gc/retired/<round>.<fence_seq>/<shard>:
/// char[4]="CART" u8 version=1 u8[3] reserved=0 u64 entry_count, then per entry:
/// u8 kind{1=blob,2=tree,3=pack} u128 hash u8 token_type{1=etag,2=generation,3=emulated}
/// u16 token_len, token_len bytes (token value), u64 size.
struct RetiredSet
{
    std::vector<RetiredEntry> entries;
};

String encodeGcState(const GcState & state);
GcState decodeGcState(std::string_view data);

String encodeRetiredSet(const RetiredSet & set);
RetiredSet decodeRetiredSet(std::string_view data);

}
