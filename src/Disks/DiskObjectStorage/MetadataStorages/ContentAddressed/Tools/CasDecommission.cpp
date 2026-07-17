#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
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
/// decommission claim (`Pool::openForDecommission`), so nothing should be racing these deletes, and a
/// plain exact-token delete of every listed object is race-free.
///
/// A per-object failure — a backend exception, a `TokenMismatch` or `NotFound` outcome, or an object
/// disappearing between `LIST` and `HEAD` — is recorded as a warning and does not prevent the remaining
/// objects from being attempted. The caller keeps the pool slot whenever warnings are present, so the
/// terminated slot remains available as a resume anchor instead of being deleted after an unconfirmed
/// drain. Returns only the objects whose exact-token delete was reported as `Deleted`.
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

    PoolPtr admin = Pool::openForDecommission(std::move(backend), std::move(config), victim_srid);
    if (sink)
        admin->setEventSink(sink);

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "begin";
        e.reason = "operator decommission of pool member";
        e.detail = {{"srid", victim_srid}};
    });

    /// `listNamespaces` includes names discovered under both `cas/refs/` and `roots/`. A name is a
    /// droppable table only when its refs prefix is present; roots-only names are loose mountpoint debris
    /// and must be left for the roots sweep below.
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
            continue;   /// A roots-only listing entry is not a table namespace.

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

    /// Manifest debris must be removed before the mount slot: deleting the mount body removes the
    /// watermark authority, after which `floorForNamespace` returns no value and the ordinary orphan
    /// sweep cannot prove that old-epoch debris is eligible. The decommission claim has advanced the
    /// writer epoch, so every build prefix with `prefix.writer_epoch < w.writer_epoch` is eligible here.
    /// Group the listed keys by namespace and build prefix so each group can use the exact-token orphan
    /// sweep while the mount body still supplies its authority.
    {
        const String debris_prefix = admin->layout().casManifestsPrefix() + victim_srid;
        std::set<std::tuple<String, uint64_t, uint64_t>> groups;   /// (namespace, writer epoch, build sequence)
        forEachListedKey(admin->backend(), debris_prefix, [&](const ListedKey & listed)
        {
            if (const auto parsed = admin->layout().parseManifestKey(listed.key))
                groups.emplace(parsed->root_namespace.string(), parsed->ref.writer_epoch, parsed->ref.build_sequence);
        });
        for (const auto & [ns_str, writer_epoch, build_sequence] : groups)
            report.manifest_debris_removed += sweepNamespace(
                *admin, RootNamespace(ns_str), BuildPrefix{writer_epoch, build_sequence}, &report.warnings);
    }

    /// Drain the victim's own `<pool_prefix>/staging/<srid>/` area. The live-mount staging helper uses
    /// an `IObjectStorage`, while this command intentionally works at the `Backend` layer, so the same
    /// prefix is listed and deleted directly. The claim fences the victim's writers during this sweep.
    report.staging_objects_removed += deleteListedPrefix(
        admin->backend(), admin->poolConfig().pool_prefix + "/staging/" + victim_srid + "/", report.warnings);

    /// Drain the victim's mountpoint objects. These are loose, non-content-addressed files under
    /// `Layout::serverRootDataPrefix`; they have no writer epoch of their own, so the claim is what
    /// prevents a returning victim from racing this deletion.
    report.mountpoint_objects_removed += deleteListedPrefix(
        admin->backend(), admin->layout().serverRootDataPrefix(victim_srid), report.warnings);

    /// Retire the slot strictly last and only after a clean drain. Copy the layout and shared backend
    /// before `admin.reset()`: graceful close destroys the `Pool`, while the backend must remain alive to
    /// delete the slot objects afterwards.
    const Layout layout = admin->layout();
    const BackendPtr pool_backend = admin->poolBackendPtr();
    if (report.warnings.empty())
    {
        /// Graceful close stamps an already-expired lease and the watermark farewell
        /// (`min_active = UINT64_MAX`), making the slot `terminated` before its control objects are
        /// removed.
        admin.reset();

        /// Delete epoch and owner before the mount body. The mount body is the claim/resume anchor: a
        /// failure between deletes must leave a still-claimable slot, never an owner/epoch-less mounted
        /// slot. Stop after the first failure so the remaining keys, including the mount, survive retry.
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
                    break;   /// Keep the remaining keys, including the mount, so a rerun can resume.
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
        admin.reset();   /// Graceful close still stamps the farewell, leaving the slot `terminated`.
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
