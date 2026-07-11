#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// `_pool_meta` — the pool-identity object (spec §4). It carries the pool id minted once at
/// creation plus the pool-wide constants every build/read must agree on. Non-hashed metadata =>
/// STRICT JSON (encoding split, decision 2026-06-11):
///   {"format":"cas_pool_meta","version":1,"pool_id":"<32 lowercase hex>","root_shards":8,"blob_header_len":256}
///
/// The POOL is authoritative: on reopen the constants come FROM `_pool_meta`, and the config values
/// passed to `createOrValidate` apply only at first creation. The `pool_id` doubles as the envelope
/// `domain_id`, so it must be stable for the lifetime of the pool.
struct PoolMeta
{
    UInt128 pool_id{};                      /// minted at creation; doubles as the envelope domain_id
    uint64_t root_shards = 0;
    uint64_t blob_header_len = 0;
    uint64_t min_reader_generation = 0;     /// startup gate: if G_BUILD < min_reader_generation => UNKNOWN_FORMAT_VERSION
    /// CAS pluggable-blob-hash Phase 1 (spec 2026-07-11, §4): the pool's blob content-hash function,
    /// as `static_cast<uint8_t>(BlobHashAlgo)`. `1` = `BlobHashAlgo::CityHash128` (the default;
    /// existing pools created before this field existed decode to `1`, which is correct — they ARE
    /// cityHash128 pools). Fixed at pool creation; on reopen the recorded value is authoritative
    /// (`createOrValidate` fails closed on a disagreeing config, spec §8 — never re-hash a pool).
    uint8_t blob_hash_algo = 1;
    /// CAS pluggable-blob-hash Phase 2 (design §12, Task 1): the pool's digest byte width, derived
    /// from `blob_hash_algo` via `blobHashLenFor` (16 for `CityHash128`/`XXH3_128`, 32 for `Sha256`).
    /// NOT an independently persisted field yet (Phase 2 Task 6 adds real persistence + fail-close
    /// validation) -- `decodePoolMeta` and `createOrValidate` always RE-DERIVE it from the decoded/
    /// validated `blob_hash_algo`, so it can never drift from the algo it comes from. Feeds
    /// `DigestCodec` (`CasBlobDigest.h`), the ONE object all digest<->hex/bytes conversion routes
    /// through.
    uint64_t blob_hash_len = 16;

    /// GET `_pool_meta`; absent => mint `pool_id` (`thread_local_rng`) and `casPut(expected=nullopt)` —
    /// a racing creator loses the CAS, re-reads, and validates like a reopen. Present => strict parse.
    /// The POOL is authoritative on reopen: `root_shards` / `blob_header_len` / `blob_hash_algo` come
    /// FROM the pool; the passed config values apply only at first creation. `blob_hash_algo` additionally
    /// fails closed on reopen (BAD_ARGUMENTS) when the config disagrees with the recorded value.
    static PoolMeta createOrValidate(
        Backend &, const Layout &, uint64_t root_shards, uint64_t blob_header_len,
        BlobHashAlgo blob_hash_algo = BlobHashAlgo::CityHash128);
};

String encodePoolMeta(const PoolMeta &);     /// exposed for the round-trip test
PoolMeta decodePoolMeta(std::string_view);   /// strict; validates the constant invariants too

}
