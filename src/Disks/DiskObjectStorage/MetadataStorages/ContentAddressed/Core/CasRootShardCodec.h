#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
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
/// Non-hashed metadata object => binary protobuf (`Proto/cas_root_shard.proto`, `RootShardManifest`,
/// B164a; replaced the earlier strict-JSON encoding). The manifest is CAS-by-token, not
/// content-addressed, so byte-determinism is not a correctness requirement; the encoder still
/// serializes deterministically (name-sorted refs via std::map; journal in insertion order). `refs`
/// maps ref name → its tree + per-ref mutable sidecar files; `journal` is the reachability-delta list
/// (`op`: add | remove). Version and fail-closed gating live in the `CasFormat` framing header
/// (`[magic CARS][writer:u16][min_reader:u16]`) prepended before the protobuf body; `decodeRootShard`
/// reads and gates the header BEFORE parsing the protobuf body. Fail-closed decode (unknown/missing
/// required field / wrong type / bad hash length / bad enum => CORRUPTED_DATA). Introspect a raw
/// manifest offline with `protoc --decode DB.Cas.Proto.RootShardManifest cas_root_shard.proto`.

/// One tree node of a precommit's inline closure: the node's tree hash and its staged entries.
/// Rides the precommit-namespace journal `Add` record (B199-S2): it survives the commit/abandon
/// `refs.erase` and is trimmed only after GC folds it.
/// Only placement/file_hash/file_size are serialized (the closure only feeds the GC walk;
/// name/inline_bytes are NOT needed and intentionally omitted — decode sets them to their defaults).
struct ClosureNode
{
    UInt128 tree_hash{};
    std::vector<TreeEntry> entries;
    bool operator==(const ClosureNode &) const = default;
};

/// The per-ref payload: the content tree plus the mutable per-ref files (txn_version.txt,
/// metadata_version.txt, ...) that are excluded from the content hash and live here per-ref.
struct RefPayload
{
    UInt128 tree_id{};
    uint64_t tree_size = 0;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
    bool operator==(const RefPayload &) const = default;
};

/// One reachability-delta record appended on publish.
struct JournalRecord
{
    enum class Op : uint8_t { Add = 1, Remove = 2 };
    Op op = Op::Add;
    String ref_name;
    UInt128 tree_id{};
    uint64_t at_version = 0;
    std::vector<ClosureNode> closure;   /// populated only on precommit-ns Add records (B199-S2)
    bool operator==(const JournalRecord &) const = default;
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

/// Decodes a manifest. First reads and gates the `CasFormat` framing header (`[magic CARS][writer:u16]
/// [min_reader:u16]`), then parses the protobuf body. Throws CORRUPTED_DATA on bad magic, malformed
/// protobuf, a bad enum (`JournalOp` unspecified), a wrong-length hash, or any field-level
/// inconsistency; UNKNOWN_FORMAT_VERSION when `min_reader` exceeds this build's `G_BUILD`.
RootShard decodeRootShard(std::string_view data);

}
