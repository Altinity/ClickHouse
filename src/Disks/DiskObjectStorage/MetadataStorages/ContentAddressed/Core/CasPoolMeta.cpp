#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint64_t POOL_META_VERSION = 1;

/// Pool-wide constant invariants, enforced in two contexts with different error codes:
///   - at creation, a bad value is the CALLER's config mistake => BAD_ARGUMENTS;
///   - on decode, a persisted object violating them is corruption => CORRUPTED_DATA.
/// `root_shards` must be at least 1; `blob_header_len` must be 8-aligned and within [96, 16 KiB].
void validateConstants(uint64_t root_shards, uint64_t blob_header_len, int error_code, std::string_view what)
{
    if (root_shards < 1)
        throw Exception(error_code, "CAS {}: root_shards must be >= 1, got {}", what, root_shards);
    if (blob_header_len < 96)
        throw Exception(error_code, "CAS {}: blob_header_len must be >= 96, got {}", what, blob_header_len);
    if (blob_header_len % 8 != 0)
        throw Exception(error_code, "CAS {}: blob_header_len must be a multiple of 8, got {}", what, blob_header_len);
    if (blob_header_len > 16 * 1024)
        throw Exception(error_code, "CAS {}: blob_header_len must be <= 16384, got {}", what, blob_header_len);
}

/// Two `thread_local_rng` u64 draws composed into a 128-bit id.
UInt128 mintPoolId()
{
    const UInt128 hi = thread_local_rng();
    const UInt128 lo = thread_local_rng();
    return (hi << 64) | lo;
}

}

String encodePoolMeta(const PoolMeta & pm)
{
    WriteBufferFromOwnString out;
    JsonObjectWriter writer(out);
    writer.field("format", "cas_pool_meta");
    writer.field("version", POOL_META_VERSION);
    writer.field("pool_id", u128ToHex(pm.pool_id));
    writer.field("root_shards", pm.root_shards);
    writer.field("blob_header_len", pm.blob_header_len);
    writer.finalize();
    return std::move(out.str());
}

PoolMeta decodePoolMeta(std::string_view data)
{
    return decodeJsonGuarded("pool meta", [&]
    {
        auto obj = parseJsonDocument(data, "cas_pool_meta", POOL_META_VERSION, "pool meta");
        checkNoUnknownKeys(*obj, {"format", "version", "pool_id", "root_shards", "blob_header_len"}, "pool meta");

        PoolMeta pm;
        pm.pool_id = requireHash(*obj, "pool_id", "pool meta");
        pm.root_shards = requireU64(*obj, "root_shards", "pool meta");
        pm.blob_header_len = requireU64(*obj, "blob_header_len", "pool meta");

        /// A persisted object violating the constant invariants is corruption, not a config error.
        validateConstants(pm.root_shards, pm.blob_header_len, ErrorCodes::CORRUPTED_DATA, "pool meta");
        return pm;
    });
}

PoolMeta PoolMeta::createOrValidate(Backend & backend, const Layout & layout, uint64_t root_shards, uint64_t blob_header_len)
{
    /// The passed config is the caller's responsibility — reject bad values before any I/O.
    validateConstants(root_shards, blob_header_len, ErrorCodes::BAD_ARGUMENTS, "pool meta");

    const String key = layout.poolMetaKey();

    /// Present => the pool is authoritative; ignore the passed config and validate like a reopen.
    if (auto existing = backend.get(key))
        return decodePoolMeta(existing->bytes);

    /// Absent => mint a pool id and try to create the object. A racing creator that wrote first
    /// turns our create-if-absent CAS into a Conflict; we then re-read and validate like a reopen.
    PoolMeta pm;
    pm.pool_id = mintPoolId();
    pm.root_shards = root_shards;
    pm.blob_header_len = blob_header_len;

    if (backend.casPut(key, encodePoolMeta(pm), /*expected*/ std::nullopt) == CasOutcome::Committed)
        return pm;

    /// Lost the race: the winner's object MUST be present now.
    auto winner = backend.get(key);
    if (!winner)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS pool meta: create-if-absent reported Conflict but '{}' is absent on re-read", key);
    return decodePoolMeta(winner->bytes);
}

}
