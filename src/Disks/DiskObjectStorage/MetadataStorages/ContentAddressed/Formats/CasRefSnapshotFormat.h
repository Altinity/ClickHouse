#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// v3 text codec for `cas_ref_snap` (codecs-v3 phase 3): the complete per-namespace ref table snapshot
/// at `_snap/<snapshot_id>`, read WHOLE (not streamed). Control family, Always/`.zst` (the caller
/// seals). Text shape: header line + a meta line (`ns`/`snapshot_id`/lifecycle, optional
/// `remove_txn_id` / `sealed_from`) + one committed-row line + one precommit-row line + `{"n":count}`
/// trailer. Carries the rev.6 RECOVERY SEAL verbatim: a seal is a Live snapshot with `sealed_from` set
/// at a synthetic `snapshot_id = {my_epoch-1, UINT64_MAX}` — every `RefTxnId` field is a decimal STRING
/// so `UINT64_MAX` round-trips. Deterministic by construction (sorted rows); NOT
/// `putDeterministicArtifact`-gated.

/// Whether a table is currently populated or has been removed (spec §Snapshot Format).
enum class RefLifecycle : uint8_t
{
    Live = 1,
    Removed = 2,
};

/// One committed (ref name -> manifest) row. `payload` is an opaque wire carrier production no longer
/// populates (kept for test coverage); `published_at_ms` is the only field a caller still sets.
struct RefCommittedRow
{
    String ref_name;
    ManifestRef manifest_ref;
    String payload;
    uint64_t published_at_ms = 0;

    bool operator==(const RefCommittedRow &) const = default;
};

/// The complete state of one table in one deterministic object (spec §Snapshot Format). `precommits`
/// reuses `RefOwnerBinding` (`CasRefWireVocab.h`); every entry's `kind` must be `Precommit`.
struct RefTableSnapshot
{
    String ns;
    RefTxnId snapshot_id;
    RefLifecycle lifecycle = RefLifecycle::Live;
    std::optional<RefTxnId> remove_txn_id;      /// present iff lifecycle == Removed
    std::optional<RefTxnId> sealed_from;        /// recovery seal upper bound (rev.6 §Recovery Seal);
                                                 /// if present, `*sealed_from <= snapshot_id`
    std::vector<RefCommittedRow> committed;     /// sorted by canonical bytewise ref_name, no duplicates
    std::vector<RefOwnerBinding> precommits;    /// sorted by (ref_name, manifest_ref), no duplicates

    bool operator==(const RefTableSnapshot &) const = default;
};

/// Hard encoded-size limit (spec §Byte, Memory, And CPU Budget), over the JSON text: reuses the
/// removal-class complete-table budget from `CasRefLogFormat.h`.
inline constexpr size_t ref_snapshot_max_bytes = ref_removal_max_bytes;

/// Encode to the canonical TEXT (NOT sealed): the caller compresses via
/// `sealObject(FormatId::RefSnapshot, …)` on the persist path (Always/`.zst`), and the in-memory
/// validation / `admits` size-estimate / `fsck` oracle callers use the uncompressed text. Throws
/// CORRUPTED_DATA on: a zero `snapshot_id`/`remove_txn_id`/`sealed_from` field; the Live/Removed
/// coupling broken (Live with `remove_txn_id`, or Removed without it / with rows); `snapshot_id <
/// *sealed_from`; a non-canonical `ref_name`; an out-of-range `manifest_ref`; non-strictly-ascending
/// `committed` / `precommits`; a `precommits` entry not `Precommit`; or an over-budget object.
String encodeRefTableSnapshot(const RefTableSnapshot & snapshot);

/// Decode the canonical TEXT (the caller `openObject`s the stored `.zst` first). `expected_ns` /
/// `expected_snapshot_id` are recovered from the object key; the decoded body must equal them (the
/// key↔body binding). Throws UNKNOWN_FORMAT_VERSION for a header `v` above this build, CORRUPTED_DATA
/// for truncation, an unknown lifecycle/owner kind, or any validation failure listed above.
RefTableSnapshot decodeRefTableSnapshot(
    std::string_view data, const String & expected_ns, const RefTxnId & expected_snapshot_id);

}
