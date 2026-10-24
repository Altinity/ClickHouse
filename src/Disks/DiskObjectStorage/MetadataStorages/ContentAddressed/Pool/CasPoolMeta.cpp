#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>
#include <algorithm>
#include <fmt/format.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int INVALID_STATE;
}
}

namespace DB::Cas
{

namespace
{

/// Two `thread_local_rng` u64 draws composed into a 128-bit id.
UInt128 mintPoolId()
{
    const UInt128 hi = thread_local_rng();
    const UInt128 lo = thread_local_rng();
    return (hi << 64) | lo;
}

/// Whether `config_algo` is already a registered member of `pool.algos_used`. Membership, not the
/// opt-in flag, is the steady-state check. `algos_used` is kept sorted, so this is a binary search.
bool isAlgoAdmittedIn(const PoolMeta & pool, BlobHashAlgo config_algo)
{
    const auto v = static_cast<uint8_t>(config_algo);
    return std::binary_search(pool.algos_used.begin(), pool.algos_used.end(), v);
}

/// Renders `algos_used` as "ch128, sha256" for the refusal message below.
String joinAlgoNames(const std::vector<uint8_t> & algos_used)
{
    String out;
    for (size_t i = 0; i < algos_used.size(); ++i)
    {
        if (i != 0)
            out += ", ";
        out += blobHashAlgoName(static_cast<BlobHashAlgo>(algos_used[i]));
    }
    return out;
}

/// Fail-closed on a non-admitted algo without the opt-in flag: admission is EXPLICIT
/// opt-in; the default stays fail-closed -- a changed config alone must never silently turn a pool
/// mixed. Never touches the pool.
[[noreturn]] void throwNotAdmitted(const PoolMeta & pool, BlobHashAlgo config_algo)
{
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "CAS pool blob_hash mismatch: pool has {{{}}}; config requests {}; set "
        "<cas_blob_hash_allow_new>1</cas_blob_hash_allow_new> to admit a new algo into this pool",
        joinAlgoNames(pool.algos_used), blobHashAlgoName(config_algo));
}

/// Read-modify-write of `_pool_meta` after `createOrValidate` proved it exists: `mutate` edits the decoded
/// meta in place and returns whether it changed anything (false = no write). Re-run against the fresh value
/// on every conditional-write retry. Returns the meta as last observed (mutated when it was written).
template <typename Mutate>
PoolMeta rmwPoolMeta(CasOperation & op, const String & key, std::string_view what, Mutate && mutate)
{
    PoolMeta observed;
    WriteResult result = op.readModifyWrite(
        key,
        [&](const std::optional<Object> & current) -> std::optional<String>
        {
            if (!current)
                throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS pool meta '{}' vanished during {}", key, what);
            observed = decodePoolMeta(current->bytes);
            if (!mutate(observed))
                return std::nullopt;
            return encodePoolMeta(observed);
        },
        Retry::standard());
    orThrow(std::move(result), fmt::format("CAS pool meta {} on '{}'", what, key));
    return observed;
}

/// The relaxed admission check (replaces an earlier fail-close that required the single pool algo to match):
/// re-reads `key` itself (via `readModifyWrite`'s own observe) rather than trusting a snapshot the
/// caller already holds, so a caller that only knows the key is present -- never a stale decode --
/// can ask this to settle admission. Already a member of `algos_used` => OK, no write (steady state,
/// the mutator declines). Not a member and `!allow_new` => `BAD_ARGUMENTS` (the pool is never touched).
/// Not a member and `allow_new` => CAS-union `config_algo` into `algos_used` (recomputed from the
/// FRESH value on every retry -- union-only, so there is no ABA). Every known algorithm is readable by a
/// generation-1 build, so admission never touches `min_reader_generation`. A concurrent
/// admitter's own union is folded in the same way, since the mutator runs again against whatever
/// `readModifyWrite` observes on retry.
PoolMeta admitOrValidate(CasOperation & op, const String & key, BlobHashAlgo config_algo, bool allow_new)
{
    return rmwPoolMeta(op, key, "admission", [&](PoolMeta & meta)
    {
        if (isAlgoAdmittedIn(meta, config_algo))
            return false;
        if (!allow_new)
            throwNotAdmitted(meta, config_algo);
        meta.algos_used.push_back(static_cast<uint8_t>(config_algo));
        std::sort(meta.algos_used.begin(), meta.algos_used.end());
        return true;
    });
}

}

PoolMeta PoolMeta::raiseMinReaderGeneration(CasOperation & op, const Layout & layout, uint64_t generation)
{
    return rmwPoolMeta(op, layout.poolMetaKey(), "reader-floor raise", [&](PoolMeta & meta)
    {
        if (meta.min_reader_generation >= generation)
            return false;
        meta.min_reader_generation = generation;
        return true;
    });
}

PoolMeta PoolMeta::createOrValidate(
    CasOperation & op, const Layout & layout, uint64_t blob_header_len, uint64_t gc_shards,
    BlobHashAlgo blob_hash_algo, bool allow_new, bool allow_mint, uint64_t initial_min_reader_generation)
{
    /// The passed config is the caller's responsibility — reject bad values before any I/O.
    validatePoolBlobHeaderLen(blob_header_len, ErrorCodes::BAD_ARGUMENTS, "pool meta");
    if (gc_shards == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CAS pool meta: gc_shards must be >= 1");
    /// `blobHashAlgoName` rejects an out-of-range `BlobHashAlgo` with `LOGICAL_ERROR`: a programming
    /// error that aborts debug and sanitizer builds, not an input-validation fence.
    blobHashAlgoName(blob_hash_algo);

    const String key = layout.poolMetaKey();

    /// Present => the pool is authoritative; ignore the passed config's blob_header_len and run the
    /// flag-gated admission check rather than the old single-value fail-close. The steady state (the
    /// configured algo is already admitted) is decided from THIS read, so the common open costs one
    /// GET; only a union or a `!allow_new` refusal falls through to `admitOrValidate`, which re-reads
    /// the key itself as part of its own conditional write.
    if (auto existing = op.read(key, Retry::standard()))
    {
        PoolMeta pm = decodePoolMeta(existing->bytes);
        if (isAlgoAdmittedIn(pm, blob_hash_algo))
            return pm;
        return admitOrValidate(op, key, blob_hash_algo, allow_new);
    }

    /// Absent => mint a pool id and try to create the object with `algos_used = {blob_hash_algo}`.
    /// The floor records which readers may mount; a new pool starts at the floor its writers need
    /// (generation 2), not at this build's read ceiling.
    ///
    /// BOOTSTRAP GATE: minting is permitted ONLY on the verified bootstrap path. A
    /// non-bootstrap caller (a read-only/observe open, `openForDecommission`) passes `allow_mint=false`
    /// and fails closed here — never minting a fresh identity outside that path (an observe scan that
    /// minted would poison the next writable mount's residual check). The residual EMPTINESS proof itself
    /// cannot live here: it must precede ANY write (spec's "zero-write residual check FIRST, before ANY
    /// probe write"), and by the time `createOrValidate` runs the capability battery has already written
    /// to the prefix, so `Pool::open` runs `probePoolBootstrapResidual` up front and only then passes
    /// `allow_mint=true`. `pool_prefix` is exclusively CAS-owned.
    if (!allow_mint)
        throw Exception(ErrorCodes::INVALID_STATE,
            "CAS pool meta: _pool_meta absent — refusing to mint outside the verified bootstrap path; "
            "run a writable mount (or recreate the pool)");

    PoolMeta pm;
    pm.pool_id = mintPoolId();
    pm.blob_header_len = blob_header_len;
    pm.gc_shards = gc_shards;
    pm.min_reader_generation = initial_min_reader_generation;
    pm.algos_used = {static_cast<uint8_t>(blob_hash_algo)};

    WriteResult result = op.create(key, encodePoolMeta(pm), Retry::standard());
    if (std::holds_alternative<Committed>(result))
        return pm;

    /// Lost the race: the winner's object MUST be present now. The loser UNIONS its algo via the SAME
    /// flag-gated admission path as a reopen, instead of the old unconditional fail-close.
    if (std::holds_alternative<Conflict>(result))
        return admitOrValidate(op, key, blob_hash_algo, allow_new);

    orThrow(std::move(result), fmt::format("CAS pool meta creation on '{}'", key));
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS pool meta: create-if-absent on '{}' neither committed, conflicted, nor threw", key);
}

}
