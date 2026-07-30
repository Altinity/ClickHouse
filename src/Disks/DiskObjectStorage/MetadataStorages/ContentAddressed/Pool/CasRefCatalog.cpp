#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>
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

NamespaceLifeId CasRefCatalog::resolveLifeOrSentinel(Backend & backend, const Layout & layout, const RootNamespace & ns)
{
    const Snapshot snap = read(backend, layout);
    for (const CatalogEntry & entry : snap.catalog.entries)
        if (entry.ns.string() == ns.string() && entry.state != NsState::Creating)
            return NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation);
    return NamespaceLifeId::stageATransition(ns);
}

std::vector<NamespaceLifeId> CasRefCatalog::liveUniverse(Backend & backend, const Layout & layout)
{
    const Snapshot snap = read(backend, layout);
    std::vector<NamespaceLifeId> universe;
    universe.reserve(snap.catalog.entries.size());
    for (const CatalogEntry & entry : snap.catalog.entries)
    {
        if (entry.state == NsState::Creating)
            continue;
        universe.push_back(NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation));
    }
    return universe;
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

/// Thrown from inside a `casUpdate` `mutate` closure to signal a refusal that must STOP the attempt
/// rather than be treated as a `Conflict` to retry: `casUpdateImpl` propagates whatever `mutate`
/// throws straight out, uncaught, which is exactly the behavior these three need. Retrying any of them
/// against a freshly re-read catalog would just re-decide against an entry that is, by definition, no
/// longer `observed` -- token-exactness means the FIRST mismatch is final, not a reason to loop.
struct CatalogFenceMovedMarker {};
struct CatalogEntryMismatchMarker {};
struct CatalogCreatorStillLiveMarker {};

/// Two `thread_local_rng` draws composed into a `UInt128`, the same pattern already used throughout
/// this tree to mint build ids and incarnation tags (`CasPartWriteTxn.cpp`'s `mintU128`,
/// `ContentAddressedTransaction.cpp`'s `incarnation_tag`). Retried on the astronomically unlikely `0`
/// draw: unlike those callers, this value must never be zero (`CatalogEntry::incarnation == 0` is
/// always invalid -- "0 never names a life"), and this is the one mint site in that family with a
/// grammar rule to uphold.
UInt128 mintFreshIncarnation()
{
    UInt128 v = 0;
    while (v == 0)
        v = (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng();
    return v;
}

/// Shared by every function below that needs "the current entry for this namespace, if any" --
/// keeping ONE lookup rather than three independently-written `find_if`s that could drift apart on
/// what counts as a match.
std::vector<CatalogEntry>::const_iterator findEntry(const RefCatalog & catalog, const RootNamespace & ns)
{
    return std::find_if(catalog.entries.begin(), catalog.entries.end(),
        [&](const CatalogEntry & e) { return e.ns.string() == ns.string(); });
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

CasRefCatalog::NamespaceCreationOutcome CasRefCatalog::completeCreation(
    Backend & backend, const Layout & layout, const CatalogEntry & observed,
    uint64_t admitted_generation, const std::function<void(uint64_t)> & check_fence_or_throw,
    const CkptDeadline & deadline)
{
    if (observed.state != NsState::Creating || !observed.creator)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CasRefCatalog::completeCreation: namespace '{}' is not a Creating entry with a creator "
            "fence -- steps 2/3 only ever run over one of those", observed.ns.string());

    /// Step 2 (spec §3): INV-4's first `_ckpt` writer for this incarnation, and the only writer that
    /// will ever know its genesis epoch -- see `Pool/CasRefCkpt.h`'s `publishCkpt` doc for the merge
    /// discipline this rides on unchanged. `FencedOut` here ends the attempt: nothing durable changed.
    const RefCkpt contribution{.life_epoch = std::optional<uint64_t>{observed.creator->writer_epoch},
                                .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt};
    if (publishCkpt(backend, layout, NamespaceLifeId::fromCatalogEntry(observed.ns, observed.incarnation),
                     contribution, admitted_generation, check_fence_or_throw,
                     deadline) == CkptPublishOutcome::FencedOut)
        return NamespaceCreationOutcome::FencedOut;

    /// Step 3. `mutate` is the fence re-check point `casUpdate`'s header doc names -- checked FIRST,
    /// mirroring `publishCkpt`'s own "after the read, before the CAS, on every attempt" placement, so a
    /// caller stale on BOTH axes between step 2 and here is reported `FencedOut`, never `Superseded`
    /// (both are truthful refusals of a CAS that was never sent; this is only which one speaks first).
    const auto mutate = [&](const RefCatalog & cur) -> RefCatalog
    {
        try { check_fence_or_throw(admitted_generation); }
        catch (...) { throw CatalogFenceMovedMarker{}; }   /// typed, not propagated -- publishCkpt's own precedent

        const auto it = findEntry(cur, observed.ns);
        if (it == cur.entries.end() || *it != observed)
            throw CatalogEntryMismatchMarker{};

        RefCatalog next = cur;
        next.entries[static_cast<size_t>(it - cur.entries.begin())].state = NsState::Live;
        next.entries[static_cast<size_t>(it - cur.entries.begin())].creator = std::nullopt;
        return next;
    };

    try
    {
        casUpdate(backend, layout, mutate);
    }
    catch (const CatalogFenceMovedMarker &) { return NamespaceCreationOutcome::FencedOut; }
    catch (const CatalogEntryMismatchMarker &) { return NamespaceCreationOutcome::Superseded; }
    return NamespaceCreationOutcome::Live;
}

CasRefCatalog::NamespaceCreationOutcome CasRefCatalog::createNamespace(
    Backend & backend, const Layout & layout, const RootNamespace & ns, const CreatorFence & creator,
    uint64_t admitted_generation, const std::function<void(uint64_t)> & check_fence_or_throw,
    const CkptDeadline & deadline)
{
    /// Read-first, per the Task 2 review's own note on `casAdmitEntry`: a namespace that already
    /// carries an entry is THIS function's job to reject with a clear message, not `casAdmitEntry`'s
    /// duplicate-namespace grammar refusal (which would report a `LOGICAL_ERROR` about canonical order
    /// -- true, but useless to a caller trying to understand why its create failed). A concurrent
    /// insert of the SAME namespace between this read and step 1 is still caught -- `casAdmitEntry`'s
    /// own grammar check is the backstop, not the only check.
    const Snapshot snap = read(backend, layout);
    const auto existing = findEntry(snap.catalog, ns);
    if (existing != snap.catalog.entries.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CasRefCatalog::createNamespace: namespace '{}' already carries a catalog entry (state "
            "'{}') -- a stalled Creating entry is resumed through reconcileStaleCreator + "
            "completeCreation, never a fresh createNamespace call, and recreating an existing "
            "Live/Removing name is Task 5's (removal's) business, not creation's",
            ns.string(), nsStateToWord(existing->state));

    const CatalogEntry entry{.ns = ns, .state = NsState::Creating,
                              .incarnation = mintFreshIncarnation(), .creator = creator};
    casAdmitEntry(backend, layout, entry);   /// step 1
    return completeCreation(backend, layout, entry, admitted_generation, check_fence_or_throw, deadline);
}

CasRefCatalog::ReconcileCreatorOutcome CasRefCatalog::reconcileStaleCreator(
    Backend & backend, const Layout & layout, const CatalogEntry & observed, const CreatorFence & new_creator,
    const std::function<bool(const CreatorFence &)> & is_creator_fence_terminal,
    uint64_t admitted_generation, const std::function<void(uint64_t)> & check_fence_or_throw)
{
    if (observed.state != NsState::Creating || !observed.creator)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CasRefCatalog::reconcileStaleCreator: namespace '{}' is not a Creating entry with a "
            "creator fence -- nothing to reconcile", observed.ns.string());

    /// Review I6: the fence re-check is checked FIRST, on every fresh read this CAS retries -- the same
    /// placement `completeCreation` uses for exactly the same reason (see that function's own doc).
    /// Token-exactness (the catalog's own entry, by full value) comes next: it is the cheaper, purely
    /// local comparison, and a mismatch here means the question "is the OLD creator's fence terminal" is
    /// moot -- `observed` no longer describes anything live to reconcile.
    const auto mutate = [&](const RefCatalog & cur) -> RefCatalog
    {
        try { check_fence_or_throw(admitted_generation); }
        catch (...) { throw CatalogFenceMovedMarker{}; }   /// typed, not propagated -- completeCreation's own precedent

        const auto it = findEntry(cur, observed.ns);
        if (it == cur.entries.end() || *it != observed)
            throw CatalogEntryMismatchMarker{};
        if (!is_creator_fence_terminal(*observed.creator))
            throw CatalogCreatorStillLiveMarker{};

        RefCatalog next = cur;
        next.entries[static_cast<size_t>(it - cur.entries.begin())].creator = new_creator;
        return next;
    };

    try
    {
        casUpdate(backend, layout, mutate);
    }
    catch (const CatalogFenceMovedMarker &) { return ReconcileCreatorOutcome::FencedOut; }
    catch (const CatalogEntryMismatchMarker &) { return ReconcileCreatorOutcome::EntryChanged; }
    catch (const CatalogCreatorStillLiveMarker &) { return ReconcileCreatorOutcome::CreatorFenceStillLive; }
    return ReconcileCreatorOutcome::Reconciled;
}

void CasRefCatalog::checkPublicationAdmittedOrThrow(const RefCatalog & catalog, const RootNamespace & ns)
{
    const auto it = findEntry(catalog, ns);
    if (it != catalog.entries.end() && it->state == NsState::Creating)
        throwCasWriteRetryLater(fmt::format(
            "CAS ref catalog: namespace '{}' is still Creating -- no ref writes are admitted until "
            "its creation completes or is reconciled away", ns.string()));
}

}
