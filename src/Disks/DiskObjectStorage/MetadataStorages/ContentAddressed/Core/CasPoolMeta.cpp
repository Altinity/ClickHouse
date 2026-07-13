#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <cas_format.pb.h>
#include <Common/Exception.h>
#include <Common/thread_local_rng.h>
#include <algorithm>

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

namespace Proto = ::clickhouse::cas::format;

namespace
{

/// Pool-wide constant invariant over `blob_header_len`, enforced in two contexts with different
/// error codes:
///   - at creation, a bad value is the CALLER's config mistake => BAD_ARGUMENTS;
///   - on decode, a persisted object violating it is corruption => CORRUPTED_DATA.
/// `blob_header_len` must be 8-aligned and within [96, 16 KiB].
void validateBlobHeaderLen(uint64_t blob_header_len, int error_code, std::string_view what)
{
    if (blob_header_len < 96)
        throw Exception(error_code, "CAS {}: blob_header_len must be >= 96, got {}", what, blob_header_len);
    if (blob_header_len % 8 != 0)
        throw Exception(error_code, "CAS {}: blob_header_len must be a multiple of 8, got {}", what, blob_header_len);
    if (blob_header_len > 16 * 1024)
        throw Exception(error_code, "CAS {}: blob_header_len must be <= 16384, got {}", what, blob_header_len);
}

/// `algos_used` invariants (Phase 3 T4): non-empty, strictly increasing (implies no duplicates), and
/// every entry a value `BlobHashAlgo` actually admits (`blobHashAlgoName` throws `BAD_ARGUMENTS` for
/// anything else -- re-thrown here under the caller-selected `error_code` so a corrupt persisted value
/// reports CORRUPTED_DATA rather than BAD_ARGUMENTS).
void validateAlgosUsed(const std::vector<uint8_t> & algos_used, int error_code, std::string_view what)
{
    if (algos_used.empty())
        throw Exception(error_code, "CAS {}: algos_used must be non-empty", what);
    for (size_t i = 0; i < algos_used.size(); ++i)
    {
        try
        {
            blobHashAlgoName(static_cast<BlobHashAlgo>(algos_used[i]));
        }
        catch (const Exception &)
        {
            throw Exception(error_code, "CAS {}: algos_used contains an unknown algo {}", what, algos_used[i]);
        }
        if (i > 0 && algos_used[i] <= algos_used[i - 1])
            throw Exception(error_code,
                "CAS {}: algos_used must be strictly sorted with no duplicates, got {} at index {} not after {}",
                what, algos_used[i], i, algos_used[i - 1]);
    }
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
    msg.set_blob_header_len(pm.blob_header_len);
    msg.set_min_reader_generation(pm.min_reader_generation);
    for (uint8_t algo : pm.algos_used)
        msg.add_algos_used(algo);

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
    pm.blob_header_len = msg.blob_header_len();
    pm.min_reader_generation = msg.min_reader_generation();
    pm.algos_used.assign(msg.algos_used().begin(), msg.algos_used().end());

    /// A persisted object violating the constant invariant is corruption, not a config error.
    validateBlobHeaderLen(pm.blob_header_len, ErrorCodes::CORRUPTED_DATA, "pool meta");
    validateAlgosUsed(pm.algos_used, ErrorCodes::CORRUPTED_DATA, "pool meta");

    /// Startup gate (forward): if min_reader_generation > G_BUILD, this binary is too old to open the pool.
    if (G_BUILD < pm.min_reader_generation)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool requires reader generation {} but this build supports at most {}",
            pm.min_reader_generation, G_BUILD);

    /// Startup gate (backward, Task 12): the ref state changed INCOMPATIBLY from the mutable
    /// pre-generation-3 ref-shard objects to immutable `_log`/`_snap` objects at generation
    /// `kRefSnapshotLogGeneration`. A pool written by an older build stamps its pool-meta
    /// `compatibility_version` below that generation and holds pre-generation-3 ref-shard-format refs
    /// this build cannot read; fail closed rather than silently mis-recovering every table from a
    /// fresh-looking empty ref prefix. CAS is pre-release (no data to migrate) -- the pool must be recreated.
    if (msg.header().compatibility_version() < kRefSnapshotLogGeneration)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool was written with the removed pre-generation-3 mutable ref-shard format "
            "(compatibility generation {}); this build reads only the snapshot+log ref format "
            "(generation {}+). The pool is not openable; CAS is pre-release -- recreate it.",
            msg.header().compatibility_version(), kRefSnapshotLogGeneration);

    return pm;
}

namespace
{

/// Whether `config_algo` is already a registered member of `pool.algos_used` (steady-state check --
/// spec §5: "membership, not the flag, is the steady-state check"). `algos_used` is kept sorted, so
/// this is a binary search.
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

/// Fail-closed on a non-admitted algo without the opt-in flag (spec §5: admission is EXPLICIT
/// opt-in; the default stays fail-closed -- a changed config alone must never silently turn a pool
/// mixed). Never touches the pool.
[[noreturn]] void throwNotAdmitted(const PoolMeta & pool, BlobHashAlgo config_algo)
{
    throw Exception(ErrorCodes::BAD_ARGUMENTS,
        "CAS pool blob_hash mismatch: pool has {{{}}}; config requests {}; set "
        "<blob_hash_allow_new>1</blob_hash_allow_new> to admit a new algo into this pool",
        joinAlgoNames(pool.algos_used), blobHashAlgoName(config_algo));
}

/// The relaxed admission check (spec §5, RELAXES the Phase 1/2 `checkBlobHashAlgoMatches` fail-close):
/// `pm`/`token` are the most-recently-read `_pool_meta` state (present, decoded, valid). Already a
/// member of `algos_used` => OK, no write (steady state). Not a member and `!allow_new` =>
/// `BAD_ARGUMENTS` (the pool is never touched). Not a member and `allow_new` => CAS-union `config_algo`
/// into `algos_used` (recomputed from the FRESH value on every retry -- union-only, so there is no
/// ABA) and raises `min_reader_generation` to THIS build's own floor (`G_BUILD`, `CasFormat.h`) in
/// the SAME write (spec §5: "first registration of a schema-3-bearing algo also raises
/// `min_reader_generation`" -- a build that cannot decode schema-3 settlement state has an OLDER
/// `G_BUILD` and is correctly refused by the startup gate once a future generation bump lands here).
/// On a CAS conflict, re-read and retry the whole decision (a concurrent admitter may have unioned a
/// DIFFERENT algo, or the very one we wanted, in the meantime).
PoolMeta admitOrValidate(Backend & backend, const String & key, PoolMeta pm, Token token, BlobHashAlgo config_algo, bool allow_new)
{
    for (;;)
    {
        if (isAlgoAdmittedIn(pm, config_algo))
            return pm;

        if (!allow_new)
            throwNotAdmitted(pm, config_algo);

        PoolMeta next = pm;
        next.algos_used.push_back(static_cast<uint8_t>(config_algo));
        std::sort(next.algos_used.begin(), next.algos_used.end());
        next.min_reader_generation = G_BUILD;

        const CasResult res = backend.casPut(key, encodePoolMeta(next), token);
        if (res.outcome == CasOutcome::Committed)
            return next;

        auto fresh = backend.get(key);
        if (!fresh)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS pool meta: '{}' vanished mid-admission (conflicting write then a concurrent delete)", key);
        pm = decodePoolMeta(fresh->bytes);
        token = fresh->token;
        /// loop: re-evaluate membership against the FRESH pm (never re-encode the stale `next`)
    }
}

}

PoolMeta PoolMeta::createOrValidate(
    Backend & backend, const Layout & layout, uint64_t blob_header_len,
    BlobHashAlgo blob_hash_algo, bool allow_new)
{
    /// The passed config is the caller's responsibility — reject bad values before any I/O.
    validateBlobHeaderLen(blob_header_len, ErrorCodes::BAD_ARGUMENTS, "pool meta");
    /// Defense against a garbage `static_cast` past the caller's own boundary: `blobHashAlgoName`
    /// throws BAD_ARGUMENTS for anything `BlobHashAlgo` does not actually admit.
    blobHashAlgoName(blob_hash_algo);

    const String key = layout.poolMetaKey();

    /// Present => the pool is authoritative; ignore the passed config's blob_header_len and run the
    /// flag-gated admission check (spec §5) rather than the old single-value fail-close.
    if (auto existing = backend.get(key))
    {
        PoolMeta pm = decodePoolMeta(existing->bytes);
        return admitOrValidate(backend, key, std::move(pm), existing->token, blob_hash_algo, allow_new);
    }

    /// Absent => mint a pool id and try to create the object with `algos_used = {blob_hash_algo}`.
    /// Every pool this build creates is schema-3-shaped from birth (schemas 1/2 do not exist in this
    /// build at all), so the reader-generation floor is stamped at THIS build's `G_BUILD` at
    /// creation, not left at 0.
    PoolMeta pm;
    pm.pool_id = mintPoolId();
    pm.blob_header_len = blob_header_len;
    pm.min_reader_generation = G_BUILD;
    pm.algos_used = {static_cast<uint8_t>(blob_hash_algo)};

    if (backend.casPut(key, encodePoolMeta(pm), /*expected*/ std::nullopt).outcome == CasOutcome::Committed)
        return pm;

    /// Lost the race: the winner's object MUST be present now. The loser UNIONS its algo via the SAME
    /// flag-gated admission path as a reopen (spec §5), instead of the old unconditional fail-close.
    auto winner = backend.get(key);
    if (!winner)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS pool meta: create-if-absent reported Conflict but '{}' is absent on re-read", key);
    PoolMeta winner_pm = decodePoolMeta(winner->bytes);
    return admitOrValidate(backend, key, std::move(winner_pm), winner->token, blob_hash_algo, allow_new);
}

}
