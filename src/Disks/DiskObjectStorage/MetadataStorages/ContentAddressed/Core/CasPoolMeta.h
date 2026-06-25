#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
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

    /// GET `_pool_meta`; absent => mint `pool_id` (`thread_local_rng`) and `casPut(expected=nullopt)` —
    /// a racing creator loses the CAS, re-reads, and validates like a reopen. Present => strict parse.
    /// The POOL is authoritative on reopen: `root_shards` / `blob_header_len` come FROM the pool; the
    /// passed config values apply only at first creation.
    static PoolMeta createOrValidate(Backend &, const Layout &, uint64_t root_shards, uint64_t blob_header_len);
};

String encodePoolMeta(const PoolMeta &);     /// exposed for the round-trip test
PoolMeta decodePoolMeta(std::string_view);   /// strict; validates the constant invariants too

}
