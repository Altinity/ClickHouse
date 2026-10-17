#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <base/defines.h>
#include <base/scope_guard.h>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <set>
#include <tuple>

namespace DB
{
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string_view removalName(Removal r)
{
    switch (r)
    {
        case Removal::Removed:  return "removed";
        case Removal::Gone:     return "gone";
        case Removal::Mismatch: return "mismatch";
    }
    UNREACHABLE();
}

/// Delete every object listed under `prefix` by its listed (or, absent a list-incarnation backend,
/// HEAD'd) incarnation. This backs the staging and roots drain phases below: the victim's writers are
/// fenced by the decommission claim (`Pool::openForDecommission`), so nothing should be racing these
/// deletes, and a plain exact-incarnation delete of every listed object is race-free.
///
/// Runs on the admin's mount plane, so a fenced admin's deletes are refused locally; only attempts already
/// sent can land late.
///
/// A per-object failure — a backend exception, a `Mismatch` or `Gone` outcome, or an object
/// disappearing between `LIST` and `HEAD` — is recorded as a warning and does not prevent the remaining
/// objects from being attempted while `op` is still admitted. Returns only the objects whose
/// exact-incarnation delete was reported as `Removed`.
uint64_t deleteListedPrefix(CasOperation & op, const String & prefix, std::vector<String> & warnings)
{
    uint64_t deleted = 0;
    op.forEachListedKey(prefix, [&](const ListedKey & listed)
    {
        try
        {
            std::optional<Etag> etag = listed.etag;
            if (!etag)
            {
                const std::optional<Meta> head = op.head(listed.key, Retry::standard());
                if (!head)
                {
                    warnings.push_back("decommission drain: " + listed.key + " vanished before delete");
                    return true;
                }
                etag = head->etag;
            }

            const Removal outcome = op.remove(listed.key, *etag, Retry::standard());
            if (outcome == Removal::Removed)
                ++deleted;
            else
                warnings.push_back("decommission drain: " + listed.key + " delete outcome "
                                    + String(removalName(outcome)));
        }
        catch (...)
        {
            warnings.push_back("decommission drain: " + listed.key + " delete failed: "
                                + getCurrentExceptionMessage(/*with_stacktrace=*/false));
            return op.admitted();
        }
        return true;
    }, Retry::standard());
    return deleted;
}

String whyNotWritten(const WriteResult & result)
{
    return std::visit(detail::Overload{
        [](const Committed &) -> String { return "committed"; },
        [](const Declined &) -> String { return "declined"; },
        [](const Conflict &) -> String { return "the object changed since it was read"; },
        [](const Refused & refused) -> String { return "the store refused it: " + refused.message; },
        [](const GaveUp & gave_up) -> String
        {
            return gave_up.sent_any ? "gave up after sending, it may have landed" : "gave up before sending";
        }}, result);
}

/// A mark counts only on a retired owner: clearing `retired_at_ms` by hand to restart a member leaves the mark
/// of the earlier life behind.
bool holdsVerifiedRetirement(const OwnerObject & owner)
{
    return owner.retirement_verified && owner.retired_at_ms.has_value();
}

/// This run's tombstone: what a later exit needs to decide whether to keep it or roll it back.
struct Tombstone
{
    OwnerObject owner;                /// the tombstone's body
    bool previously_retired = false;  /// the owner was already retired; a rollback never undoes that
    Etag etag;                        /// the tombstone's incarnation, the rollback's precondition
};

void logWarnings(const String & srid, const std::vector<String> & warnings)
{
    for (const String & warning : warnings)
        LOG_WARNING(getLogger("CasDecommission"), "CAS decommission '{}': {}", srid, warning);
}

/// Undo this run's tombstone after a lost claim: the next life, not retired, no mark. An earlier retirement
/// stays: its tombstone landed while its run held the claim, so no member runs under it. A rollback whose
/// precondition moved was overtaken by a later owner write, which then owns the outcome.
void rollBackTombstone(CasOperation & op, const Layout & layout, const String & srid, const Tombstone & tombstone,
                       std::vector<String> & warnings)
{
    if (tombstone.previously_retired)
    {
        warnings.push_back("the admin claim was lost after the tombstone; the owner keeps its earlier retirement");
        return;
    }
    const OwnerObject restored = nextOwnerLife(tombstone.owner, std::nullopt, false);
    const WriteResult written = op.replace(layout.ownerKey(srid), encodeOwner(restored), tombstone.etag, Retry::standard());
    if (std::holds_alternative<Committed>(written))
        warnings.push_back("the admin claim was lost after the tombstone; the tombstone was rolled back (owner life "
                           + std::to_string(restored.owner_life) + "); rerun to decommission");
    else if (std::holds_alternative<Conflict>(written))
        warnings.push_back("the admin claim was lost after the tombstone and the rollback did not land: the owner "
                           "changed after this run's tombstone; read the owner object '" + layout.ownerKey(srid)
                           + "' before rerunning");
    else
        warnings.push_back("the admin claim was lost after the tombstone and the rollback did not complete ("
                           + whyNotWritten(written) + "); read the owner object '" + layout.ownerKey(srid)
                           + "' before rerunning");
}

/// Every exit after the tombstone and before the farewell: the tombstone stays while the claim holds. A failed
/// confirm is a warning, so an exit that already carries an error still propagates its own.
void confirmClaimOrRollBack(CasOperation & op, const Layout & layout, const String & srid, UInt128 victim_uuid,
                            uint64_t admin_epoch, const Tombstone & tombstone, std::vector<String> & warnings)
{
    try
    {
        if (!adminClaimStillHeld(op, layout, srid, victim_uuid, admin_epoch))
            rollBackTombstone(op, layout, srid, tombstone, warnings);
    }
    catch (...)
    {
        warnings.push_back("could not confirm the admin claim: " + getCurrentExceptionMessage(/*with_stacktrace=*/false)
                           + "; read the owner object '" + layout.ownerKey(srid) + "' before rerunning");
    }
}

/// Reads the owner on `control`, requires the victim's uuid, and replaces it on the admin's mount plane by the
/// next life, retired, keeping an earlier verified mark. The mount plane sends no attempt after the admin's
/// lease deadline, but the store may apply one later. A paused earlier run cannot roll this tombstone back:
/// its precondition is older. A write with an unknown outcome is still an exit after the tombstone when it
/// landed: if the owner reads back as exactly these bytes, the claim is confirmed (or the tombstone rolled
/// back) before the error propagates. Otherwise the admin withholds its clean farewell: the victim then runs
/// only after its slot is fenced or observed dead.
Tombstone writeTombstone(CasOperation & control, Pool & admin, const Layout & layout, const String & srid,
                         UInt128 victim_uuid, uint64_t admin_epoch, std::vector<String> & warnings)
{
    CasOperation fenced = admin.mountRequests().admit();
    const String key = layout.ownerKey(srid);
    const std::optional<Object> current = control.read(key, Retry::standard());
    if (!current)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS decommission '{}': the owner anchor vanished after the admin claim", srid);
    const OwnerObject read = decodeOwner(current->bytes);
    if (read.server_uuid != victim_uuid)
        throw Exception(ErrorCodes::ABORTED,
            "CAS decommission '{}': the owner is now server_uuid={}, the claim was made for {}; another server "
            "took this server_root_id since the claim, nothing was removed",
            srid, u128ToHex(read.server_uuid), u128ToHex(victim_uuid));

    const bool previously_retired = read.retired_at_ms.has_value();
    const OwnerObject owner = nextOwnerLife(read, nowMs(), holdsVerifiedRetirement(read));
    const String bytes = encodeOwner(owner);
    const String what = fmt::format("CAS decommission '{}' owner tombstone", srid);

    std::optional<WriteResult> written;
    std::exception_ptr unknown_outcome;
    try
    {
        written = fenced.replace(key, bytes, current->etag, Retry::standard());
    }
    catch (...)
    {
        /// The engine rethrows a local failure even after an attempt was sent.
        unknown_outcome = std::current_exception();
    }
    if (written)
    {
        if (const Committed * committed = std::get_if<Committed>(&*written))
            return Tombstone{.owner = owner, .previously_retired = previously_retired, .etag = committed->etag};
        if (std::holds_alternative<Conflict>(*written))
            throw Exception(ErrorCodes::ABORTED,
                "CAS decommission '{}': the owner was rewritten between the claim and the tombstone (a rerun or a "
                "rejoin); nothing was removed, rerun to decide against the current owner", srid);
        const GaveUp * gave_up = std::get_if<GaveUp>(&*written);
        if (!gave_up || !gave_up->sent_any)
        {
            orThrow(std::move(*written), what);
            throw Exception(ErrorCodes::LOGICAL_ERROR, "{}: a conditional replace was declined", what);
        }
        try
        {
            orThrow(std::move(*written), what);
        }
        catch (...)
        {
            unknown_outcome = std::current_exception();
        }
    }

    std::optional<Object> landed;
    try
    {
        landed = control.read(key, Retry::standard());
    }
    catch (...)
    {
        warnings.push_back("could not read the owner back after the tombstone write: "
                           + getCurrentExceptionMessage(/*with_stacktrace=*/false));
    }
    if (landed && landed->bytes == bytes)
    {
        const Tombstone tombstone{.owner = owner, .previously_retired = previously_retired, .etag = landed->etag};
        confirmClaimOrRollBack(control, layout, srid, victim_uuid, admin_epoch, tombstone, warnings);
    }
    else
        admin.withholdFarewell();
    logWarnings(srid, warnings);
    std::rethrow_exception(unknown_outcome);
}

/// Whether the owner carries a verified retirement once this run can no longer act. Otherwise says why not:
/// an unretired owner past this run's tombstone moved on (a rejoin, or another run's rollback), and a rerun
/// there could drain a rejoined member; anything else, a later run's unverified tombstone included, means
/// rerun.
bool ownerHoldsVerifiedRetirement(CasOperation & op, const Layout & layout, const String & srid, const Tombstone & tombstone,
                                  std::vector<String> & warnings)
{
    try
    {
        if (const std::optional<Object> current = op.read(layout.ownerKey(srid), Retry::standard()))
        {
            const OwnerObject owner = decodeOwner(current->bytes);
            if (holdsVerifiedRetirement(owner))
                return true;
            if (!owner.retired_at_ms && owner.owner_life > tombstone.owner.owner_life)
            {
                warnings.push_back(fmt::format(
                    "the owner moved on to life {} after this run's tombstone (a rejoin or another run's rollback); "
                    "read system.cas_mounts and the owner object '{}' before rerunning",
                    owner.owner_life, layout.ownerKey(srid)));
                return false;
            }
        }
    }
    catch (...)
    {
        warnings.push_back("could not re-read the owner: " + getCurrentExceptionMessage(/*with_stacktrace=*/false));
    }
    warnings.push_back("the retirement is not verified; rerun SYSTEM CAS DROP POOL MEMBER");
    return false;
}

/// The mark records that this run's claim, tombstone and farewell all held; it does not wait for GC.
bool verifyAndMark(CasOperation & op, const Layout & layout, const String & srid, UInt128 victim_uuid,
                   uint64_t admin_epoch, const Tombstone & tombstone, std::vector<String> & warnings)
{
    const size_t warnings_before = warnings.size();
    const String owner_key = layout.ownerKey(srid);
    try
    {
        const std::optional<Object> slot = op.read(layout.mountKey(srid), Retry::standard());
        const std::optional<Object> current = op.read(owner_key, Retry::standard());
        if (slot && current)
        {
            const MountLease body = decodeMountLease(slot->bytes);
            const OwnerObject owner = decodeOwner(current->bytes);
            const bool farewell_held = body.isHeldBy(victim_uuid, admin_epoch) && !body.gc_fenced && body.isFarewell();
            const bool tombstone_held = owner.server_uuid == victim_uuid && owner.retired_at_ms
                && owner.owner_life == tombstone.owner.owner_life;
            if (farewell_held && tombstone_held)
            {
                const OwnerObject marked_owner = nextOwnerLife(owner, owner.retired_at_ms, true);
                const WriteResult marked = op.replace(owner_key, encodeOwner(marked_owner), current->etag, Retry::standard());
                if (std::holds_alternative<Committed>(marked))
                    return true;
                warnings.push_back("the verified mark was not written (" + whyNotWritten(marked) + ")");
            }
            else
                warnings.push_back(farewell_held ? "the owner is not this run's tombstone" : "the slot does not hold this run's farewell");
        }
    }
    catch (...)
    {
        warnings.push_back("verification failed: " + getCurrentExceptionMessage(/*with_stacktrace=*/false));
    }
    if (!ownerHoldsVerifiedRetirement(op, layout, srid, tombstone, warnings))
        return false;
    /// The owner re-read found the mark: the failure above, e.g. an unclear mark write, needs no rerun.
    for (size_t i = warnings_before; i < warnings.size(); ++i)
        LOG_INFO(getLogger("CasDecommission"), "CAS decommission '{}': settled by the owner re-read: {}", srid, warnings[i]);
    warnings.resize(warnings_before);
    return true;
}

/// The drain on the admin's fenced mount plane: victim namespaces, manifest debris, staging, roots. Per-object
/// failures land in `report.warnings`; a namespace that changed or lost its checkpoint throws.
void drainVictim(Pool & admin, const CasRefCatalog::Snapshot & catalog_cut, const String & victim_srid,
                 DecommissionReport & report, bool & gc_round_needed)
{
    CasOperation op = admin.mountRequests().admit();

    /// The pre-impersonation catalog cut is the complete ownership universe. Physical life keys carry
    /// no logical path, and raw string prefixes such as `victim` must not select the distinct owner
    /// `victim2`; the slash makes `victim` one canonical path component.
    const String victim_namespace_prefix = victim_srid + "/";
    std::vector<std::pair<CatalogEntry, NamespaceLifeId>> owned_lives;
    for (const CatalogEntry & entry : catalog_cut.catalog.entries)
    {
        if (entry.ns.string() != victim_srid && !entry.ns.string().starts_with(victim_namespace_prefix))
            continue;
        const auto life = catalog_cut.life_index.resolve(entry.incarnation);
        if (!life)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "ca-decommission: catalog entry '{}' has no physical life resolution", entry.ns.string());
        owned_lives.emplace_back(entry, *life);
    }

    for (const auto & [selected_entry, life] : owned_lives)
    {
        const RootNamespace & ns = life.ns;
        const String & ns_str = ns.string();

        /// Refuse a same-name lifecycle move that landed after the immutable selection cut. The
        /// exact-life overloads below also pin recovery to `life`, closing the race after this check:
        /// a later replacement can never redirect a removal to its new incarnation.
        const CasRefCatalog::Snapshot current_catalog = CasRefCatalog::read(op, admin.layout());
        const auto current_entry = std::find_if(
            current_catalog.catalog.entries.begin(), current_catalog.catalog.entries.end(),
            [&](const CatalogEntry & entry) { return entry.ns.string() == ns_str; });
        if (current_entry == current_catalog.catalog.entries.end() || *current_entry != selected_entry)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "ca-decommission: namespace '{}' changed incarnation after the validated catalog cut; "
                "refusing destructive work",
                ns_str);

        if (selected_entry.state == NsState::Removing)
        {
            if (!op.head(admin.layout().refCkptKey(life), Retry::standard()).has_value())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "ca-decommission: namespace '{}' is Removing but its exact checkpoint is absent; "
                    "the catalog row remains owned and the victim slot cannot be retired",
                    ns_str);

            /// `dropNamespace` is the sole terminal writer. On an already-complete removal this is an
            /// idempotent observation; on a pre-terminal `Removing` life it resumes the exact terminal
            /// append under the administrative writer fence. Catalog deletion remains GC's job.
            (void)admin.dropNamespace(life);
            ++report.namespaces_already_removed;
            gc_round_needed = true;
            continue;
        }

        const auto stats = admin.dropNamespace(life);
        ++report.namespaces_removed;
        report.committed_refs_removed += stats.committed_refs;
        report.precommits_removed += stats.precommits;
        report.edge_deltas_emitted += stats.committed_refs + stats.precommits;
        if (selected_entry.state != NsState::Creating)
            gc_round_needed = true;

        EventEmitter{admin}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::MemberDecommission;
            e.outcome = "namespace_removed";
            e.reason = "decommission dropped a victim namespace";
            e.detail = {{"server_root_id", victim_srid}, {"namespace", ns_str},
                        {"committed", std::to_string(stats.committed_refs)},
                        {"precommits", std::to_string(stats.precommits)}};
        });
    }

    /// The decommission claim has advanced the writer epoch, so every build prefix with
    /// `prefix.writer_epoch < w.writer_epoch` is eligible here. Group the listed keys by namespace and
    /// build prefix so each group can use the exact-token orphan sweep.
    {
        const String debris_prefix = admin.layout().casManifestsServerPrefix(victim_srid);
        std::set<std::tuple<String, uint64_t, uint64_t>> groups;   /// (namespace, writer epoch, build sequence)
        op.forEachListedKey(debris_prefix, [&](const ListedKey & listed)
        {
            if (const auto parsed = admin.layout().parseManifestKey(listed.key))
                groups.emplace(parsed->root_namespace.string(), parsed->ref.writer_epoch, parsed->ref.build_sequence);
            return true;
        }, Retry::standard());
        for (const auto & [ns_str, writer_epoch, build_sequence] : groups)
            report.manifest_debris_removed += sweepNamespace(
                admin, RootNamespace(ns_str), BuildPrefix{writer_epoch, build_sequence}, &report.warnings);
    }

    /// Drain the victim's own `<pool_prefix>/staging/<srid>/` area. The live-mount staging helper uses
    /// an `IObjectStorage`, while this command intentionally works at the `Backend` layer, so the same
    /// prefix is listed and deleted directly. The claim fences the victim's writers during this sweep.
    report.staging_objects_removed += deleteListedPrefix(
        op, admin.poolConfig().pool_prefix + "/staging/" + victim_srid + "/", report.warnings);

    /// Drain the victim's mountpoint objects. These are loose, non-content-addressed files under
    /// `Layout::serverRootDataPrefix`; they have no writer epoch of their own, so the claim is what
    /// prevents a returning victim from racing this deletion.
    report.mountpoint_objects_removed += deleteListedPrefix(
        op, admin.layout().serverRootDataPrefix(victim_srid), report.warnings);

    if (!op.admitted())
    {
        report.warnings.push_back("the admin lost its mount lease during the drain; rerun");
        return;
    }

    /// Rows still owned by the victim after the drain. `Removing` rows are GC's: they count as pending, not
    /// as a failure. Anything else appeared after the ownership cut and was not drained: rerun.
    const CasRefCatalog::Snapshot after_drain = CasRefCatalog::read(op, admin.layout());
    for (const CatalogEntry & entry : after_drain.catalog.entries)
    {
        if (entry.ns.string() != victim_srid && !entry.ns.string().starts_with(victim_namespace_prefix))
            continue;
        if (entry.state == NsState::Removing)
            ++report.namespaces_pending_gc;
        else
            report.warnings.push_back("namespace '" + entry.ns.string() + "' appeared after the ownership cut and was not drained; rerun");
    }
}

}

bool adminClaimStillHeld(CasOperation & op, const Layout & layout, const String & srid, UInt128 victim_uuid, uint64_t admin_epoch)
{
    const std::optional<Object> slot = op.read(layout.mountKey(srid), Retry::standard());
    if (!slot)
        return false;
    const MountLease body = decodeMountLease(slot->bytes);
    return body.isHeldBy(victim_uuid, admin_epoch) && !body.gc_fenced && !body.isFarewell();
}

DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                          const String & victim_srid, const CasEventSink & sink,
                                          const std::function<void()> & request_gc_round,
                                          const std::function<uint64_t()> & drain_now_fn,
                                          const std::function<void(uint64_t)> & drain_sleep_fn)
{
    DecommissionReport report;
    report.srid = victim_srid;
    bool gc_round_needed = false;
    /// A namespace may have reached `Removing` before a later namespace fails closed: wake GC on every exit.
    SCOPE_EXIT({
        if (gc_round_needed && request_gc_round)
            request_gc_round();
    });

    /// Validate one immutable ownership cut before impersonating the victim, on the raw backend: no `Pool`
    /// exists yet. Namespace selection reuses this exact pre-mutation decision rather than a later read.
    CasRequests preflight_requests(backend, Fence::open());
    CasOperation preflight_op = preflight_requests.admit();
    const Layout catalog_layout(config.pool_prefix);
    const CasRefCatalog::Snapshot catalog_cut = CasRefCatalog::read(preflight_op, catalog_layout);
    catalog_cut.life_index.throwIfAmbiguous("CAS decommission");

    /// One fake clock and sleep for every plane: the farewell deadline is bound to the boot clock, and a
    /// plane on a frozen clock that sleeps for real would never see its retry deadline elapse.
    if (drain_now_fn && drain_sleep_fn)
    {
        if (!config.boot_ms_fn)
            config.boot_ms_fn = drain_now_fn;
        if (!config.retry_sleep_fn)
            config.retry_sleep_fn = drain_sleep_fn;
    }
    config.event_sink = sink;

    /// The admin claim. `NoWait` refuses a live victim and accepts a tombstoned owner.
    PoolPtr admin = Pool::openForDecommission(std::move(backend), std::move(config), victim_srid);
    if (drain_now_fn && drain_sleep_fn)
    {
        admin->setCasRequestNowFnForTest(drain_now_fn);
        admin->setCasRetrySleepForTest(drain_sleep_fn);
    }
    const UInt128 victim_uuid = admin->poolConfig().server_id;
    const uint64_t admin_epoch = admin->writerEpoch();
    const Layout layout = admin->layout();

    /// The control plane: always admitted, so a fenced admin can still confirm, roll back and verify. It keeps
    /// its own shared_ptr to the backend and outlives `admin.reset()`.
    CasRequests control_requests(admin->poolBackendPtr(), Fence::open());
    if (drain_now_fn && drain_sleep_fn)
    {
        control_requests.setNowFnForTest(drain_now_fn);
        control_requests.setSleepFnForTest(drain_sleep_fn);
    }
    CasOperation control = control_requests.admit();

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "begin";
        e.reason = "operator decommission of pool member";
        e.detail = {{"server_root_id", victim_srid}, {"server_uuid", u128ToHex(victim_uuid)}};
    });

    /// From here on every exit decides the tombstone's fate (`confirmClaimOrRollBack`).
    const Tombstone tombstone = writeTombstone(control, *admin, layout, victim_srid, victim_uuid, admin_epoch, report.warnings);

    try
    {
        /// The claim must have held through the tombstone write.
        if (!adminClaimStillHeld(control, layout, victim_srid, victim_uuid, admin_epoch))
        {
            rollBackTombstone(control, layout, victim_srid, tombstone, report.warnings);
        }
        else
        {
            drainVictim(*admin, catalog_cut, victim_srid, report, gc_round_needed);
            if (!report.warnings.empty())
                confirmClaimOrRollBack(control, layout, victim_srid, victim_uuid, admin_epoch, tombstone, report.warnings);
        }
    }
    catch (...)
    {
        /// The farewell `admin`'s destructor writes on unwind is a clean body, which the confirm would read as
        /// a lost claim, so the decision is made here, while the admin is alive.
        confirmClaimOrRollBack(control, layout, victim_srid, victim_uuid, admin_epoch, tombstone, report.warnings);
        logWarnings(victim_srid, report.warnings);
        throw;
    }

    /// The farewell: it lands while the claim holds and loses to the fence after a lost claim.
    /// `MountLeaseRenewer::terminate` reports no outcome, so `verifyAndMark` reads it back.
    admin.reset();
    if (report.warnings.empty())
    {
        report.member_retired = verifyAndMark(control, layout, victim_srid, victim_uuid, admin_epoch, tombstone, report.warnings);
        if (report.member_retired)
            LOG_INFO(getLogger("CasDecommission"), "CAS decommission '{}': retired server_uuid={} at owner life {}",
                     victim_srid, u128ToHex(victim_uuid), tombstone.owner.owner_life);
    }
    logWarnings(victim_srid, report.warnings);

    /// Emitted through `sink` directly: `admin` is gone by now.
    if (sink)
    {
        CasEvent e;
        e.type = CasEventType::MemberDecommission;
        e.outcome = "end";
        e.reason = "decommission finished";
        e.detail = {{"server_root_id", victim_srid},
                    {"server_uuid", u128ToHex(victim_uuid)},
                    {"owner_life", std::to_string(tombstone.owner.owner_life)},
                    {"namespaces_removed", std::to_string(report.namespaces_removed)},
                    {"namespaces_pending_gc", std::to_string(report.namespaces_pending_gc)},
                    {"warnings", std::to_string(report.warnings.size())},
                    {"member_retired", report.member_retired ? "1" : "0"}};
        sink(std::move(e));
    }
    return report;
}

}
