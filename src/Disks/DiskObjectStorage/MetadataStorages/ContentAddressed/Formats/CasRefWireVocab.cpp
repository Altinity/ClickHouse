#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefWireVocab.h>
#include <Common/Exception.h>

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

}
