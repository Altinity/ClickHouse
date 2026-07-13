#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Retire outcomes (spec §7 R4) — what the recheck decided per retired entry. One object per
/// gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>; written once (`putIfAbsent` —
/// idempotent replay reads and verifies). 412-saves (replaced) are a health metric.
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
    BlobRef ref{};
    Token token;                 /// the retired (condemned) token the recheck acted on
    OutcomeKind outcome = OutcomeKind::Spared;
};

/// Outcome log (`GcOutcomeLogProto`, magic CAGO), one object per
/// gc/gen/<generation>/attempt/<attempt>/outcomes/<round>/<shard>: a `CasHeader` followed by a
/// repeated `GcOutcomeEntryProto` list (kind/hash/token/token_type/outcome/hash_algo per entry, in
/// insertion order).
struct OutcomeLog
{
    std::vector<OutcomeEntry> entries;
};

String encodeOutcomeLog(const OutcomeLog & log);
OutcomeLog decodeOutcomeLog(std::string_view data);

}
