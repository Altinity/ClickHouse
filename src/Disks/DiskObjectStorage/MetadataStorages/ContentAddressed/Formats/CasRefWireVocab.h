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

}
