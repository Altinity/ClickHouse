#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromMemory.h>
#include <Common/Exception.h>
#include <city.h>
#include <algorithm>
#include <cstring>
#include <memory>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

/// Run key is the 16-byte big-endian blob_hash (so RunFile's byte ordering == UInt128 numeric order).
String keyOf(const UInt128 & blob_hash)
{
    return u128ToBytesBE(blob_hash);
}

UInt128 keyToHash(const String & key)
{
    return u128FromBytesBE(key, "blob in-degree run key");
}

/// Payload is an 8-byte little-endian int64 count/delta.
String payloadOf(int64_t v)
{
    String s(8, '\0');
    auto u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i)
        s[i] = static_cast<char>(static_cast<UInt8>(u >> (8 * i)));
    return s;
}

int64_t payloadToInt(std::string_view p)
{
    if (p.size() != 8)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS blob in-degree: run payload must be 8 bytes, got {}", p.size());
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i)
        u |= static_cast<uint64_t>(static_cast<UInt8>(p[i])) << (8 * i);
    return static_cast<int64_t>(u);
}

UInt128 cityHash128(const String & bytes)
{
    const auto h = CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

/// Read every blob in-degree run segment of (generation, shard) into a flat sorted list of
/// (hash, count). The segments are written one per generation (seq 0); enumerating seq>0 future-proofs
/// the multi-segment layout. Returns rows in key order.
std::vector<std::pair<UInt128, int64_t>> readGenerationRows(
    Backend & backend, const Layout & layout, uint64_t generation, uint64_t attempt, uint64_t shard)
{
    std::vector<std::pair<UInt128, int64_t>> rows;
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got)
            break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        String k;
        String p;
        while (r.next(k, p))
            rows.emplace_back(keyToHash(k), payloadToInt(p));
    }
    return rows;
}

}

void putDeterministicArtifact(Backend & backend, const String & key, const String & bytes)
{
    if (backend.putIfAbsent(key, bytes).outcome == PutOutcome::PreconditionFailed)
    {
        const auto existing = backend.get(key);
        if (!existing || existing->bytes != bytes)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS gc: deterministic artifact at {} occupied by divergent bytes (impossible under "
                "correct operation; refusing to proceed)", key);
        /// byte-equal => our own deterministic replay; adopt (no-op).
    }
}

void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t prior_attempt,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs)
{
    /// Deterministic input ordering => byte-reproducible output run (OQ5 resume/adoption).
    std::sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b) { return a.blob_hash < b.blob_hash; });

    /// The prior generation's per-blob counts (one row per key, in key order). A key with count==0 in
    /// the prior gen is a transitioned-to-0 marker we must NOT carry forward as a fresh candidate.
    const std::vector<std::pair<UInt128, int64_t>> prior_rows =
        readGenerationRows(backend, layout, prior_generation, prior_attempt, shard);

    /// Merge prior counts with scattered deltas, both already in key order. For each key sum
    /// prior_count + Σ deltas. A merged count < 0 is an undercount (would over-delete) — fail closed.
    DB::WriteBufferFromOwnString out;
    RunHeader header;
    header.kind = RunKind::BlobInDegree;
    header.key_schema = 0;
    RunFileWriter writer(out, header);

    size_t pi = 0;
    size_t di = 0;
    while (pi < prior_rows.size() || di < scattered.size())
    {
        /// Pick the smallest key across the two cursors.
        UInt128 key;
        bool have = false;
        if (pi < prior_rows.size())
        {
            key = prior_rows[pi].first;
            have = true;
        }
        if (di < scattered.size() && (!have || scattered[di].blob_hash < key))
        {
            key = scattered[di].blob_hash;
            have = true;
        }
        chassert(have);

        int64_t prior_count = 0;
        bool prior_present = false;
        while (pi < prior_rows.size() && prior_rows[pi].first == key)
        {
            prior_count += prior_rows[pi].second;
            prior_present = true;
            ++pi;
        }
        int64_t delta_sum = 0;
        bool delta_present = false;
        while (di < scattered.size() && scattered[di].blob_hash == key)
        {
            delta_sum += scattered[di].delta;
            delta_present = true;
            ++di;
        }

        const int64_t merged = prior_count + delta_sum;
        if (merged < 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS blob in-degree: merged in-degree {} < 0 for a blob in gen {} shard {} "
                "(undercount — fail closed rather than over-delete)", merged, new_generation, shard);

        if (merged > 0)
        {
            writer.append(keyOf(key), payloadOf(merged));
        }
        else
        {
            /// merged == 0. Emit an explicit 0-row ONLY when this key actually transitioned to zero this
            /// generation (it was pinned before and a delta dropped it, OR a fresh +/- cancelled). A key
            /// that was already 0 in the prior gen and got no delta this gen is stale debris — drop it so
            /// it is not re-reported as a candidate every round.
            const bool transitioned = delta_present && (prior_count > 0 || (!prior_present && delta_sum == 0));
            if (transitioned)
                writer.append(keyOf(key), payloadOf(0));
        }
    }

    writer.finish();
    const String run_bytes = out.str();

    const String run_key = layout.blobTargetRunKey(new_generation, attempt, shard, 0);
    putDeterministicArtifact(backend, run_key, run_bytes);
    out_runs.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes)});
}

std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t attempt, uint64_t shard)
{
    std::vector<BlobCandidate> result;
    for (const auto & [hash, count] : readGenerationRows(backend, layout, generation, attempt, shard))
    {
        if (count == 0)
            result.push_back(BlobCandidate{.hash = hash});
    }
    return result;
}

int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t attempt, uint64_t shard, const UInt128 & blob_hash)
{
    /// The run is sorted by hash and carries at most one row per key; absent => 0 (never pinned, or a
    /// prior-gen zero that was dropped). An explicit 0-row (transitioned this gen) also reads as 0.
    for (const auto & [hash, count] : readGenerationRows(backend, layout, generation, attempt, shard))
        if (hash == blob_hash)
            return count;
    return 0;
}

}
