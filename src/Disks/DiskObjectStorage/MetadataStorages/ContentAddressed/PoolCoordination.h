#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <cstdint>
#include <optional>
#include <string>

namespace DB::ContentAddressed
{

/// The foundational create-if-absent compare-and-set (CAS) primitive for content-addressed pool
/// coordination. The object-store bucket is the single source of truth and this conditional create is
/// the ONLY consistency primitive we assume; everything in the safe-shared-pool work (write-session
/// pins, the GC-leader lock, fencing, the `_pool_meta` claim) is built on it.
///
/// Atomically create `key` carrying `bytes` iff it does not already exist. Returns true if THIS call
/// created the object; false if it already existed (the CAS was lost — another writer won). Throws on
/// any other error.
///
/// Backends:
///   - S3-like / Azure: the write is issued with `If-None-Match: *`, so the object store rejects a PUT
///     onto an existing key with `PreconditionFailed` (HTTP 412), surfaced as a conflict (returns
///     false). This field is honored ONLY by S3-like / Azure object storages.
///   - `LocalObjectStorage`: there is no conditional-PUT, so the create is made atomic with an
///     `O_CREAT | O_EXCL` open at the resolved local path (`EEXIST` -> conflict). This keeps the
///     primitive unit-testable without MinIO.
///
/// Fail closed: never silently fall back to a read-then-write (that races). If the backend cannot be
/// shown to support conditional create, `condCreateIfAbsent` throws `NOT_IMPLEMENTED` rather than
/// performing an unsafe non-atomic create.
bool condCreateIfAbsent(IObjectStorage & object_storage, const std::string & key, const std::string & bytes);

/// Allocate a fence token: a monotonically-increasing, never-repeating u64 built ENTIRELY on the
/// create-if-absent CAS primitive (the only primitive the local test backend can offer atomically).
/// Tokens live as tiny objects at `pool/fence/<n>` (n = 1,2,3...). Starting from `start_hint` (clamped
/// to >= 1), scan n upward and `condCreateIfAbsent(fence/<n>)` until one create SUCCEEDS; that n is the
/// returned token. Because only one caller can ever create `fence/<n>` for a given n, no two callers
/// can be handed the same token, and a later allocation always yields a strictly higher token than any
/// earlier-COMPLETED one. The token is the GC-safety authority (a later task re-checks the max fence at
/// delete time), so the lock object below is only a liveness hint.
uint64_t allocateFenceToken(IObjectStorage & object_storage, const std::string & key_prefix, uint64_t start_hint);

/// The GC-leader lock record persisted at `pool/gc.lock`. It records who currently believes they hold
/// GC leadership, until when (an advisory lease), and the fence token they took. It is a CONVENIENCE /
/// liveness hint only: the safety backstop (a paused holder must not delete) is enforced later by
/// re-checking the max fence at delete time. The fence token — not this lock object — is the authority.
struct GcLock
{
    /// The mounter (`ServerUUID`) that took the lock.
    std::string server_id;
    /// Unix seconds until which the holder claims leadership. A liveness HINT only — an expired lock may
    /// be stolen; never the basis of a positive safety decision.
    uint64_t lease_deadline_unix = 0;
    /// The fence token the holder allocated when taking the lock. Strictly increases on every steal, so
    /// a higher token on disk means a successor took over and the previous holder lost leadership.
    uint64_t fence_token = 0;

    /// Binary, deterministic, explicitly little-endian on the shared codec (cross-arch determinism).
    std::string serialize() const;

    /// Parse the lock. Throws on a malformed object (wrong magic, an encoding version this build does
    /// not understand, or a truncated body) — fail-closed, never best-effort.
    static GcLock deserialize(const std::string & bytes);

    /// 4-byte magic `CAGL` ("Content-Addressed Gc Lock") + a 1-byte ENCODING version, per the shared
    /// codec. Distinct from the manifest (`CAMF`), sidecar (`CASC`), ref payload (`CARF`), pool meta
    /// (`CAPM`) and write session (`CAWS`) families, so a stray object is rejected rather than misparsed.
    static constexpr FormatMagic MAGIC = makeMagic("CAGL");
    static constexpr uint8_t ENCODING_VERSION = 1;
};

/// Try to take GC leadership. `now_unix` is passed in (the clock is never read inside) so tests are
/// deterministic. Behaviour:
///   - `gc.lock` absent  -> cond-create it with a freshly-allocated fence (start_hint = 1). If THIS
///     call created it, we are the leader: return the GcLock.
///   - present and LIVE   (lease_deadline_unix >= now_unix) -> someone holds it: return nullopt.
///   - present and EXPIRED -> STEAL: allocate a fence with start_hint = existing.fence_token + 1 (so the
///     new token is strictly higher than the dead holder's), then rewrite `gc.lock` with the new record
///     and return it.
/// A two-stealer race on the steal rewrite is acceptable: both stealers allocate DISTINCT (and both
/// higher) fence tokens, and the fence — not the lock object — is the safety authority that a later task
/// re-checks before deleting. The lock object merely records the most-recent rewrite.
std::optional<GcLock> tryAcquireGcLock(
    IObjectStorage & object_storage,
    const std::string & key_prefix,
    const std::string & server_id,
    uint64_t lease_seconds,
    uint64_t now_unix);

/// Renew an already-held lock: extend its lease to `now_unix + lease_seconds` keeping the SAME fence
/// token and server id, but ONLY if the on-disk lock still carries `held.fence_token`. If a higher fence
/// took over (a successor stole leadership), we lost it: return false and do not write. On success,
/// update `held.lease_deadline_unix` and return true.
bool renewGcLock(
    IObjectStorage & object_storage,
    const std::string & key_prefix,
    GcLock & held,
    uint64_t lease_seconds,
    uint64_t now_unix);

/// Release a held lock: delete `gc.lock`, but ONLY if it still carries `held.fence_token` (a read-check
/// so we never delete a successor's lock). Best-effort: a missing lock is a no-op.
void releaseGcLock(IObjectStorage & object_storage, const std::string & key_prefix, const GcLock & held);

}
