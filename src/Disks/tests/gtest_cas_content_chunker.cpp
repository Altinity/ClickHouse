#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasContentChunker.h>
#include <Common/Exception.h>

#include <cstring>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace DB::Cas;

namespace
{

/// Drive the chunker across a whole buffer, feeding it in `feed_span`-sized slices, and return the
/// resulting chunk sizes. Slicing is a parameter because the production caller's slice size is
/// whatever the write buffer hands it, and the boundaries must not depend on it.
std::vector<uint64_t> split(std::string_view data, ChunkerParams params, size_t feed_span)
{
    ContentChunker chunker(params);
    chunker.startChunk();

    std::vector<uint64_t> sizes;
    uint64_t current = 0;
    size_t offset = 0;
    while (offset < data.size())
    {
        const size_t span = std::min(feed_span, data.size() - offset);
        const ChunkerFeedResult res = chunker.feed(data.substr(offset, span));
        current += res.taken;
        offset += res.taken;
        if (res.boundary)
        {
            sizes.push_back(current);
            current = 0;
            chunker.startChunk();
        }
    }
    if (current != 0)
        sizes.push_back(current);
    return sizes;
}

/// Deterministic pseudo-random bytes: the realistic shape of a compressed column file, where LZ4 has
/// already removed the redundancy that would make chunking trivially easy.
std::string randomBytes(size_t n, uint64_t seed)
{
    std::mt19937_64 rng(seed);
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i += 8)
    {
        const uint64_t v = rng();
        std::memcpy(s.data() + i, &v, std::min<size_t>(8, n - i));
    }
    return s;
}

/// Absolute end offsets of each chunk, which is what has to be preserved for dedup to fire.
std::set<uint64_t> cutOffsets(const std::vector<uint64_t> & sizes)
{
    std::set<uint64_t> offsets;
    uint64_t acc = 0;
    for (uint64_t s : sizes)
    {
        acc += s;
        offsets.insert(acc);
    }
    return offsets;
}

}

TEST(CasContentChunker, CutBitsFollowAverageSize)
{
    EXPECT_EQ((ChunkerParams{1 << 20, 4 << 20, 16 << 20}).cutBits(), 22u);
    EXPECT_EQ((ChunkerParams{1 << 20, 1 << 20, 16 << 20}).cutBits(), 20u);
    EXPECT_EQ((ChunkerParams{512, 1 << 24, 1 << 25}).cutBits(), 24u);
}

TEST(CasContentChunker, RejectsInvalidParameters)
{
    EXPECT_THROW((ChunkerParams{0, 4 << 20, 16 << 20}).validate(), DB::Exception);
    /// min above avg
    EXPECT_THROW((ChunkerParams{8 << 20, 4 << 20, 16 << 20}).validate(), DB::Exception);
    /// avg above max
    EXPECT_THROW((ChunkerParams{1 << 20, 32 << 20, 16 << 20}).validate(), DB::Exception);
    /// avg of 1 byte leaves zero cut bits
    EXPECT_THROW((ChunkerParams{1, 1, 16}).validate(), DB::Exception);
    EXPECT_NO_THROW((ChunkerParams{1 << 20, 4 << 20, 16 << 20}).validate());
}

TEST(CasContentChunker, ChunksCoverTheFileExactly)
{
    const ChunkerParams params;
    const std::string data = randomBytes(64u << 20, 7);
    const auto sizes = split(data, params, 1u << 16);

    ASSERT_GT(sizes.size(), 1u);
    EXPECT_EQ(std::accumulate(sizes.begin(), sizes.end(), uint64_t(0)), data.size());
}

TEST(CasContentChunker, InteriorChunksRespectMinAndMax)
{
    const ChunkerParams params;
    const std::string data = randomBytes(64u << 20, 11);
    const auto sizes = split(data, params, 1u << 16);

    /// Only the final chunk may fall below the floor: it ends where the file ends.
    for (size_t i = 0; i + 1 < sizes.size(); ++i)
    {
        EXPECT_GE(sizes[i], params.min_bytes) << "chunk " << i;
        EXPECT_LE(sizes[i], params.max_bytes) << "chunk " << i;
    }
    EXPECT_LE(sizes.back(), params.max_bytes);
}

TEST(CasContentChunker, MeanChunkSizeTracksTheTarget)
{
    const ChunkerParams params;
    const std::string data = randomBytes(128u << 20, 13);
    const auto sizes = split(data, params, 1u << 16);

    const double mean = double(data.size()) / double(sizes.size());
    /// The cut predicate has period `avg_bytes`, but no boundary is accepted below `min_bytes`, so
    /// the expected chunk is about `min_bytes + avg_bytes` (~5 MB with the defaults) rather than
    /// `avg_bytes`. The hash gives a geometric distribution on top of that, and `max_bytes` truncates
    /// the tail, so bound this loosely: the assertion exists to catch a mask or shift error that
    /// would move the mean by an order of magnitude, not to pin the distribution.
    EXPECT_GT(mean, 2.0e6);
    EXPECT_LT(mean, 1.2e7);
}

/// The property the `ChunkerFeedResult::boundary` flag exists to guarantee. Write-buffer sizes vary
/// with adaptive sizing and per-column settings, so if boundaries depended on the caller's slicing,
/// two replicas writing identical bytes would cut differently and deduplicate nothing.
TEST(CasContentChunker, BoundariesAreIndependentOfFeedSliceSize)
{
    const ChunkerParams params;
    const std::string data = randomBytes(32u << 20, 17);
    const auto reference = split(data, params, 1u << 16);

    for (size_t span : {size_t(1), size_t(7), size_t(4096), size_t(1u << 20), data.size()})
        EXPECT_EQ(split(data, params, span), reference) << "feed span " << span;
}

/// The property that makes chunking worth doing at all: a merge that re-emits the same bytes at a
/// shifted offset must reuse the chunks already in the pool. Whole-file hashing cannot.
TEST(CasContentChunker, BoundariesResyncAfterAByteShift)
{
    const ChunkerParams params;
    const std::string data = randomBytes(64u << 20, 19);
    const auto base = split(data, params, 1u << 16);

    constexpr uint64_t shift = 5000;
    const std::string shifted = randomBytes(shift, 23) + data;
    const auto moved = split(shifted, params, 1u << 16);

    const auto base_offsets = cutOffsets(base);
    std::set<uint64_t> moved_offsets;
    for (uint64_t v : cutOffsets(moved))
        if (v >= shift)
            moved_offsets.insert(v - shift);

    size_t shared = 0;
    for (uint64_t v : base_offsets)
        if (moved_offsets.contains(v))
            ++shared;

    /// Re-alignment is immediate, not gradual, because `startChunk` resets the rolling hash: a
    /// chunk's cut position is a function of its own bytes only, so shifting everything by a prefix
    /// does not move any boundary. Measured at 100% for shifts of 1 byte to 1 MB across many inputs;
    /// one lost boundary is tolerated so a future parameter change does not fail spuriously.
    EXPECT_GE(shared + 1, base_offsets.size());
}

TEST(CasContentChunker, RepetitiveContentIsBoundedByMaxBytes)
{
    const ChunkerParams params;
    /// A constant byte stream never satisfies the hash predicate, so only `max_bytes` terminates a
    /// chunk. Without that ceiling one chunk would swallow the whole file.
    const std::string repeated(40u << 20, 'x');
    const auto sizes = split(repeated, params, 1u << 16);

    ASSERT_FALSE(sizes.empty());
    for (size_t i = 0; i + 1 < sizes.size(); ++i)
        EXPECT_EQ(sizes[i], params.max_bytes) << "chunk " << i;
}

TEST(CasContentChunker, MinEqualToMaxGivesExactFixedSizeChunks)
{
    /// The degenerate configuration must not overshoot the ceiling by the one byte it takes to
    /// discover it.
    const ChunkerParams params{1u << 20, 1u << 20, 1u << 20};
    const std::string data = randomBytes(10u << 20, 29);
    const auto sizes = split(data, params, 7777);

    ASSERT_EQ(sizes.size(), 10u);
    for (uint64_t s : sizes)
        EXPECT_EQ(s, 1u << 20);
}

TEST(CasContentChunker, EmptyAndSubMinimumInputs)
{
    const ChunkerParams params;
    EXPECT_TRUE(split("", params, 4096).empty());

    const std::string tiny(100, 'a');
    const auto sizes = split(tiny, params, 4096);
    ASSERT_EQ(sizes.size(), 1u);
    EXPECT_EQ(sizes[0], 100u);
}

/// The gear table is generated from a pinned seed by `constexpr` arithmetic. Pin a few realised cut
/// positions so an accidental change to the seed, the generator, or the hash update is caught here
/// rather than silently halving the dedup rate of every pool in the field.
TEST(CasContentChunker, CutPositionsArePinned)
{
    const ChunkerParams params;
    const std::string data = randomBytes(64u << 20, 31);
    const auto sizes = split(data, params, 1u << 16);

    const auto offsets = cutOffsets(sizes);
    /// Recorded from this implementation. A mismatch means boundaries moved, which halves the dedup
    /// rate against every pool already written and is therefore a change to announce deliberately
    /// rather than discover in production.
    ASSERT_FALSE(offsets.empty());
    EXPECT_EQ(*offsets.begin(), 14790740u);
    EXPECT_EQ(sizes.size(), 10u);
}
