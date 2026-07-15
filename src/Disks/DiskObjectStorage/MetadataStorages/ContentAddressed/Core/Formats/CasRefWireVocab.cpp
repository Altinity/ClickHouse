#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasRefWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Common/Exception.h>
#include <IO/WriteHelpers.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

std::string_view refOwnerKindToWord(RefOwnerKind k)
{
    switch (k)
    {
        case RefOwnerKind::Committed: return "committed";
        case RefOwnerKind::Precommit: return "precommit";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS ref wire: unknown RefOwnerKind {}", static_cast<int>(k));
}

RefOwnerKind refOwnerKindFromWord(std::string_view w, std::string_view what)
{
    if (w == "committed") return RefOwnerKind::Committed;
    if (w == "precommit") return RefOwnerKind::Precommit;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown owner kind '{}'", what, w);
}

void writeManifestRefFields(WriteBuffer & out, bool & first, std::string_view prefix, const ManifestRef & r)
{
    /// Concatenate the prefix into each key name. Keys stay 2-5 chars ("me"/"mb"/"mo", or "ome"/"nmo").
    const String me = String(prefix) + "me";
    const String mb = String(prefix) + "mb";
    const String mo = String(prefix) + "mo";
    writeKey(out, me, first);
    writeU64StringValue(out, r.writer_epoch);
    writeKey(out, mb, first);
    writeU64StringValue(out, r.build_sequence);
    writeKey(out, mo, first);
    writeIntText(r.manifest_ordinal, out);
}

ManifestRef manifestRefFromFields(uint64_t writer_epoch, uint64_t build_sequence, uint64_t manifest_ordinal,
                                  std::string_view caller, std::string_view what)
{
    if (manifest_ordinal > kMaxManifestOrdinal)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: {} manifest_ordinal {} out of range", caller, what, manifest_ordinal);
    ManifestRef r;
    r.writer_epoch = writer_epoch;
    r.build_sequence = build_sequence;
    r.manifest_ordinal = static_cast<uint32_t>(manifest_ordinal);
    /// Full field validation (nonzero we/bs, ordinal in [1, max]) — the same rule the binary codec applied.
    checkManifestRef(r, caller, what);
    return r;
}

}
