#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <base/types.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace DB::ContentAddressed
{

/// In-flight part-write PIN. While a part is being written, its blob hashes have been uploaded (or are
/// being uploaded) to `blobs/` but are NOT yet referenced by any published ref. A concurrent GC sweep
/// on ANOTHER mounter, listing the bucket, would otherwise see those blobs as unreachable and reclaim
/// them out from under the live writer (the B49 data-loss race, but cross-mounter). A `WriteSession`
/// object published in the bucket records that pending set so a remote GC treats the listed hashes as
/// reachable for the lifetime of the write. The write-protocol wiring (when the session is published,
/// refreshed and removed) is a separate task; this struct + its codec is the on-object form.
///
/// The session also carries the owning mounter's identity, an advisory lease deadline (a liveness HINT
/// only — GC treats an EXPIRED session as reclaimable so a crashed writer cannot pin blobs forever; it
/// is never used to make a positive safety decision), and a fence token used by the commit path of a
/// later task to fence a paused writer. Time protects failure detection here, never live work.
struct WriteSession
{
    /// The mounter (`ServerUUID`) that owns this session.
    std::string server_id;
    /// Unix seconds after which the session is a liveness suspect: GC treats an expired session as
    /// reclaimable. This is a HINT only — never the basis of a positive "still live" decision.
    UInt64 lease_deadline_unix = 0;
    /// Fences a paused writer at commit (used by a later task); 0 until assigned.
    UInt64 fence_token = 0;
    /// The part this pin is for.
    PartId part_id;
    /// The blob hashes uploaded-or-being-uploaded for this part, not yet referenced by a ref. GC must
    /// treat every hash here as reachable while the (unexpired) session is present.
    std::vector<BlobHash> pending;

    /// CA GC S4 (§5.1 rule 3, §7.3) — the session is now retained until its `+` deltas are FOLDED into a
    /// durable snapshot, not merely until commit. These two fields carry the durable foldedness state so a
    /// reaper (running on a different round, even after a crash/restart) can decide reapability on its own:
    ///
    ///   `committed` — false while the writer is still staging/uploading (an ABORT before commit drops the
    ///   session in O(1): nothing was referenced). Set true once the live ref is published; only THEN does
    ///   the session linger to cover the `+`-before-fold gap (rule 3).
    ///   `delta_epochs` — the `(shard, epoch)` each of this commit's `+` fragments durably settled in (after
    ///   the §5.1 rule-2 re-append). The reaper deletes the session once `GcCompaction::isEpochFolded` holds
    ///   for EVERY pair (the §7.3 mechanical rule (b): all delta event_ids folded). Empty for an uncommitted
    ///   session (then `committed` is false) or for a delta-less commit (then it is trivially folded).
    bool committed = false;
    std::vector<std::pair<ShardId, UInt64>> delta_epochs;

    /// Binary, deterministic, explicitly little-endian on the shared codec (cross-arch determinism).
    std::string serialize() const;

    /// Parse the session. Throws on a malformed object (wrong magic, an encoding version this build does
    /// not understand, or a truncated body) — fail-closed, never best-effort.
    static WriteSession deserialize(const std::string & bytes);

    /// 4-byte magic `CAWS` ("Content-Addressed Write Session") + a 1-byte ENCODING version, per the
    /// shared codec. A stray / foreign object is rejected (bad magic) rather than misparsed. The magic
    /// is distinct from the manifest (`CAMF`), sidecar (`CASC`), ref payload (`CARF`) and pool meta
    /// (`CAPM`) families.
    static constexpr FormatMagic MAGIC = makeMagic("CAWS");
    /// Version 2 (CA GC S4) appends the `committed` flag and the `delta_epochs` `(shard, epoch)` list to the
    /// body so the session-until-folded reaper can decide reapability durably (§5.1 rule 3, §7.3). A v3 pool
    /// is created fresh (PoolMeta v3, no back-compat), so no v1 session can exist in it — reading only v2 is
    /// correct and fail-closed.
    static constexpr uint8_t ENCODING_VERSION = 2;
};

}
