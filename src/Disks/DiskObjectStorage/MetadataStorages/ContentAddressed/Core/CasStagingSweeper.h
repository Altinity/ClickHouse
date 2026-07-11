#pragma once
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <base/types.h>

namespace DB::Cas
{

/// Mount-lease-scoped staging sweeper (S3-native staging plan
/// `docs/superpowers/plans/2026-07-11-cas-s3-native-staging.md` Task 6; design
/// `docs/superpowers/specs/2026-07-11-cas-s3-native-staging-design.md` §6 crash & orphan handling).
///
/// A leaked S3 staging object happens two ways: (1) an exception between `promoteStaged` succeeding and
/// `cleanupPendingTempFiles` deleting the staging key (`ContentAddressedTransaction.cpp`), or (2) an
/// aborted/cancelled transaction whose pending blobs were staged but never promoted — by design,
/// `cleanupPendingTempFiles` deliberately leaves an S3 staging object in place on the abort path (never a
/// bare `fs::remove` on a remote key), so this sweeper is its ONLY reclaimer. Debris from either case is
/// bounded to `staging/<mount_id>/` — the ONE mount that could ever have written under that prefix, since
/// every staging key this mount ever mints comes from `ContentAddressedMetadataStorage::stagingKeyPrefix()`
/// (`physicalKey(pool_prefix + "/staging/" + server_root_id)`), keyed by THIS mount's own `server_root_id`.
///
/// LEASE-FENCE (fail-closed, never fail-open): `sweepOwnMountStaging` removes ONLY objects whose key
/// starts with the given `mount_staging_prefix` — pass your OWN mount's prefix, never another mount's.
/// The caller (`ContentAddressedMetadataStorage::startup()`) invokes this exactly once, at mount start,
/// with `stagingKeyPrefix() + "/"` — the SAME prefix construction the writer uses to mint staging keys, so
/// this sweep can never reach a different mount's `staging/<other_mount_id>/` subtree: no other writer
/// ever stages a key under THIS mount's own `server_root_id` prefix, and this function never lists or
/// touches anything outside the prefix it is given.
///
/// Best-effort and NEVER THROWS: one stubborn key (or a LIST failure) must never abort the sweep of the
/// rest, and must never fail the mount (mirrors `feedback_ca_gc_never_throw_on_404` — a throw here would
/// only wedge startup, not GC, but the same fail-open-on-error discipline applies to any best-effort
/// reclaim of debris).
///
/// GC excludes `staging/` entirely: GC blob discovery LISTs `Layout::blobsPrefix()`
/// (`<pool_prefix>/blobs/`) — a distinct top-level prefix from `staging/`, `cas/refs/`, and
/// `cas/manifests/` (see `CasLayout.h`) — so a `staging/` object is never listed, HEAD'd, or condemned by
/// GC's fold. This sweeper is the ONLY reclaimer of `staging/` debris.
void sweepOwnMountStaging(IObjectStorage & object_storage, const String & mount_staging_prefix) noexcept;

}
