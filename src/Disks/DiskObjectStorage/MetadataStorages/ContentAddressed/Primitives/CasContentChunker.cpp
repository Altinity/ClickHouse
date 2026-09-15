#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasContentChunker.h>
#include <Common/Exception.h>
#include <algorithm>
#include <array>
#include <bit>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

namespace
{

/// The per-byte gear values. Derived by `constexpr` splitmix64 from a pinned seed rather than written
/// out as 256 literals: the table must be bit-identical on every platform and build (two replicas
/// that disagree on boundaries deduplicate nothing), and a generator that the compiler evaluates is
/// easier to audit for that property than a wall of constants. The seed is part of the on-disk
/// bargain only in the sense that changing it changes future cut positions; it can never invalidate
/// an existing manifest, which lists its chunks explicitly.
constexpr std::array<uint64_t, 256> makeGearTable()
{
    std::array<uint64_t, 256> table{};
    uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (size_t i = 0; i < table.size(); ++i)
    {
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        table[i] = z ^ (z >> 31);
    }
    return table;
}

constexpr std::array<uint64_t, 256> kGearTable = makeGearTable();

}

uint32_t ChunkerParams::cutBits() const
{
    /// `bit_width(2^22) == 23`, so subtract one to make an `avg_bytes` of 2^22 ask for 22 zero bits
    /// and therefore cut with probability 2^-22. A non-power-of-two `avg_bytes` rounds down to the
    /// enclosing power of two, which is why the setting is documented as a target rather than exact.
    if (avg_bytes == 0)
        return 0;
    return static_cast<uint32_t>(std::bit_width(avg_bytes) - 1);
}

void ChunkerParams::validate() const
{
    if (min_bytes == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CAS chunker: min_bytes must be greater than zero");
    if (min_bytes > avg_bytes)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS chunker: min_bytes ({}) must not exceed avg_bytes ({})", min_bytes, avg_bytes);
    if (avg_bytes > max_bytes)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS chunker: avg_bytes ({}) must not exceed max_bytes ({})", avg_bytes, max_bytes);

    const uint32_t bits = cutBits();
    if (bits < 1 || bits > 63)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS chunker: avg_bytes ({}) yields {} cut bits, which is outside the supported range [1, 63]",
            avg_bytes, bits);
}

ContentChunker::ContentChunker(ChunkerParams params_)
    : params(params_)
    , cut_bits(params_.cutBits())
{
    params.validate();
}

void ContentChunker::startChunk()
{
    hash = 0;
    chunk_len = 0;
}

ChunkerFeedResult ContentChunker::feed(std::string_view data)
{
    /// `min_bytes` bytes are always taken without testing the hash, so skip the per-byte boundary
    /// test entirely while the current chunk is still below the floor. This is the bulk of a large
    /// file (with the defaults, the first 1 MiB of every 4 MiB) and the hash still has to absorb
    /// those bytes, so only the two comparisons are skipped, not the rolling update.
    size_t i = 0;
    if (chunk_len < params.min_bytes)
    {
        const uint64_t to_floor = params.min_bytes - chunk_len;
        const size_t bulk = std::min(static_cast<size_t>(to_floor), data.size());
        for (; i < bulk; ++i)
            hash = (hash << 1) + kGearTable[static_cast<unsigned char>(data[i])];
        chunk_len += bulk;
        /// `min_bytes <= max_bytes`, so reaching the floor can only reach the ceiling when the two are
        /// equal -- fixed-size chunking. Cut exactly on it here; letting the loop below discover it
        /// would first take one more byte and overshoot `max_bytes`.
        if (chunk_len >= params.max_bytes)
            return {i, true};
        if (i == data.size())
            return {data.size(), false};
    }

    for (; i < data.size(); ++i)
    {
        hash = (hash << 1) + kGearTable[static_cast<unsigned char>(data[i])];
        ++chunk_len;

        /// The forced cut is tested first so `max_bytes` is an unconditional ceiling.
        if (chunk_len >= params.max_bytes)
            return {i + 1, true};
        if ((hash >> (64 - cut_bits)) == 0)
            return {i + 1, true};
    }
    return {data.size(), false};
}

}
