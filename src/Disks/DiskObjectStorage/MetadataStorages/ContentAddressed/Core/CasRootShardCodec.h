#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Root-shard manifest codec — the only mutable object and the single commit point (protocol spec §3).
/// One manifest names a set of refs (parts) → their tree, plus the per-ref mutable sidecar files and
/// an embedded reachability journal. Magic "CARS", version 1.

/// The per-ref payload: the content tree plus the mutable per-part files (txn_version.txt,
/// metadata_version.txt, ...) that are excluded from the content hash and live here per-ref.
struct RefPayload
{
    UInt128 tree_id{};
    uint64_t tree_size = 0;
    std::map<String, String> mutable_files;
};

/// One reachability-delta record appended on publish.
struct JournalRecord
{
    enum class Op : uint8_t { Add = 1, Remove = 2 };
    Op op = Op::Add;
    String ref_name;
    UInt128 tree_id{};
    uint64_t at_version = 0;
};

struct RootShard
{
    uint64_t shard_version = 0;
    uint64_t fence_round = 0;
    std::map<String, RefPayload> refs;    /// std::map keeps refs in canonical name order
    std::vector<JournalRecord> journal;   /// preserved in insertion order
};

/// Encodes the manifest. Refs are serialized in name-sorted order (std::map already gives it); the
/// journal is preserved in insertion order; mutable files within a ref are name-sorted (std::map).
String encodeRootShard(const RootShard & root);

/// Decodes a manifest. Throws CORRUPTED_DATA on bad magic, future version, truncation, or a journal
/// op outside {1,2}.
RootShard decodeRootShard(std::string_view data);

}
