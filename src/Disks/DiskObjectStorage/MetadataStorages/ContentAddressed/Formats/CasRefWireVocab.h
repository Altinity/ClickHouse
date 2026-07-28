#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <base/types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// Shared identifier vocabulary for the ref-log and ref-snapshot text formats. `RefOwnerBinding` and
/// `RefOwnerKind` are used in log `OwnerTransition` records and snapshot `precommits`, so neither
/// format owns a duplicate definition. This header has no backend or storage dependencies; the
/// manifest-reference JSON helpers are shared by both ref formats and part-manifest descriptors
/// through `CasWireVocab.h`.

/// Identifies which ownership slot a `RefOwnerBinding` occupies. The numeric values are the
/// in-memory discriminators; text codecs render them as the words "committed" and "precommit".
enum class RefOwnerKind : uint8_t
{
    Committed = 1,
    Precommit = 2,
};

/// One ref-name-to-manifest ownership binding. Log transitions use it for the optional old and new
/// owners; snapshots use it for precommit rows, whose `kind` must be `Precommit`. A precommit's build
/// identity is the pair `{manifest_ref.writer_epoch, manifest_ref.build_sequence}`; the binding has
/// no additional build token. `manifest_ref` is validated by the format that decodes the binding.
struct RefOwnerBinding
{
    RefOwnerKind kind = RefOwnerKind::Committed;
    String ref_name;
    ManifestRef manifest_ref;

    bool operator==(const RefOwnerBinding &) const = default;
};

/// Convert an owner-kind discriminator to its canonical text word. Throws `CORRUPTED_DATA` for a
/// value not represented by this format; accepting an unknown value would produce an ambiguous wire
/// record.
std::string_view refOwnerKindToWord(RefOwnerKind k);

/// Parse a canonical owner-kind word. `what` identifies the containing field in the
/// `CORRUPTED_DATA` exception. Unknown words are rejected rather than treated as a default kind.
RefOwnerKind refOwnerKindFromWord(std::string_view w, std::string_view what);

/// Append two flat `RefTxnId` fields -- its `writer_epoch` and `ref_sequence` components -- to an
/// in-progress JSON object, both as decimal STRINGS (`ref_sequence` reaches `UINT64_MAX` for a
/// recovery seal). `epoch_key`/`seq_key` name the two fields, letting each format distinguish its
/// primary id from any secondary id it embeds (`cas_ref_log`'s `we`/`rs` vs its `prev_epoch_seal`
/// pair; `cas_ref_snap`'s `we`/`rs` vs `remove_txn_id` vs `sealed_from`) while sharing one writer so
/// the formats can never disagree on the representation.
void writeRefTxnIdFields(CasJsonWriter & out, bool & first, std::string_view epoch_key, std::string_view seq_key, const RefTxnId & id);

}
