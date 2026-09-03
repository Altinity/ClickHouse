#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>

#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
    extern const Event CASMetaPut;
    extern const Event CASMetaCompareSwap;
    extern const Event CASMetaDelete;
}

namespace DB::Cas
{

std::optional<LoadedMeta> loadMeta(CasOperation & op, const Layout & layout, const BlobRef & ref)
{
    auto got = op.read(layout.blobMetaKey(ref), Retry::standard());
    if (!got)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(got->bytes), .incarnation = std::move(got->incarnation)};
}

WriteResult putMetaIfAbsent(Pool & pool, const BlobRef & ref, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CASMetaPut);
    const String key = pool.layout().blobMetaKey(ref);
    return pool.stagingPutIfAbsentMutable(key, encodeBlobMeta(meta));
}

WriteResult casMeta(CasOperation & op, const Layout & layout, const BlobRef & ref,
                    const Incarnation & expected, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CASMetaCompareSwap);
    return op.replace(layout.blobMetaKey(ref), encodeBlobMeta(meta), expected, Retry::standard());
}

Removal deleteMetaExact(CasOperation & op, const Layout & layout, const BlobRef & ref,
                        const Incarnation & expected)
{
    ProfileEvents::increment(ProfileEvents::CASMetaDelete);
    return op.remove(layout.blobMetaKey(ref), expected, Retry::standard());
}

}
