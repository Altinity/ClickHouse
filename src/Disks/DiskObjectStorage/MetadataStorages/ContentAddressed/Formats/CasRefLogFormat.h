#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <base/types.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Text codec for `cas_ref_log`, the immutable object stored at `_log/<txn_id>`. Each object contains
/// exactly one committed transaction: its namespace, transaction id, and the batch of `RefOp`s applied
/// by that commit. The body has a header, a meta line `{"ns","we","rs"}`, one JSON record per op,
/// and a `{"n":count}` trailer. Records are emitted in the transaction's stored order and contain no
/// codec-generated timestamps, so encoding the same value is byte-identical. This determinism is a
/// property of the representation, not an adoption gate: ref commits use `putIfAbsentControlled`, and
/// the caller applies the `Always`/`.zst` storage policy by sealing the returned text.

/// One operation kind in a ref transaction log. The numeric values are part of the in-memory and
/// serialized representation; unknown values are rejected by the decoder.
enum class RefOpKind : uint8_t
{
    NamespaceBirth = 1,
    OwnerTransition = 2,
    SetPayload = 3,
    RemoveNamespace = 4,
};

/// One operation inside a `RefLogTxn`. Only the fields documented next to `kind` are meaningful for
/// that kind, and the codec never reads or writes the others. `OwnerTransition` optionally removes
/// `old_binding` and/or installs `new_binding`; `SetPayload` carries the expected manifest, an opaque
/// payload string retained for wire compatibility, and its publication timestamp. `RefOwnerBinding`
/// is shared with the snapshot format through `CasRefWireVocab.h`.
struct RefOp
{
    RefOpKind kind = RefOpKind::NamespaceBirth;

    std::optional<RefOwnerBinding> old_binding;    /// OwnerTransition: absent = pure add
    std::optional<RefOwnerBinding> new_binding;    /// OwnerTransition: absent = pure removal

    String ref_name;                               /// SetPayload
    ManifestRef expected_manifest_ref;             /// SetPayload
    String payload;                                /// SetPayload -- opaque wire carrier; production
                                                   /// never populates this anymore (always empty), but
                                                   /// the field stays on the wire (test coverage keys
                                                   /// off it as a generic byte carrier)
    uint64_t published_at_ms = 0;                  /// SetPayload

    bool operator==(const RefOp &) const = default;
};

/// The complete immutable body of one ref transaction log object. `ns` and `txn_id` are repeated in
/// the body even though both are key-derived. `decodeRefLogTxn` compares them with the values supplied
/// from the object key, rejecting a valid body copied under a different key as corruption.
struct RefLogTxn
{
    String ns;
    RefTxnId txn_id;
    std::vector<RefOp> ops;

    bool operator==(const RefLogTxn &) const = default;
};

/// Hard limits enforced by the codec at both encode and decode, measured over the JSON text bytes.
/// Normal transactions have both an operation-count and a byte limit. A transaction containing
/// `RemoveNamespace` is "removal-class": it shares the larger complete-table byte budget and has no
/// separate operation-count cap because that byte budget bounds it.
inline constexpr size_t ref_txn_max_ops = 1000;
inline constexpr size_t ref_txn_max_bytes = 1024 * 1024;
inline constexpr size_t ref_removal_max_bytes = 64 * 1024 * 1024;

/// Encode the transaction to canonical, uncompressed text. The persist path seals this text according
/// to the `Always`/`.zst` policy; keeping the codec unsealed lets the byte-budget check and preview
/// callers measure the actual text, and lets the state machine validate an uncompressed round-trip.
/// Operations retain their stored order. Throws CORRUPTED_DATA on a zero `txn_id` field, a
/// non-canonical `ref_name`, an out-of-range `manifest_ref`, an unknown op kind, or an op-count/byte
/// limit violation over the encoded text.
String encodeRefLogTxn(const RefLogTxn & txn);

/// Decode canonical text after the caller has opened the stored `.zst` object. `expected_ns` and
/// `expected_txn_id` come from the object key; the decoded body must equal them, otherwise the body/key
/// binding fails with CORRUPTED_DATA. Unknown non-critical fields are skipped so additive fields can be
/// introduced without changing this reader, while a future header version is rejected with
/// UNKNOWN_FORMAT_VERSION. Truncation, an unknown op or owner kind, a non-canonical `ref_name`, a zero
/// transaction-id field, a body/key mismatch, and a limit violation are reported as CORRUPTED_DATA.
RefLogTxn decodeRefLogTxn(std::string_view data, const String & expected_ns, const RefTxnId & expected_txn_id);

/// Encoded byte size of exactly one exact-owner-removal op line, as `buildHypotheticalRemovalTxn` +
/// `encodeRefLogTxn` would emit it (an owner_transition with only an old binding).
size_t removalOpEncodedSize(RefOwnerKind owner_kind, const String & ref_name, const ManifestRef & manifest_ref);

/// Encoded byte size of a removal transaction's framing (header + meta + terminal remove_namespace op +
/// trailer) for `op_count` total ops, excluding the per-owner removal op lines. `removalFramingSize(...)
/// + Σ removalOpEncodedSize` equals `encodeRefLogTxn(buildHypotheticalRemovalTxn(...)).size()` exactly.
size_t removalFramingSize(const String & ns, const RefTxnId & txn_id, uint64_t op_count);

}
