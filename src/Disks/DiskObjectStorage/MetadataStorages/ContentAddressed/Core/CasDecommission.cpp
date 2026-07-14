#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Common/logger_useful.h>

namespace DB::Cas
{

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

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "end";
        e.reason = "decommission finished";
        e.detail = {{"srid", victim_srid},
                    {"namespaces_removed", std::to_string(report.namespaces_removed)},
                    {"warnings", std::to_string(report.warnings.size())}};
    });
    return report;
}

}
