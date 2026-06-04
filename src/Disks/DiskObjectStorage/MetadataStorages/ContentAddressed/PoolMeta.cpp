#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolCoordination.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>
#include <Core/UUID.h>

#include <ctime>

namespace DB
{

namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int SUPPORT_IS_DISABLED;
}

namespace ContentAddressed
{

std::string PoolMeta::serialize() const
{
    /// MAGIC(4) + encoding version(1) + body, on the shared codec. The body carries the POOL-content
    /// `version` FIRST (so the caller can gate compatibility) as a little-endian u32, then the owner
    /// server id (length-prefixed string) and the informational claimed_at_unix (little-endian i64).
    std::string out;
    DB::WriteBufferFromString buf(out);
    FormatHeader{MAGIC, ENCODING_VERSION}.write(buf);
    DB::writeBinaryLittleEndian(version, buf);
    DB::writeStringBinary(owner_server_id, buf);
    DB::writeBinaryLittleEndian(claimed_at_unix, buf);
    /// `pool_uuid` is appended LAST (version 2+). It comes after the body fields the version-1 layout
    /// already carried, so the body's leading `version` u32 still gates compatibility for old readers.
    DB::writeStringBinary(pool_uuid, buf);
    buf.finalize();
    return out;
}

PoolMeta PoolMeta::deserialize(const std::string & bytes)
{
    DB::ReadBufferFromString buf(bytes);
    /// The shared header gates only the ENCODING version (a wrong magic or an unparseable encoding
    /// fails closed here). The POOL-content `version` is a body field the CALLER checks, so this build
    /// can read a future-pool-version marker far enough to produce a precise fail-closed message.
    FormatHeader::readAndValidate(buf, MAGIC, ENCODING_VERSION, "_pool_meta");

    PoolMeta meta;
    DB::readBinaryLittleEndian(meta.version, buf);
    DB::readStringBinary(meta.owner_server_id, buf);
    DB::readBinaryLittleEndian(meta.claimed_at_unix, buf);
    /// `pool_uuid` is present in the body only for version 2+. Read it only when the body declares a
    /// version that carries it; a version-1 body has no such trailing field. The caller still gates the
    /// POOL-content `version` (an unknown/newer version fails closed there), so this only needs to parse
    /// far enough to read the version-2 layout this build understands.
    if (meta.version >= 2)
        DB::readStringBinary(meta.pool_uuid, buf);

    if (meta.owner_server_id.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: _pool_meta is missing required fields");

    return meta;
}

std::string claimPoolOwnership(
    const ObjectStoragePtr & object_storage,
    const std::string & key_prefix,
    const std::string & server_id,
    bool allow_shared,
    const LoggerPtr & log)
{
    const std::string key = poolMetaKey(key_prefix);

    /// Register THIS mounter in the per-mounter registry so the live set is listable from the bucket
    /// (the bucket is the single source of truth) — needed for the multi-mounter milestone. Idempotent:
    /// a re-mount loses the CAS (false), which is fine. This does NOT gate ownership; the single-owner
    /// check below is unchanged. Done first so even a fail-closed mount leaves a record it was attempted.
    auto register_mounter = [&]
    {
        PoolMeta marker;
        marker.version = PoolMeta::CURRENT_VERSION;
        marker.owner_server_id = server_id;
        marker.claimed_at_unix = static_cast<int64_t>(std::time(nullptr));
        condCreateIfAbsent(*object_storage, poolMounterKey(key_prefix, server_id), marker.serialize());
    };

    PoolMeta meta;
    meta.version = PoolMeta::CURRENT_VERSION;
    meta.owner_server_id = server_id;
    meta.claimed_at_unix = static_cast<int64_t>(std::time(nullptr));
    /// Mint the stable `pool_uuid` HERE — only the first claimant (the one that wins the CAS below)
    /// ever writes a freshly-minted value. `UUIDHelpers::generateV4` is the same server-context random
    /// UUID generator used for the `ServerUUID` (a server-side, non-deterministic identity is correct
    /// for a pool created once and read back by every later mount). On the lost-CAS paths below we
    /// instead READ and keep the existing `pool_uuid`, so it is never re-minted.
    meta.pool_uuid = toString(UUIDHelpers::generateV4());

    /// Absent -> claim, ATOMICALLY. `condCreateIfAbsent` is the compare-and-set: exactly one of two
    /// truly-concurrent first-mounters can create the marker (B51 — the old read-then-write let both
    /// see "absent" and both claim). If we created it we are the first owner — done.
    if (condCreateIfAbsent(*object_storage, key, meta.serialize()))
    {
        LOG_INFO(
            log, "ContentAddressed: claimed pool ownership at '{}' for server '{}' (pool_uuid '{}')",
            key, server_id, meta.pool_uuid);
        register_mounter();
        return meta.pool_uuid;
    }

    /// The CAS was lost: the marker already existed (a prior mount of ours, or another mounter that won
    /// the concurrent claim). Read it and apply the EXISTING single-owner compatibility rules unchanged.
    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);

    const PoolMeta existing = PoolMeta::deserialize(content);

    if (existing.version != PoolMeta::CURRENT_VERSION)
        throw Exception(
            ErrorCodes::SUPPORT_IS_DISABLED,
            "ContentAddressed: pool at '{}' was written with format version {} which this build does not "
            "understand (supported version {}); refusing to mount to avoid misinterpreting the pool",
            key, existing.version, PoolMeta::CURRENT_VERSION);

    if (existing.owner_server_id == server_id)
    {
        /// The same server re-mounting its own pool — the normal case (incl. each M6 test run, which
        /// uses a stable server id and a fresh pool). Nothing to do; keep the existing marker and its
        /// already-minted `pool_uuid` (never re-mint).
        LOG_TRACE(
            log, "ContentAddressed: pool at '{}' already owned by this server '{}' (pool_uuid '{}')",
            key, server_id, existing.pool_uuid);
        register_mounter();
        return existing.pool_uuid;
    }

    if (!allow_shared)
        throw Exception(
            ErrorCodes::SUPPORT_IS_DISABLED,
            "ContentAddressed: pool at '{}' is already owned by another mounter (server '{}'); concurrent "
            "multi-mounter use of a content_addressed pool is not supported yet (B11/B32). Set "
            "'content_addressed_allow_shared_pool' on the disk to acknowledge shared/takeover use.",
            key, existing.owner_server_id);

    /// Operator explicitly acknowledged shared use. Do NOT rewrite the marker (that would hide the
    /// first owner OR re-mint the pool_uuid); register THIS mounter in the registry and proceed. Shared
    /// use is coordinated (M8): the background sweep deletes only while it holds the fenced per-pool
    /// GC-leader lock, and live write-session pins keep just-uploaded blobs reachable across mounters,
    /// so concurrent mounters are safe. Keep the existing (creator's) `pool_uuid`. Log at INFO so the
    /// shared configuration stays visible without implying it is unsafe.
    LOG_INFO(
        log,
        "ContentAddressed: pool at '{}' is owned by another server '{}' (pool_uuid '{}') and "
        "'content_addressed_allow_shared_pool' is set; registering as a shared mounter (coordinated "
        "GC-leader lock + write-session pins). Server '{}'.",
        key, existing.owner_server_id, existing.pool_uuid, server_id);
    register_mounter();
    return existing.pool_uuid;
}

}

}
