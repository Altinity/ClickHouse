#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
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
/// rendering is also shared with phase 6 (part manifest); if identical it may promote to
/// `CasWireVocab` when phase 6 lands (YAGNI until then). Backend-free (identifier layer only).

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

/// Append `,"<p>me":"<writer_epoch>","<p>mb":"<build_sequence>","<p>mo":<manifest_ordinal>` for a
/// ManifestRef under an optional key prefix `p` ("" for a bare row, "o"/"n" for the old/new binding of
/// an OwnerTransition op). writer_epoch / build_sequence are unbounded u64 -> decimal strings;
/// manifest_ordinal is bounded [1, kMaxManifestOrdinal] -> a JSON number.
void writeManifestRefFields(WriteBuffer & out, bool & first, std::string_view prefix, const ManifestRef & r);

/// Build a ManifestRef from the three decoded field values and validate it (`checkManifestRef` range
/// rules — writer_epoch/build_sequence nonzero, manifest_ordinal in [1, kMaxManifestOrdinal]).
/// `caller`/`what` name the codec + field in a CORRUPTED_DATA message.
ManifestRef manifestRefFromFields(uint64_t writer_epoch, uint64_t build_sequence, uint64_t manifest_ordinal,
                                  std::string_view caller, std::string_view what);

}
