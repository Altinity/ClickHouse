#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

class Backend;
class Layout;

/// `_pool_meta` — pool identity + the pool-wide constants every build/read agrees on (spec §4).
/// v3 text form: header line + one JSON body object
/// {"pid":"<32hex>","hln":<blob_header_len>,"mrg":<min_reader_generation>,"alg":"<algo-words>"}.
/// The POOL is authoritative on reopen: constants come FROM this object; `createOrValidate`'s config
/// args apply only at first creation. `pool_id` doubles as the envelope domain_id — stable for life.
struct PoolMeta
{
    UInt128 pool_id{};
    uint64_t blob_header_len = 0;
    uint64_t min_reader_generation = 0;
    /// Every hash algo ever admitted, as static_cast<uint8_t>(BlobHashAlgo), sorted + append-only.
    std::vector<uint8_t> algos_used;

    static PoolMeta createOrValidate(
        Backend &, const Layout &, uint64_t blob_header_len,
        BlobHashAlgo blob_hash_algo = BlobHashAlgo::CityHash128, bool allow_new = false);
};

String encodePoolMeta(const PoolMeta &);
PoolMeta decodePoolMeta(std::string_view);

/// Pure invariant checks over the pool constants, shared by decodePoolMeta (a persisted violation is
/// corruption => pass CORRUPTED_DATA) and PoolMeta::createOrValidate (a bad caller config => pass
/// BAD_ARGUMENTS). `blob_header_len` must be 8-aligned and within [96, 16 KiB]; `algos_used` must be
/// non-empty, strictly increasing, and every entry a real BlobHashAlgo.
void validatePoolBlobHeaderLen(uint64_t blob_header_len, int error_code, std::string_view what);
void validatePoolAlgosUsed(const std::vector<uint8_t> & algos_used, int error_code, std::string_view what);

}
