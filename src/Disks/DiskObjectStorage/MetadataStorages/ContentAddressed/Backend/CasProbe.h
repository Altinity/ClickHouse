#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <base/types.h>

namespace DB::Cas
{

/// Run the capability battery against `op`, using throwaway keys under `probe_prefix`.
///
/// `op` must already be admitted. The two store-level precondition hooks —
/// `Backend::checkPoolPreconditions` and `Backend::checkConditionalWriteSingleAttemptSupport` — are the
/// CALLER's responsibility, run through `CasRequests::backendForCapabilityPredicates()` before admitting
/// `op` and calling this: an admitted `CasOperation` carries no route back to the raw backend, by design,
/// so this function cannot reach them itself.
///
/// The battery validates the backend preconditions required by a writable content-addressed pool:
///   1. Conditional-create and conditional-overwrite are enforced (`create` prevents overwrites, and
///      `replace` rejects a stale incarnation).
///   2. `create`/`replace` support create-if-absent, conflict-on-existing, conflict-on-stale, and
///      commit-on-current — the same guarantees the legacy `casPut` chain validated.
///   3. Conditional-delete is enforced (`remove` with a stale incarnation is rejected and the object
///      survives).
///   4. Listing reflects both creation and deletion of a probe object.
///   5. Successful deletion does not create a versioning delete marker. A content-addressed pool cannot
///      reclaim storage correctly from a versioned bucket: garbage-collection deletes would archive old
///      versions instead of removing objects, and repeated ref updates would accumulate versions.
///
/// On any failed check, throws a DB::Exception(ErrorCodes::NOT_IMPLEMENTED) with a message naming the
/// specific failed check. This is fail-closed: a backend that does not pass the battery MUST NOT be
/// used to coordinate a content-addressed pool.
///
/// Cleanup of probe keys is best-effort and runs unconditionally: after the battery completes, or on
/// the failure path immediately before the check-failing exception is rethrown. Cleanup itself suppresses
/// exceptions so that it cannot hide the capability-check failure.
void runCapabilityProbe(CasOperation & op, const String & probe_prefix);

}
