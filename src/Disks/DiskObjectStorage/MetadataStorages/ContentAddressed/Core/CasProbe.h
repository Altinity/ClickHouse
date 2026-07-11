#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
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

/// Probe whether `object_storage` ENFORCES a write-once conditional server-side copy
/// (`IObjectStorage::copyObjectConditional`, `If-None-Match: *`) — an OPTIONAL capability, unlike
/// the mandatory battery above (`runCapabilityProbe`). Only meaningful for a disk configured with
/// `cas_staging_backend=s3` (design `docs/superpowers/specs/2026-07-11-cas-s3-native-staging-design.md`
/// §3): when the backend does not enforce the precondition, the S3-native staging promote path is
/// UNSAFE (it could silently overwrite a live blob), so the metadata layer must fall back to local
/// staging rather than refuse to mount.
///
/// Goes directly through `IObjectStorage`, not through `Backend`/`CasProbe`'s battery — this keeps
/// the probe decoupled from the (later-task) S3-staging promote machinery it is gating.
///
/// Writes a tiny throwaway object at `<probe_prefix>/src`, conditionally copies it to
/// `<probe_prefix>/dst` (expects `created == true` — a fresh destination), then repeats the SAME
/// conditional copy onto the now-existing `dst` (expects `created == false` — the destination must
/// be REJECTED, proving the backend enforces `If-None-Match`). Returns `true` only when both
/// expectations hold.
///
/// Fail-close: ANY exception (including the default `NOT_IMPLEMENTED` a backend throws when it does
/// not override `copyObjectConditional` at all) OR a non-enforcing result (the second copy also
/// reports `created == true`, i.e. the backend silently overwrote the destination) returns `false`.
/// This function never throws — the caller treats `false` as "fall back to local staging", never as
/// a mount failure.
///
/// Cleanup of the probe objects (`src`, `dst`) is best-effort and runs unconditionally (RAII scope
/// guard), mirroring `runCapabilityProbe`.
bool probeConditionalCopy(IObjectStorage & object_storage, const String & probe_prefix);

}
