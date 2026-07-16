#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>

#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
    extern const Event CasMetaPut;
    extern const Event CasMetaCas;
    extern const Event CasMetaDelete;
}

namespace DB::Cas
{

std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    const String key = layout.blobMetaKey(ref);
    auto got = backend.get(key);
    if (!got)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(got->bytes), .etag = got->token};
}

CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CasMetaPut);
    const String key = layout.blobMetaKey(ref);
    return backend.casPut(key, encodeBlobMeta(meta), std::nullopt);
}

CasResult casMeta(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CasMetaCas);
    const String key = layout.blobMetaKey(ref);
    return backend.casPut(key, encodeBlobMeta(meta), expected);
}

DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected)
{
    ProfileEvents::increment(ProfileEvents::CasMetaDelete);
    const String key = layout.blobMetaKey(ref);
    return backend.deleteExact(key, expected);
}

}
