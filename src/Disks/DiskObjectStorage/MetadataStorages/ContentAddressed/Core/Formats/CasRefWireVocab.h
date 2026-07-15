#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// Shared refsnaplog wire sub-types + their JSON rendering (codecs-v3 phase 3). `RefOwnerBinding` and
/// its `RefOwnerKind` are used by BOTH ref codecs — the log (`OwnerTransition` op bindings) and the
/// snapshot (`precommits`) — so they live here rather than in either codec, and `CasRefLogFormat` /
/// `CasRefSnapshotFormat` both include this header (neither depends on the other). `ManifestRef` field
/// rendering (`writeManifestRefFields`/`manifestRefFromFields`) promoted to `CasWireVocab` in phase 6
/// (part manifest): it fit the manifest descriptor rendering exactly, so `CasRefLogFormat` /
/// `CasRefSnapshotFormat` keep calling it unqualified via this header's transitive include of
/// `CasWireVocab.h`. Backend-free (identifier layer only).

/// Which slot of the table an `RefOwnerBinding` occupies (spec §State Transitions). Moved verbatim from
/// the deleted `CasRefLogCodec.h`.
enum class RefOwnerKind : uint8_t
{
    Committed = 1,
    Precommit = 2,
};

/// One (ref name -> manifest) binding (spec §Transaction Log Format). Moved verbatim from the deleted
/// `CasRefLogCodec.h`. The build identity of a precommit is {manifest_ref.writer_epoch,
/// manifest_ref.build_sequence} -- there is no second build token.
struct RefOwnerBinding
{
    RefOwnerKind kind = RefOwnerKind::Committed;
    String ref_name;
    ManifestRef manifest_ref;

    bool operator==(const RefOwnerBinding &) const = default;
};

std::string_view refOwnerKindToWord(RefOwnerKind k);
RefOwnerKind refOwnerKindFromWord(std::string_view w, std::string_view what);

}
