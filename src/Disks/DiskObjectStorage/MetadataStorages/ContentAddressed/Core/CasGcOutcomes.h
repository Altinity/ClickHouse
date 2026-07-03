#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Retire outcomes (spec §7 R4) — what the recheck decided per retired entry. One JSON object per
/// gc/outcomes/<round>.<fence_seq>/<snap_shard>; written once (`putIfAbsent` — idempotent replay reads
/// and verifies). Non-hashed metadata => strict JSON. 412-saves (replaced) are a health metric.
enum class OutcomeKind : uint8_t
{
    Deleted = 1,    /// exact-token delete landed
    Absent = 2,     /// already gone (a prior crashed round's delete landed)
    Replaced = 3,   /// 412 - a resurrection won; the new incarnation lives on
    Spared = 4,     /// fold-through-fence found in-degree > 0
};

struct OutcomeEntry
{
    ObjectKind kind = ObjectKind::Blob;
    UInt128 hash{};
    Token token;                 /// the retired (condemned) token the recheck acted on
    OutcomeKind outcome = OutcomeKind::Spared;
};

/// Outcome log ("cas_gc_outcomes" v1), one object per gc/outcomes/<round>.<fence_seq>/<snap_shard>:
///   {"format":"cas_gc_outcomes","version":1,
///    "entries":[{"kind":"blob","hash":"<32 lowercase hex>","token":"etag-1",
///                "token_type":"etag","outcome":"deleted"}]}
/// outcome: "deleted" | "absent" | "replaced" | "spared"; kind/token_type as in the retired set.
struct OutcomeLog
{
    std::vector<OutcomeEntry> entries;
};

String encodeOutcomeLog(const OutcomeLog & log);
OutcomeLog decodeOutcomeLog(std::string_view data);

}
