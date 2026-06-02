#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Common/Logger.h>
#include <cstdint>
#include <optional>
#include <string>

namespace DB::ContentAddressed
{

/// On-disk pool-ownership marker (`_pool_meta`) — the conservative M1 form of the B11 multi-mounter
/// guard. A single small object at the pool root recording the on-disk pool format version and the
/// identity of the server that currently owns (mounts) the pool. It lets a process fail closed when
/// it would otherwise silently share an un-coordinated pool with a second mounter, which is what
/// gates re-enabling background GC later (a GC sweep must never run on a pool another live server
/// could be writing to). The full lease/leader protocol is B32; this only stops two *independent*
/// servers from silently sharing one pool.
struct PoolMeta
{
    /// Bumped only on an INCOMPATIBLE pool-content change. A reader that sees a version it does
    /// not understand fails closed (a newer server must not silently reinterpret an older pool, and
    /// an older server must not touch a pool written by a newer one). This is the POOL-content version
    /// carried in the body; it is distinct from the codec ENCODING version in the shared FormatHeader.
    static constexpr uint32_t CURRENT_VERSION = 1;

    /// 4-byte magic `CAPM` ("Content-Addressed Pool Meta") + a 1-byte ENCODING version, per the shared
    /// codec. A stray / foreign object at `_pool_meta` is rejected (bad magic) rather than misparsed.
    static constexpr FormatMagic MAGIC = makeMagic("CAPM");
    static constexpr uint8_t ENCODING_VERSION = 1;

    uint32_t version = CURRENT_VERSION;
    /// The owning server's identity (`ServerUUID`). Empty is never written.
    std::string owner_server_id;
    /// Unix seconds when ownership was claimed (informational / debugging only — NOT used for any
    /// time-based safety decision; time may protect failure detection, never live work).
    int64_t claimed_at_unix = 0;

    /// Binary, deterministic, explicitly little-endian on the shared codec (cross-arch determinism).
    std::string serialize() const;

    /// Parse the marker. Throws on a malformed object (wrong magic, or an encoding version this build
    /// does not understand). Does NOT enforce the POOL-content `version` here — the caller decides what
    /// to do with an unknown pool version so it can produce a precise fail-closed message.
    static PoolMeta deserialize(const std::string & bytes);
};

/// Claim or validate pool ownership on mount. Reads `_pool_meta` at the pool root and:
///   - absent              -> write a fresh marker owned by this server (claim);
///   - present, our id     -> OK (same server re-mounting, the common case incl. each M6 test run);
///   - present, unknown ver-> throw SUPPORT_IS_DISABLED (this build cannot understand the pool);
///   - present, other id   -> throw SUPPORT_IS_DISABLED, UNLESS allow_shared (operator opt-in) — in
///                            which case we log loudly and proceed without rewriting the marker.
/// Single-process-correct only (no lease/CAS); the shared-pool protocol is B32.
void claimPoolOwnership(
    const ObjectStoragePtr & object_storage,
    const std::string & key_prefix,
    const std::string & server_id,
    bool allow_shared,
    const LoggerPtr & log);

}
