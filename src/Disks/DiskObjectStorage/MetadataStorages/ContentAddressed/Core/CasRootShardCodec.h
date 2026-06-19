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
/// One manifest names a set of refs → their tree, plus the per-ref mutable sidecar files and
/// an embedded reachability journal.
///
/// Non-hashed metadata object => STRICT JSON (spec §4 encoding split, decision 2026-06-11):
///   {"format":"cas_root_shard","version":1,"shard_version":7,"fence_round":2,
///    "refs":{"ref_1":{"tree":"<32 lowercase hex>","tree_size":123,
///                      "mutable_files":{"txn_version.txt":"42"}}},
///    "journal":[{"op":"add","ref":"ref_1","tree":"<32 lowercase hex>","at_version":7}]}
/// `refs` is a JSON object keyed by ref name (name-sorted, std::map order); `journal` is an array in
/// insertion order. `op`: "add" | "remove". Fail-closed decode (wrong format / unknown key / missing
/// key / wrong type / bad enum string / bad hash hex / malformed document => CORRUPTED_DATA; future
/// version => NOT_IMPLEMENTED).

/// The per-ref payload: the content tree plus the mutable per-ref files (txn_version.txt,
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

/// Decodes a manifest. Throws CORRUPTED_DATA on a wrong `format`, malformed JSON, an unknown/missing
/// key, a wrong field type, a bad hash hex, or a journal `op` outside {"add","remove"}; a future
/// `version` throws NOT_IMPLEMENTED.
RootShard decodeRootShard(std::string_view data);

}
