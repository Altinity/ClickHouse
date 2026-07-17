#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Text codec for `cas_ref_snap`, the complete per-namespace ref table snapshot at
/// `_snap/<snapshot_id>`. The object is read whole rather than streamed and belongs to the Control
/// family: callers store the encoded text as an Always/`.zst` object. Its canonical text consists of
/// a header, a metadata line, committed and precommit row lines, and a `{"n":count}` trailer.
///
/// A recovery seal is represented by the same snapshot type: it is a Live snapshot whose
/// `sealed_from` is set and whose `snapshot_id` is the synthetic `{my_epoch - 1, UINT64_MAX}`. All
/// `RefTxnId` components are encoded as decimal strings so this sentinel value round-trips. Rows are
/// emitted in canonical order, making re-encoding deterministic by construction; these objects are
/// still published through the ordinary single-owner `putIfAbsentControlled` path, not a
/// `putDeterministicArtifact` byte-adoption gate.

/// Whether a table is currently populated or has been removed.
enum class RefLifecycle : uint8_t
{
    Live = 1,
    Removed = 2,
};

/// One committed ref-name-to-manifest row in a `RefTableSnapshot`. `payload` is an opaque wire
/// carrier that production no longer populates but the format retains for compatibility and test
/// coverage; `published_at_ms` is the only field current callers set.
struct RefCommittedRow
{
    String ref_name;
    ManifestRef manifest_ref;
    String payload;
    uint64_t published_at_ms = 0;

    bool operator==(const RefCommittedRow &) const = default;
};

/// The complete state of one namespace's ref table in one canonical snapshot object. `precommits`
/// reuses `RefOwnerBinding` from `CasRefWireVocab.h`; every entry's `kind` must be `Precommit`.
/// A Removed snapshot carries its nonzero `remove_txn_id` and no rows. A Live snapshot has no
/// `remove_txn_id`; when `sealed_from` is present it is nonzero and no later than `snapshot_id`.
/// Both row vectors must already be strictly sorted by their documented keys, because the codec
/// validates and emits the caller-provided order rather than sorting it.
struct RefTableSnapshot
{
    String ns;
    RefTxnId snapshot_id;
    RefLifecycle lifecycle = RefLifecycle::Live;
    std::optional<RefTxnId> remove_txn_id;      /// present iff lifecycle == Removed
    std::optional<RefTxnId> sealed_from;        /// recovery seal upper bound;
                                                 /// if present, `*sealed_from <= snapshot_id`
    std::vector<RefCommittedRow> committed;     /// sorted by canonical bytewise ref_name, no duplicates
    std::vector<RefOwnerBinding> precommits;    /// sorted by (ref_name, manifest_ref), no duplicates

    bool operator==(const RefTableSnapshot &) const = default;
};

/// Hard encoded-size limit over the uncompressed text. The snapshot reuses the removal-class
/// complete-table budget from `CasRefLogFormat.h`.
inline constexpr size_t ref_snapshot_max_bytes = ref_removal_max_bytes;

/// Encode to the canonical text (not sealed): the caller compresses via
/// `sealObject(FormatId::RefSnapshot, …)` on the persist path (Always/`.zst`), and the in-memory
/// validation / `admits` size-estimate / `fsck` oracle callers use the uncompressed text. Throws
/// CORRUPTED_DATA on: a zero `snapshot_id`/`remove_txn_id`/`sealed_from` field; the Live/Removed
/// coupling broken (Live with `remove_txn_id`, or Removed without it / with rows); `snapshot_id <
/// *sealed_from`; a non-canonical `ref_name`; an out-of-range `manifest_ref`; non-strictly-ascending
/// `committed` / `precommits`; a `precommits` entry not `Precommit`; or an over-budget object.
String encodeRefTableSnapshot(const RefTableSnapshot & snapshot);

/// Decode the canonical text (the caller `openObject`s the stored `.zst` first). `expected_ns` /
/// `expected_snapshot_id` are recovered from the object key; the decoded body must equal them (the
/// key↔body binding). Throws UNKNOWN_FORMAT_VERSION for a header `v` above this build, CORRUPTED_DATA
/// for truncation, an unknown lifecycle/owner kind, or any validation failure listed above.
RefTableSnapshot decodeRefTableSnapshot(
    std::string_view data, const String & expected_ns, const RefTxnId & expected_snapshot_id);

}
