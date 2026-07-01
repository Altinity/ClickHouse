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

constexpr char kEdgeActive    = 0x01;   // sealed-run row: a surviving active edge
constexpr char kZeroMarker    = 0x00;   // sealed-run row: blob transitioned to zero this generation
const UInt128 kZeroSourceId{0};

UInt128 cityHash128(const String & bytes)
{
    const auto h = CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

// Read every (blob_hash, source_id) row of the prior generation's SourceEdge run into a sorted list.
// Streaming; one RunFileReader. Skips zero-marker rows (they are per-generation, not carried forward).
std::vector<std::pair<String, char>> readPriorEdges(
    Backend & backend, const Layout & layout, uint64_t generation, uint64_t attempt, uint64_t shard)
{
    std::vector<std::pair<String, char>> rows;   // (32-byte key, tag)
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got)
            break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        String k, p;
        while (r.next(k, p))
            if (!p.empty() && p[0] == kEdgeActive)   // carry forward only surviving edges
                rows.emplace_back(k, kEdgeActive);
    }
    return rows;   // already sorted by (blob_hash, source_id): the run is sorted
}

}

UInt128 sourceEdgeId(const ManifestId & id, const String & path)
{
    String canon;
    canon += id.root_namespace.string();
    canon += '\0';
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    auto beU32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(id.ref.writer_epoch); beU64(id.ref.build_sequence); beU32(id.ref.manifest_ordinal);
    canon += '\0';
    canon += path;
    const auto h = CityHash_v1_0_2::CityHash128(canon.data(), canon.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id)
{
    return u128ToBytesBE(blob_hash) + u128ToBytesBE(source_id);
}

bool parseSrcEdgeRunKey(const String & key, UInt128 & blob_hash, UInt128 & source_id)
{
    if (key.size() != 32)
        return false;
    blob_hash = u128FromBytesBE(key.substr(0, 16), "src-edge run key blob_hash");
    source_id = u128FromBytesBE(key.substr(16, 16), "src-edge run key source_id");
    return true;
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
    // Deterministic input ordering => byte-reproducible run (OQ5 resume/adoption).
    // MUST be stable: for the same (blob_hash, source_id) the journal ordering is
    // activation-before-removal; "last wins" then correctly resolves to removal (edge absent).
    // An unstable sort can put removal before activation => last=activation => false positive.
    std::stable_sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b)
        {
            if (a.blob_hash != b.blob_hash) return a.blob_hash < b.blob_hash;
            return a.source_id < b.source_id;
        });

    const auto prior = readPriorEdges(backend, layout, prior_generation, prior_attempt, shard);

    DB::WriteBufferFromOwnString out;
    RunHeader header;
    header.kind = RunKind::SourceEdge;
    header.key_schema = 0;   // (blob_hash, source_id) 32-byte fixed
    RunFileWriter writer(out, header);

    // Streaming two-cursor merge over prior edges (by 32-byte key) and this round's edge deltas
    // (by (blob_hash, source_id)). All rows for one edge key are adjacent in BOTH inputs. We resolve
    // final presence per edge locally (idempotent: prior present + activate => present; any remove =>
    // absent), emit surviving edges, and accumulate the current blob's surviving-edge count on the fly
    // to emit a zero-transition marker. O(block) IO + O(1) per current blob.
    size_t pi = 0, di = 0;
    UInt128 cur_blob{0};
    bool have_blob = false;
    uint64_t cur_edges = 0;    // surviving edges of cur_blob so far
    bool cur_touched = false;  // cur_blob had prior edges or deltas this generation

    auto closeBlob = [&]()
    {
        if (have_blob && cur_edges == 0 && cur_touched)
            writer.append(srcEdgeRunKey(cur_blob, kZeroSourceId), String(1, kZeroMarker));
    };
    auto openBlobIfNeeded = [&](const UInt128 & b)
    {
        if (!have_blob || b != cur_blob)
        {
            closeBlob();
            cur_blob = b; have_blob = true; cur_edges = 0; cur_touched = false;
        }
    };

    while (pi < prior.size() || di < scattered.size())
    {
        // Pick the smallest edge key across the two cursors.
        String key;
        bool from_prior = false;
        if (pi < prior.size()) { key = prior[pi].first; from_prior = true; }
        if (di < scattered.size())
        {
            const String dk = srcEdgeRunKey(scattered[di].blob_hash, scattered[di].source_id);
            if (!from_prior || dk < key) { key = dk; from_prior = false; }
        }

        UInt128 blob_hash, source_id;
        if (!parseSrcEdgeRunKey(key, blob_hash, source_id))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: malformed key");
        openBlobIfNeeded(blob_hash);

        bool present = false;
        if (from_prior && prior[pi].first == key) { present = true; ++pi; cur_touched = true; }
        while (di < scattered.size()
               && scattered[di].blob_hash == blob_hash && scattered[di].source_id == source_id)
        {
            present = scattered[di].remove ? false : true;   // apply in order; last wins
            cur_touched = true;
            ++di;
        }

        if (present)
        {
            writer.append(key, String(1, kEdgeActive));
            ++cur_edges;
        }
    }
    closeBlob();

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
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got) break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        String k, p;
        while (r.next(k, p))
            if (!p.empty() && p[0] == kZeroMarker)
            {
                UInt128 bh, sid;
                if (parseSrcEdgeRunKey(k, bh, sid))
                    result.push_back(BlobCandidate{.hash = bh});
            }
    }
    return result;
}

int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t attempt, uint64_t shard, const UInt128 & blob_hash)
{
    int64_t count = 0;
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got) break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        r.seek(u128ToBytesBE(blob_hash));   // sparse-index skip to this blob's edges
        String k, p;
        while (r.next(k, p))
        {
            UInt128 bh, sid;
            if (!parseSrcEdgeRunKey(k, bh, sid) || bh != blob_hash) break;   // past this blob
            if (!p.empty() && p[0] == kEdgeActive) ++count;
        }
    }
    return count;
}

}
