#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackendListing.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <set>
#include <tuple>

namespace DB::Cas
{

namespace
{

/// Delete every object listed under `prefix` by its listed (or, absent a list-token backend, HEAD'd)
/// token. This backs the staging and roots drain phases below: the victim's writers are fenced by the
/// decommission claim (`Store::openForDecommission`), so nothing should be racing these deletes, and a
/// plain exact-token delete of every listed object is race-free.
///
/// Fail-close (spec §core "Fail-close", plan Global Constraints): a per-object failure -- a thrown
/// exception (a transient backend hiccup) or a `TokenMismatch`/`NotFound`/vanished-before-HEAD delete
/// outcome -- is recorded as a WARNING and the sweep continues to the next object. This is the ONLY
/// tolerated-and-continue class in the whole feature; the caller keeps the pool slot on any non-empty
/// `DecommissionReport::warnings` (Task 4) so a re-run drains whatever this pass left behind, rather
/// than reporting a clean drain that never happened. Returns the count actually deleted.
uint64_t deleteListedPrefix(Backend & backend, const String & prefix, std::vector<String> & warnings)
{
    uint64_t deleted = 0;
    forEachListedKey(backend, prefix, [&](const ListedKey & listed)
    {
        try
        {
            Token token;
            if (listed.token)
                token = *listed.token;
            else
            {
                const HeadResult head = backend.head(listed.key);
                if (!head.exists)
                {
                    warnings.push_back("decommission drain: " + listed.key + " vanished before delete");
                    return;
                }
                token = head.token;
            }

            const DeleteOutcome outcome = backend.deleteExact(listed.key, token);
            const DeleteClass outcome_class = classifyDeleteOutcome(outcome);
            if (outcome_class == DeleteClass::Deleted)
                ++deleted;
            else
                warnings.push_back("decommission drain: " + listed.key + " delete outcome "
                                    + String(deleteClassName(outcome_class)));
        }
        catch (...)
        {
            warnings.push_back("decommission drain: " + listed.key + " delete failed: "
                                + getCurrentExceptionMessage(/*with_stacktrace=*/false));
        }
    });
    return deleted;
}

}

DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                          const String & victim_srid, const CasEventSink & sink)
{
    DecommissionReport report;
    report.srid = victim_srid;

    StorePtr admin = Store::openForDecommission(std::move(backend), std::move(config), victim_srid);
    if (sink)
        admin->setEventSink(sink);

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "begin";
        e.reason = "operator decommission of pool member";
        e.detail = {{"srid", victim_srid}};
    });

    /// Phase: namespace erasure. `listNamespaces` unions `cas/refs/` and `roots/`; only entries with a
    /// present ref-object prefix are droppable tables -- a roots-only entry is mountpoint debris handled
    /// by the roots sweep (Task 3).
    for (const String & ns_str : admin->listNamespaces(victim_srid))
    {
        const RootNamespace ns(ns_str);
        if (admin->namespaceIsRemoved(ns))
        {
            ++report.namespaces_already_removed;
            continue;
        }
        const auto ref_objects = admin->backend().list(admin->layout().refsNamespacePrefix(ns), /*cursor=*/"", /*limit=*/1);
        if (ref_objects.keys.empty())
            continue;   /// not a table (roots-only listing entry)

        const auto stats = admin->dropNamespace(ns);
        ++report.namespaces_removed;
        report.committed_refs_removed += stats.committed_refs;
        report.precommits_removed += stats.precommits;
        report.edge_deltas_emitted += stats.committed_refs + stats.precommits;

        EventEmitter{*admin}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::MemberDecommission;
            e.outcome = "namespace_removed";
            e.reason = "decommission dropped a victim namespace";
            e.detail = {{"srid", victim_srid}, {"namespace", ns_str},
                        {"committed", std::to_string(stats.committed_refs)},
                        {"precommits", std::to_string(stats.precommits)}};
        });
    }

    /// Phase: manifest-debris drain. MUST precede any future slot deletion (Task 4): deleting the mount
    /// body would destroy the watermark authority (`floorForNamespace` -> nullopt -> "not eligible") and
    /// strand this debris forever (spec §core step 4). We are the epoch authority here -- every
    /// old-epoch build prefix is eligible (`prefix.writer_epoch < w.writer_epoch`,
    /// `CasOrphanManifestSweep.cpp`), so a scoped sweep over the victim's `cas/manifests/` build
    /// prefixes removes the rest by exact token.
    {
        const String debris_prefix = admin->layout().casManifestsPrefix() + victim_srid;
        std::set<std::tuple<String, uint64_t, uint64_t>> groups;   /// (namespace, writer_epoch, build_sequence)
        forEachListedKey(admin->backend(), debris_prefix, [&](const ListedKey & listed)
        {
            if (const auto parsed = admin->layout().parseManifestKey(listed.key))
                groups.emplace(parsed->root_namespace.string(), parsed->ref.writer_epoch, parsed->ref.build_sequence);
        });
        for (const auto & [ns_str, writer_epoch, build_sequence] : groups)
            report.manifest_debris_removed += sweepNamespace(
                *admin, RootNamespace(ns_str), BuildPrefix{writer_epoch, build_sequence}, &report.warnings);
    }

    /// Phase: staging sweep. The victim's own `<pool_prefix>/staging/<srid>/` area -- the same prefix
    /// `sweepOwnMountStaging` (`CasStagingSweeper.h`) owns for a live mount's own leaked debris. That
    /// helper takes an `IObjectStorage`, unavailable at this `Backend`-only layer, so this drains the
    /// same prefix directly.
    report.staging_objects_removed += deleteListedPrefix(
        admin->backend(), admin->poolConfig().pool_prefix + "/staging/" + victim_srid + "/", report.warnings);

    /// Phase: roots sweep. The victim's mountpoint objects (`Layout::serverRootDataPrefix`,
    /// `mountpointObjectKey`, `CasLayout.h`) -- loose, non-content-addressed files with no epoch of
    /// their own, but the victim's writers are fenced by the claim, so no write can race the sweep.
    report.mountpoint_objects_removed += deleteListedPrefix(
        admin->backend(), admin->layout().serverRootDataPrefix(victim_srid), report.warnings);

    /// Phase: slot retirement -- STRICTLY LAST, and only over a clean drain (fail-close: an unconfirmed
    /// drain keeps the resume anchor; the operator re-runs). `layout`/`pool_backend` are copied out
    /// BEFORE `admin.reset()` below -- `admin` (and the `Store` it owns) is gone the instant the
    /// graceful close runs, so nothing after that point may dereference it. `Layout` is a cheap value
    /// type (one `String`); `poolBackendPtr()` shares ownership of the backend so it outlives the Store.
    const Layout layout = admin->layout();
    const BackendPtr pool_backend = admin->poolBackendPtr();
    if (report.warnings.empty())
    {
        /// Graceful close of the admin store: `Store::~Store`'s mount-lease keeper stamps the lease
        /// already-expired and folds in the watermark farewell (`min_active = UINT64_MAX`) -- the
        /// `terminated` state (`CasServerRoot.cpp`).
        admin.reset();

        /// Delete the control objects; the mount body LAST -- it is the claim/resume anchor, so a crash
        /// between deletes must never leave an owner/epoch-less but still-mounted slot, only a
        /// still-claimable one. A per-key failure here is itself a drain-incomplete signal: record it
        /// and stop (the remaining keys, including the mount, survive so a re-run can retry).
        const std::vector<String> slot_keys = {layout.epochKey(victim_srid), layout.ownerKey(victim_srid),
                                                layout.mountKey(victim_srid)};
        bool all_deleted = true;
        for (const String & key : slot_keys)
        {
            if (const auto got = pool_backend->get(key))
            {
                try
                {
                    pool_backend->deleteExact(key, got->token);
                }
                catch (...)
                {
                    report.warnings.push_back("slot delete failed: " + key + ": "
                                              + getCurrentExceptionMessage(/*with_stacktrace=*/false));
                    all_deleted = false;
                    break;   /// keep the remaining keys — mount survives ⇒ resume works
                }
            }
        }
        report.slot_removed = all_deleted;
    }
    else
    {
        report.slot_removed = false;
        LOG_WARNING(getLogger("CasDecommission"),
            "CAS decommission '{}': drain incomplete ({} warnings) — mount slot kept (terminated); "
            "re-run the command to finish", victim_srid, report.warnings.size());
        admin.reset();   /// graceful close still stamps the farewell — the slot reads `terminated`
    }

    /// The `end` event is emitted via `sink` directly, not `EventEmitter{*admin}`: `admin` is gone by
    /// now. This also means its `warnings` count reflects the FINAL total, including a slot-delete
    /// failure appended just above -- `EventEmitter`'s own zero-cost-when-absent guard is reproduced by
    /// the `if (sink)` below.
    if (sink)
    {
        CasEvent e;
        e.type = CasEventType::MemberDecommission;
        e.outcome = "end";
        e.reason = "decommission finished";
        e.detail = {{"srid", victim_srid},
                    {"namespaces_removed", std::to_string(report.namespaces_removed)},
                    {"warnings", std::to_string(report.warnings.size())},
                    {"slot_removed", report.slot_removed ? "1" : "0"}};
        sink(std::move(e));
    }
    return report;
}

}
