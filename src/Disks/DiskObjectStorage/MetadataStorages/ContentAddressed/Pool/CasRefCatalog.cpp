#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Common/Exception.h>
#include <fmt/format.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

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

/// Live-lock brake, the same shape and for the same reason as `publishCkpt`'s/`allocateWriterEpoch`'s
/// on their own contended token-CAS singletons: the catalog is ONE object mutated by every lifecycle
/// transition of every namespace in the pool, so persistent contention is a real, not theoretical,
/// exit condition to plan for.
constexpr size_t kMaxCatalogCasAttempts = 100;

/// Shared body of `casUpdate`/`casAdmitEntry`. `encode` turns a freshly `mutate`d candidate into the
/// bytes to write: the plain path just grammar-checks (`encodeRefCatalog`), the admitting path also
/// runs both admission predicates (`checkCatalogAdmission`) first. Retries on `Conflict` against a
/// FRESH read, exactly like `PoolMeta::admitOrValidate` -- never re-encoding the stale candidate.
RefCatalog casUpdateImpl(
    Backend & backend, const Layout & layout,
    const std::function<RefCatalog(const RefCatalog &)> & mutate,
    const std::function<String(const RefCatalog &)> & encode)
{
    const String key = layout.refCatalogKey();
    CasRefCatalog::Snapshot snap = CasRefCatalog::read(backend, layout);

    for (size_t attempt = 0; attempt < kMaxCatalogCasAttempts; ++attempt)
    {
        const bool existed_before = snap.token.has_value();
        RefCatalog candidate = mutate(snap.catalog);
        const String bytes = encode(candidate);
        const CasResult res = backend.casPut(key, bytes, snap.token);
        if (res.outcome == CasOutcome::Committed)
            return candidate;

        snap = CasRefCatalog::read(backend, layout);
        /// `Backend::get` returns `nullopt` only for authoritative absence (spec of every backend in
        /// this tree), so an object that existed a moment ago (we just read a token for it) and is
        /// now genuinely absent is a REAL concurrent delete, never a bootstrap. Falling back to an
        /// empty catalog here -- the correct behaviour for the very first, pre-loop read -- would let
        /// the NEXT attempt's create-if-absent `casPut` replace EVERY other namespace's entry with
        /// whatever this one mutation produced. `PoolMeta::admitOrValidate` fails closed on the
        /// identical observation ("vanished mid-admission"); this does the same.
        if (existed_before && !snap.token.has_value())
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS ref catalog: '{}' vanished mid-update (conflicting write then a concurrent "
                "delete) -- refusing to replace it with a fresh catalog containing only this update",
                key);
    }

    throwCasWriteRetryLater(fmt::format(
        "CAS ref catalog '{}' did not converge after {} attempts", key, kMaxCatalogCasAttempts));
}

}

RefCatalog CasRefCatalog::casUpdate(
    Backend & backend, const Layout & layout, const std::function<RefCatalog(const RefCatalog &)> & mutate)
{
    return casUpdateImpl(backend, layout, mutate, [](const RefCatalog & c) { return encodeRefCatalog(c); });
}

RefCatalog CasRefCatalog::casAdmitEntry(Backend & backend, const Layout & layout, const CatalogEntry & entry)
{
    /// The mutation shape is FIXED (insert `entry` at its canonical position) rather than a
    /// caller-supplied lambda -- see the header comment on why that is the point, not an
    /// inconvenience. A namespace that already has an entry is not de-duplicated here: the insert
    /// makes the candidate carry two adjacent equal-ns rows, and `encodeRefCatalog`'s own
    /// canonical-order/no-duplicate check (run inside `checkCatalogAdmission` below) rejects that
    /// shape -- one place owns that rule, not two.
    const auto mutate = [&entry](const RefCatalog & cur) -> RefCatalog
    {
        RefCatalog next = cur;
        const auto it = std::lower_bound(next.entries.begin(), next.entries.end(), entry,
            [](const CatalogEntry & a, const CatalogEntry & b) { return a.ns.string() < b.ns.string(); });
        next.entries.insert(it, entry);
        return next;
    };
    return casUpdateImpl(backend, layout, mutate,
        [&entry](const RefCatalog & c) { return checkCatalogAdmission(c, entry.ns); });
}

}
