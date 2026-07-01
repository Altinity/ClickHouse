#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace DB::Cas
{

/// A `{writer_epoch, build_sequence}` pair that identifies the incarnation of a ref-shard.
/// `{0, 0}` is the unstamped (legacy / never-created) sentinel — a valid value, not an error.
/// Used in `RootShard::incarnation` and `ShardCoverage::incarnation`.
struct ShardIncarnation
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    bool operator==(const ShardIncarnation &) const = default;
    bool operator<(const ShardIncarnation & o) const
    {
        return std::tie(writer_epoch, build_sequence) < std::tie(o.writer_epoch, o.build_sequence);
    }
};

/// Root-shard manifest codec — the only mutable object and the single commit point (protocol spec §3).
/// CA GC root-local part-manifest redesign (rev. 15): the root journal is ONE ordered stream of
/// `RootOwnerEvent` in `transition_version` order. There are no separate transitions/precommits/
/// promotions vectors, and there is no `ClosureNode`/`JournalRecord`/`RefPayload` — only blobs stay
/// content-addressed; a part is a single-owner namespace-qualified `ManifestId`. The GC fold reads the
/// single journal and dispatches each event by comparing `old_binding.manifest_ref` with
/// `new_binding.manifest_ref`: equal ⇒ owner move (no blob delta, no cleanup); true removal ⇒ `-1` +
/// cleanup; activation ⇒ `+1` (subject to the fold barrier).
///
/// Non-hashed metadata object => binary protobuf (`Proto/cas_format.proto`, `RootShardManifest`).
/// The manifest is CAS-by-token, not content-addressed; the encoder still serializes deterministically
/// (name-sorted refs via std::map; journal in insertion order, which is transition_version order).
/// Version + fail-closed gating live in the `CasHeader` field 1; `decodeRootShard` checks the magic and
/// `compatibility_version` BEFORE reading any other field. Fail-closed decode (unknown OwnerKind, a
/// `manifest_ordinal`/`build_id` whose length != 16, a `RootOwnerEvent` with neither binding set,
/// or a bad envelope => CORRUPTED_DATA).

/// Owner of a part manifest in the root journal: a committed ref or a precommit build intent.
enum class OwnerKind : uint8_t
{
    Committed = 1,
    Precommit = 2,
};

/// One owner binding: which kind of owner names which manifest. Committed: `ref_name` set,
/// `build_id` = 0. Precommit: `ref_name` = the final committed ref name, `build_id` set. Carries the
/// full `ManifestRef`, never a bare nonce.
struct OwnerBinding
{
    OwnerKind owner_kind = OwnerKind::Committed;
    String ref_name;                /// committed ref_name, or the precommit's final_ref_name
    UInt128 build_id{};             /// 0 for Committed; the build id for Precommit
    ManifestRef manifest_ref;
    bool operator==(const OwnerBinding &) const = default;
};

/// One ordered owner-change event in the SINGLE root journal stream (spec §Root Journal Format).
/// Removes at most one `old_binding` and adds at most one `new_binding`; folded in transition_version
/// order. create precommit = old none / new {Precommit,…}; abandon = old {Precommit,…} / new none;
/// publish committed = old none / new {Committed,…}; drop = old {Committed,…} / new none; repoint =
/// old {Committed,ref,T_old} / new {Committed,ref,T_new}; promote = old {Precommit,final,build,T} /
/// new {Committed,final,T} (SAME manifest_ref T ⇒ owner move, blob Δ = 0, no cleanup).
struct RootOwnerEvent
{
    uint64_t transition_version = 0;
    std::optional<OwnerBinding> old_binding;
    std::optional<OwnerBinding> new_binding;
    bool operator==(const RootOwnerEvent &) const = default;
};

/// The current committed ref payload in the root journal. Carries the committed `ManifestRef` plus the
/// mutable per-ref files (txn_version.txt, metadata_version.txt, ...). `root_namespace_id` is NOT
/// stored here — it comes from the owning root context (spec §Root Journal Format).
struct RootRef
{
    String ref_name;
    ManifestRef manifest_ref;
    std::map<String, String> mutable_files;
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
    bool operator==(const RootRef &) const = default;
};

struct RootShard
{
    uint64_t shard_version = 0;
    uint64_t fence_round = 0;
    ShardIncarnation incarnation;              /// {0,0} = unstamped (legacy/never-created); stamped by Task 2+
    std::map<String, RootRef> refs;            /// std::map keeps refs in canonical name order
    std::vector<RootOwnerEvent> journal;       /// ONE ordered stream, folded in transition_version order
    bool operator==(const RootShard &) const = default;
};

/// Encodes the manifest. Refs are serialized in name-sorted order (std::map already gives it); the
/// journal is preserved in insertion order (== transition_version order). Each `manifest_ordinal`
/// and each `build_id` is encoded as exactly 16 bytes.
String encodeRootShard(const RootShard & root);

/// Decodes a manifest. Checks the `CasHeader` magic + compatibility_version before any other field,
/// then maps the protobuf back. Throws CORRUPTED_DATA on bad magic, malformed protobuf, an unknown
/// OwnerKind, a 16-byte field of the wrong length, or a RootOwnerEvent with neither binding set;
/// UNKNOWN_FORMAT_VERSION when compatibility_version exceeds this build's G_BUILD.
RootShard decodeRootShard(std::string_view data);

}
