#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>

#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <ctime>
#include <sstream>

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
    /// One key=value per line. version FIRST so a future reader can read it before anything else.
    std::ostringstream out;
    out << MAGIC << "\n";
    out << "version=" << version << "\n";
    out << "owner_server_id=" << owner_server_id << "\n";
    out << "claimed_at_unix=" << claimed_at_unix << "\n";
    return out.str();
}

PoolMeta PoolMeta::deserialize(const std::string & bytes)
{
    std::istringstream in(bytes);
    std::string line;

    if (!std::getline(in, line) || line != MAGIC)
        throw Exception(
            ErrorCodes::CORRUPTED_DATA,
            "ContentAddressed: _pool_meta object has an unexpected format (bad magic); refusing to mount the pool");

    PoolMeta meta;
    bool have_version = false;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "ContentAddressed: malformed _pool_meta line '{}'", line);
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "version")
        {
            meta.version = parse<uint32_t>(value);
            have_version = true;
        }
        else if (key == "owner_server_id")
            meta.owner_server_id = value;
        else if (key == "claimed_at_unix")
            meta.claimed_at_unix = parse<int64_t>(value);
        /// Unknown keys are ignored on purpose: a newer (but still version-compatible) writer may add
        /// fields; the version byte is the hard compatibility gate.
    }

    if (!have_version || meta.owner_server_id.empty())
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
