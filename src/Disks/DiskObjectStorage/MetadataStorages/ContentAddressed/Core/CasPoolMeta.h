#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>
#include <vector>

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
    /// CAS mixed-algo pools (Phase 3 T4, design 2026-07-11-cas-mixed-algo-pools-design.md §5):
    /// every hash algo ever ADMITTED to this pool, as `static_cast<uint8_t>(BlobHashAlgo)`,
    /// canonically SORTED and APPEND-ONLY (never shrinks, never reorders). Replaces the Phase 1/2
    /// single fail-closed `blob_hash_algo` -- a pool may now hold blobs under SEVERAL algos at once
    /// (additive switching, no migration). `PoolMeta::createOrValidate` is pool-authoritative: a
    /// config algo already IN this set is accepted with no write; a config algo NOT in this set is
    /// admitted via a CAS-union (opt-in, `blob_hash_allow_new`) or refused (`BAD_ARGUMENTS`, the
    /// default). There is no separately-persisted digest width anymore -- `Cas::codecFor(algo)`
    /// (`CasBlobRef.h`) derives it per-`BlobRef` from `blobHashLenFor`, never from the pool.
    std::vector<uint8_t> algos_used;

    /// GET `_pool_meta`; absent => mint `pool_id` (`thread_local_rng`), seed `algos_used = {blob_hash_algo}`,
    /// and `casPut(expected=nullopt)` — a racing creator loses the CAS and re-enters the SAME
    /// admission path as a reopen (spec §5: "the creation-race loser UNIONS its algo instead of
    /// today's fail-close"). Present => strict parse, then admission: `blob_hash_algo` already a
    /// member of `algos_used` => OK, no write; not a member and `allow_new` => CAS-union it in
    /// (read+token -> insert sorted -> casPut; re-read and retry on conflict); not a member and
    /// `!allow_new` => `BAD_ARGUMENTS` naming `<blob_hash_allow_new>` (never touches the pool). The
    /// POOL is authoritative on reopen: `root_shards` / `blob_header_len` come FROM the pool; the
    /// passed config values apply only at first creation.
    static PoolMeta createOrValidate(
        Backend &, const Layout &, uint64_t root_shards, uint64_t blob_header_len,
        BlobHashAlgo blob_hash_algo = BlobHashAlgo::CityHash128, bool allow_new = false);
};

String encodePoolMeta(const PoolMeta &);     /// exposed for the round-trip test
PoolMeta decodePoolMeta(std::string_view);   /// strict; validates the constant invariants too

}
