#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

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

    if (meta.owner_server_id.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: _pool_meta is missing required fields");

    return meta;
}

void claimPoolOwnership(
    const ObjectStoragePtr & object_storage,
    const std::string & key_prefix,
    const std::string & server_id,
    bool allow_shared,
    const LoggerPtr & log)
{
    const std::string key = poolMetaKey(key_prefix);

    auto write_marker = [&]
    {
        PoolMeta meta;
        meta.version = PoolMeta::CURRENT_VERSION;
        meta.owner_server_id = server_id;
        meta.claimed_at_unix = static_cast<int64_t>(std::time(nullptr));
        const std::string bytes = meta.serialize();
        auto out = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite);
        out->write(bytes.data(), bytes.size());
        out->finalize();
    };

    if (!object_storage->tryGetObjectMetadata(key, /*with_tags=*/false))
    {
        /// Absent -> claim. Single-process-correct: there is no compare-and-set here (that is B32);
        /// two truly concurrent first-mounters could both claim, which is exactly the case the full
        /// lease protocol closes. For M1 single-owner pools this is sound.
        LOG_INFO(log, "ContentAddressed: claiming pool ownership at '{}' for server '{}'", key, server_id);
        write_marker();
        return;
    }

    StoredObject object(key);
    auto buf = object_storage->readObject(object, getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);

    const PoolMeta meta = PoolMeta::deserialize(content);

    if (meta.version != PoolMeta::CURRENT_VERSION)
        throw Exception(
            ErrorCodes::SUPPORT_IS_DISABLED,
            "ContentAddressed: pool at '{}' was written with format version {} which this build does not "
            "understand (supported version {}); refusing to mount to avoid misinterpreting the pool",
            key, meta.version, PoolMeta::CURRENT_VERSION);

    if (meta.owner_server_id == server_id)
    {
        /// The same server re-mounting its own pool — the normal case (incl. each M6 test run, which
        /// uses a stable server id and a fresh pool). Nothing to do; keep the existing marker.
        LOG_TRACE(log, "ContentAddressed: pool at '{}' already owned by this server '{}'", key, server_id);
        return;
    }

    if (!allow_shared)
        throw Exception(
            ErrorCodes::SUPPORT_IS_DISABLED,
            "ContentAddressed: pool at '{}' is already owned by another mounter (server '{}'); concurrent "
            "multi-mounter use of a content_addressed pool is not supported yet (B11/B32). Set "
            "'content_addressed_allow_shared_pool' on the disk to acknowledge shared/takeover use.",
            key, meta.owner_server_id);

    /// Operator explicitly acknowledged shared/takeover use. Do NOT rewrite the marker (that would
    /// hide the other owner); proceed but log loudly so the unsafe configuration is visible.
    LOG_WARNING(
        log,
        "ContentAddressed: pool at '{}' is owned by another server '{}' but 'content_addressed_allow_shared_pool' "
        "is set; proceeding without coordination (unsafe — background GC must stay disabled). Server '{}'.",
        key, meta.owner_server_id, server_id);
}

}

}
