#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefLogCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <base/types.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Whether a table is currently populated or has been removed (spec §Snapshot Format). `Removed`
/// makes namespace removal durable even after the `remove_namespace` log that caused it is eventually
/// cleaned up.
enum class RefLifecycle : uint8_t
{
    Live = 1,
    Removed = 2,
};

/// One committed (ref name -> manifest) row (spec's `CommittedRef`): the complete current mapping
/// entry. All-tree-part-files Task 9: `payload` is an opaque wire carrier production never populates
/// anymore (every per-part file is an ordinary manifest tree entry now) -- kept on the wire as a
/// generic byte carrier (codec/state-machine test coverage keys off it); `published_at_ms` is the
/// only field a caller still legitimately sets.
struct RefCommittedRow
{
    String ref_name;
    ManifestRef manifest_ref;
    String payload;
    uint64_t published_at_ms = 0;

    bool operator==(const RefCommittedRow &) const = default;
};

/// The complete state of one table in one deterministic object (spec §Snapshot Format), stored at
/// `_snap/<snapshot_id>.proto`. `precommits` reuses `RefOwnerBinding` from `CasRefLogCodec.h` rather
/// than a separate `PrecommitRef` type; every entry's `kind` must be `Precommit` (`encodeRefTableSnapshot`
/// / `decodeRefTableSnapshot` both enforce this -- it is never anything else in this list).
struct RefTableSnapshot
{
    String ns;
    RefTxnId snapshot_id;
    RefLifecycle lifecycle = RefLifecycle::Live;
    std::optional<RefTxnId> remove_txn_id;      /// present iff lifecycle == Removed
    std::optional<RefTxnId> sealed_from;        /// recovery seal upper bound (rev.6 §Recovery Seal); if
                                                 /// present, `*sealed_from <= snapshot_id`
    std::vector<RefCommittedRow> committed;     /// sorted by canonical bytewise ref_name, no duplicates
    std::vector<RefOwnerBinding> precommits;    /// sorted by (ref_name, manifest_ref), no duplicates

    bool operator==(const RefTableSnapshot &) const = default;
};

/// Hard encoded-size limit (spec §Snapshot Format, §Byte, Memory, And CPU Budget): "the same
/// complete-table limit class" bounds both the snapshot and the `remove_namespace` transaction, so
/// this reuses `ref_removal_max_bytes` (`CasRefLogCodec.h`) rather than a second independent constant.
inline constexpr size_t ref_snapshot_max_bytes = ref_removal_max_bytes;

/// Deterministic encode: `u32 format_version=2 | ns | snapshot_id | u8 lifecycle | [remove_txn_id if
/// Removed] | u8 has_sealed_from | [sealed_from] | u32 n_committed | committed rows... | u32
/// n_precommits | precommit rows...` (fixed-width little-endian integers, u32-length-prefixed byte
/// strings; `snapshot_id`/`remove_txn_id`/`sealed_from` as raw writer_epoch/ref_sequence u64 pairs, not
/// the hex render). No creation timestamp, attempt id, or map iteration order is ever written -- the
/// encoding is a pure function of `snapshot`'s contents.
///
/// Throws CORRUPTED_DATA if: `snapshot.snapshot_id` (or `remove_txn_id`/`sealed_from`, when present)
/// has a zero field; `lifecycle == Live` but `remove_txn_id` is set, or `lifecycle == Removed` but it
/// is unset or `committed`/`precommits` is non-empty; `sealed_from` is present and `snapshot_id <
/// *sealed_from`; any row's `ref_name` is not a canonical clean relative path; any row's `manifest_ref`
/// has a zero/out-of-range field; `committed` is not strictly ascending by `ref_name` (this also
/// rejects a duplicate ref_name -- there is exactly one committed manifest per name); `precommits` is
/// not strictly ascending by `(ref_name, manifest_ref)` (this also rejects an exact duplicate binding);
/// a `precommits` entry has `kind != Precommit`; or the encoded result would exceed
/// `ref_snapshot_max_bytes`.
String encodeRefTableSnapshot(const RefTableSnapshot & snapshot);

/// Decode. `expected_ns`/`expected_snapshot_id` are the values recovered from the object key; the
/// decoded body's `ns`/`snapshot_id` must equal them exactly (same key/body cross-check contract as
/// `decodeRefLogTxn`). Throws UNKNOWN_FORMAT_VERSION for a `format_version` other than 2 -- format
/// version 1 (no `sealed_from`) is rejected fail-closed, CAS is pre-release and carries no compat
/// scaffolding -- and CORRUPTED_DATA for a truncated buffer, an unknown lifecycle byte, an unrecognized
/// owner kind, or any validation failure listed on `encodeRefTableSnapshot`.
RefTableSnapshot decodeRefTableSnapshot(
    std::string_view data, const String & expected_ns, const RefTxnId & expected_snapshot_id);

}
