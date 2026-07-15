#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIds.h>
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

/// One transaction operation kind (spec §Transaction Log Format). The wire tag; a decoded op carrying
/// any other value fails closed.
enum class RefOpKind : uint8_t
{
    NamespaceBirth = 1,
    OwnerTransition = 2,
    SetPayload = 3,
    RemoveNamespace = 4,
};

/// Which slot of the table an `RefOwnerBinding` occupies (spec §State Transitions).
enum class RefOwnerKind : uint8_t
{
    Committed = 1,
    Precommit = 2,
};

/// One (ref name -> manifest) binding inside an `OwnerTransition` op (spec §Transaction Log Format).
/// The build identity of a precommit is {manifest_ref.writer_epoch, manifest_ref.build_sequence} --
/// there is no second build token.
struct RefOwnerBinding
{
    RefOwnerKind kind = RefOwnerKind::Committed;
    String ref_name;
    ManifestRef manifest_ref;

    bool operator==(const RefOwnerBinding &) const = default;
};

/// One operation inside a `RefLogTxn` (spec §Transaction Log Format). Only the fields documented next
/// to `kind` are meaningful for that kind; the codec never reads or writes the others.
struct RefOp
{
    RefOpKind kind = RefOpKind::NamespaceBirth;

    std::optional<RefOwnerBinding> old_binding;    /// OwnerTransition: absent = pure add
    std::optional<RefOwnerBinding> new_binding;    /// OwnerTransition: absent = pure removal

    String ref_name;                               /// SetPayload
    ManifestRef expected_manifest_ref;              /// SetPayload
    String payload;                                 /// SetPayload -- opaque wire carrier; all-tree-
                                                     /// part-files Task 9: production never populates
                                                     /// this anymore (always empty), but the field
                                                     /// stays on the wire (codec/state-machine test
                                                     /// coverage keys off it as a generic byte carrier)
    uint64_t published_at_ms = 0;                   /// SetPayload

    bool operator==(const RefOp &) const = default;
};

/// One immutable ref-transaction log object (spec §Transaction Log Format, §One Log Encoding): the
/// complete body stored at `_log/<txn_id>`. `ns`/`txn_id` are repeated here even though both are also
/// key-derived; `decodeRefLogTxn` cross-checks the decoded values against the caller's expected ones.
struct RefLogTxn
{
    String ns;
    RefTxnId txn_id;
    std::vector<RefOp> ops;

    bool operator==(const RefLogTxn &) const = default;
};

/// Hard limits (spec §Transaction Log Format, §Byte, Memory, And CPU Budget). A normal transaction is
/// capped on both operation count and encoded bytes. A transaction containing a `RemoveNamespace` op
/// is "removal-class": it shares the larger complete-table byte budget also used for snapshots and
/// carries no separate operation-count cap (the byte budget bounds it).
inline constexpr size_t ref_txn_max_ops = 1000;
inline constexpr size_t ref_txn_max_bytes = 1024 * 1024;
inline constexpr size_t ref_removal_max_bytes = 64 * 1024 * 1024;

/// Deterministic encode: `u32 format_version=1 | u32 ns_len+bytes | u64 writer_epoch | u64 ref_sequence
/// | u32 op_count | ops...` (fixed-width little-endian integers, u32-length-prefixed byte strings).
/// Throws CORRUPTED_DATA if `txn.txn_id` has a zero field, any `ref_name` is not a canonical clean
/// relative path (empty, ".", "..", a repeated/leading/trailing separator, or a backslash), an op
/// carries an unrecognized kind, or the encoded result would violate the op-count/byte limits above --
/// the codec never emits a body it would then refuse to decode (mirrors `encodePartManifest`, which
/// applies its own invariant checks -- duplicate paths -- at encode time too).
String encodeRefLogTxn(const RefLogTxn & txn);

/// Decode. `expected_ns`/`expected_txn_id` are the values recovered from the object key; the decoded
/// body's `ns`/`txn_id` must equal them exactly (spec §Object Layout: "Key-derived fields are repeated
/// in each body and must agree with the key.") -- a valid body copied under the wrong key is rejected
/// as corruption, not silently accepted. Throws UNKNOWN_FORMAT_VERSION for a `format_version` other
/// than 1, and CORRUPTED_DATA for a truncated buffer, an unknown op/owner kind, a non-canonical
/// `ref_name`, a zero txn-id field, a body/key mismatch, or a limit violation.
RefLogTxn decodeRefLogTxn(std::string_view data, const String & expected_ns, const RefTxnId & expected_txn_id);

}
