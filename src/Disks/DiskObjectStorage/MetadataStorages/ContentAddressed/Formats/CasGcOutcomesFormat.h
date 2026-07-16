#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobEnvelopeFormat.h>
#include <base/types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Retire outcomes (spec §7 R4): what the recheck decided per retired entry. One object per
/// `gc/gen/{g}/attempt/{a}/outcomes/{round}/{shard}`, written once. v3 text: header line + one flat
/// JSON record per entry (insertion order) + {"n":count} trailer; Always-compressed (.zst key).
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

struct OutcomeLog
{
    std::vector<OutcomeEntry> entries;
};

String encodeOutcomeLog(const OutcomeLog & log);
OutcomeLog decodeOutcomeLog(std::string_view data);

}
