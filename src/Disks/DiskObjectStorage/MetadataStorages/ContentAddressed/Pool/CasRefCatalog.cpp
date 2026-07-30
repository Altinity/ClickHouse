#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>

namespace DB::Cas
{

CasRefCatalog::Snapshot CasRefCatalog::read(Backend & backend, const Layout & layout)
{
    const auto got = backend.get(layout.refCatalogKey());
    if (!got)
        return Snapshot{.catalog = RefCatalog{}, .token = std::nullopt};
    return Snapshot{.catalog = decodeRefCatalog(got->bytes), .token = got->token};
}

namespace
{

/// Shared body of `casUpdate`/`casUpdateAdmitting`. `encode` turns a freshly `mutate`d candidate into
/// the bytes to write: the plain path just grammar-checks (`encodeRefCatalog`), the admitting path
/// also runs both admission predicates (`checkCatalogAdmission`) first. Retries on `Conflict` against
/// a FRESH read, exactly like `PoolMeta::admitOrValidate` -- never re-encoding the stale candidate.
RefCatalog casUpdateImpl(
    Backend & backend, const Layout & layout,
    const std::function<RefCatalog(const RefCatalog &)> & mutate,
    const std::function<String(const RefCatalog &)> & encode)
{
    CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);
    for (;;)
    {
        RefCatalog candidate = mutate(snap.catalog);
        const String bytes = encode(candidate);
        const CasResult res = backend.casPut(layout.refCatalogKey(), bytes, snap.token);
        if (res.outcome == CasOutcome::Committed)
            return candidate;

        snap = CasRefCatalog::read(backend, layout);
    }
}

}

RefCatalog CasRefCatalog::casUpdate(
    Backend & backend, const Layout & layout, const std::function<RefCatalog(const RefCatalog &)> & mutate)
{
    return casUpdateImpl(backend, layout, mutate, [](const RefCatalog & c) { return encodeRefCatalog(c); });
}

RefCatalog CasRefCatalog::casUpdateAdmitting(
    Backend & backend, const Layout & layout, const RootNamespace & admitting_ns,
    const std::function<RefCatalog(const RefCatalog &)> & mutate)
{
    return casUpdateImpl(backend, layout, mutate,
        [&admitting_ns](const RefCatalog & c) { return checkCatalogAdmission(c, admitting_ns); });
}

}
