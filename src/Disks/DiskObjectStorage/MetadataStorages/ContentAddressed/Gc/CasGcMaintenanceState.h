#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcMaintenanceStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <optional>

namespace DB::Cas
{

enum class GcMaintenanceReadStatus : uint8_t { Absent, Valid, Corrupt };
struct GcMaintenanceReadResult
{
    GcMaintenanceReadStatus status;
    std::optional<GcMaintenanceState> state;
    std::optional<Incarnation> incarnation;
    String diagnostic;
};

GcMaintenanceReadResult readGcMaintenanceState(CasOperation & op, const Layout & layout);
/// `create`s the maintenance-state key on absence, `replace`s it when `expected` names the
/// incarnation last observed. `policy` is named by the caller because the catch-path reset in
/// `NamespaceJanitor::runOnePage` uses `Retry::once()` while every other write here uses `standard`.
WriteResult casGcMaintenanceState(
    CasOperation & op, const Layout & layout, const std::optional<Incarnation> & expected,
    const GcMaintenanceState & next, const Retry & policy);

}
