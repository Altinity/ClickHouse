#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_root_shard.pb.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{

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
    Cas::Proto::PoolMetaProto msg;

    /// Set CasHeader as field 1 (pure protobuf — no binary prefix).
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::PoolMeta));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_pool_id(u128ToBytesBE(pm.pool_id));
    msg.set_root_shards(pm.root_shards);
    msg.set_blob_header_len(pm.blob_header_len);
    msg.set_min_reader_generation(pm.min_reader_generation);

    std::string out;
    if (!msg.SerializeToString(&out))
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS pool meta: protobuf serialization failed");
    return out;
}

PoolMeta decodePoolMeta(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: empty object");

    /// Parse the whole message directly (pure protobuf, no binary prefix).
    Cas::Proto::PoolMetaProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: protobuf parse failed");

    /// Check magic then compatibility_version BEFORE reading any other fields.
    if (msg.header().magic() != magicFor(FormatId::PoolMeta))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS pool meta: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::PoolMeta));
    checkCompatibility(msg.header().compatibility_version(), "pool meta");

    PoolMeta pm;
    pm.pool_id = u128FromBytesBE(msg.pool_id(), "pool meta pool_id");
    pm.root_shards = msg.root_shards();
    pm.blob_header_len = msg.blob_header_len();
    pm.min_reader_generation = msg.min_reader_generation();

    /// A persisted object violating the constant invariants is corruption, not a config error.
    validateConstants(pm.root_shards, pm.blob_header_len, ErrorCodes::CORRUPTED_DATA, "pool meta");

    /// Startup gate: if min_reader_generation > G_BUILD, this binary is too old to open the pool.
    if (G_BUILD < pm.min_reader_generation)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool requires reader generation {} but this build supports at most {}",
            pm.min_reader_generation, G_BUILD);

    return pm;
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
    pm.min_reader_generation = 0;   /// pre-release: no minimum reader generation required

    if (backend.casPut(key, encodePoolMeta(pm), /*expected*/ std::nullopt).outcome == CasOutcome::Committed)
        return pm;

    /// Lost the race: the winner's object MUST be present now.
    auto winner = backend.get(key);
    if (!winner)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS pool meta: create-if-absent reported Conflict but '{}' is absent on re-read", key);
    return decodePoolMeta(winner->bytes);
}

}
