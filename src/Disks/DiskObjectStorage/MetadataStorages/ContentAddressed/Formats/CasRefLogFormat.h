#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h>
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

/// v3 text codec for `cas_ref_log` (codecs-v3 phase 3): the immutable ref transaction log object at
/// `_log/<txn_id>` — ONE object = ONE committed transaction (a batch of `RefOp`s). Control family,
/// Always/`.zst` (the caller seals; see the note on `encodeRefLogTxn`). Text shape: header line + a meta
/// line `{"ns","we","rs"}` + one JSON record per op + `{"n":count}` trailer. Deterministic by
/// construction (ops in stored order, no timestamps beyond the op's own field) so a re-encode is
/// byte-identical, but NOT `putDeterministicArtifact`-gated (refs commit via `putIfAbsentControlled`).

/// One transaction operation kind (spec §Transaction Log Format).
enum class RefOpKind : uint8_t
{
    NamespaceBirth = 1,
    OwnerTransition = 2,
    SetPayload = 3,
    RemoveNamespace = 4,
};

/// One operation inside a `RefLogTxn`. Only the fields documented next to `kind` are meaningful for
/// that kind; the codec never reads or writes the others. (Moved verbatim from the deleted
/// `CasRefLogCodec.h`; `RefOwnerBinding` now lives in `CasRefWireVocab.h`.)
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

/// One immutable ref-transaction log object (spec §Transaction Log Format). `ns`/`txn_id` are repeated
/// in the body even though both are key-derived; `decodeRefLogTxn` cross-checks them against the
/// caller's expected values (the key↔body binding).
struct RefLogTxn
{
    String ns;
    RefTxnId txn_id;
    std::vector<RefOp> ops;

    bool operator==(const RefLogTxn &) const = default;
};

/// Hard limits (spec §Byte, Memory, And CPU Budget), enforced by the codec at BOTH encode and decode,
/// now measured over the JSON text bytes (codecs-v3 phase 3). A `RemoveNamespace`-bearing txn is
/// "removal-class": it shares the larger complete-table byte budget and carries no op-count cap.
inline constexpr size_t ref_txn_max_ops = 1000;
inline constexpr size_t ref_txn_max_bytes = 1024 * 1024;
inline constexpr size_t ref_removal_max_bytes = 64 * 1024 * 1024;

/// Encode to the canonical TEXT (NOT sealed): the caller compresses via `sealObject(FormatId::RefLog, …)`
/// on the persist path (the Always/`.zst` policy), exactly like `cas_gc_outcomes`. Returning text keeps
/// the byte-budget self-check (below) and the `admits`/preview size-estimate callers measuring the
/// text size, and keeps the in-memory validation round-trip (`CasRefStateMachine`) uncompressed.
/// Throws CORRUPTED_DATA on: a zero `txn_id` field, a non-canonical `ref_name`, an out-of-range
/// `manifest_ref`, an unknown op kind, or an op-count/byte-limit violation over the encoded text.
String encodeRefLogTxn(const RefLogTxn & txn);

/// Decode the canonical TEXT (the caller `openObject`s the stored `.zst` first). `expected_ns` /
/// `expected_txn_id` are recovered from the object key; the decoded body must equal them or
/// CORRUPTED_DATA (the key↔body binding). Throws UNKNOWN_FORMAT_VERSION for a header `v` above this
/// build, CORRUPTED_DATA for truncation, an unknown op/owner kind, a non-canonical `ref_name`, a zero
/// txn-id field, a body/key mismatch, or a limit violation.
RefLogTxn decodeRefLogTxn(std::string_view data, const String & expected_ns, const RefTxnId & expected_txn_id);

}
