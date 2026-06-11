#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <base/types.h>

namespace DB::Cas
{

/// Run the capability battery against `backend`, using throwaway keys under `probe_prefix`.
///
/// The probe validates that the backend:
///   1. Enforces conditional-create (putIfAbsent prevents overwrites).
///   2. Enforces conditional-overwrite (putOverwrite rejects wrong-token updates).
///   3. Enforces casPut semantics (create-if-absent, conflict-on-existing, conflict-on-stale, commit-on-current).
///   4. Enforces conditional-delete (deleteExact with a wrong token is REJECTED and the object survives).
///   5. Supports list-after-write (object appears in the listing after creation).
///   6. Does NOT create versioning delete markers (deleteExact on success must have created_delete_marker == false).
///   7. Supports list-after-delete (object disappears from the listing after deletion).
///
/// On any failed check, throws a DB::Exception(ErrorCodes::NOT_IMPLEMENTED) with a message naming the
/// specific failed check. This is fail-closed: a backend that does not pass the battery MUST NOT be
/// used to coordinate a content-addressed pool.
///
/// Cleanup of probe keys is best-effort and runs unconditionally (even after a failed check, after
/// the throw is caught by the caller, or on a passing run). Within this function, cleanup runs at the
/// end (after the battery completes or after a failure — via RAII scope guard).
void runCapabilityProbe(Backend & backend, const String & probe_prefix);

}
