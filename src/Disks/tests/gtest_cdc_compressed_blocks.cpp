#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasClickHouseCompressedBlocks.h>
#include <Storages/MergeTree/CdcCompressedWriteBuffer.h>
#include <Compression/CompressedReadBufferFromFile.h>
#include <Compression/CompressionFactory.h>
#include <IO/HashingWriteBuffer.h>
#include <IO/ReadBufferFromFile.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromVector.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace DB;

namespace
{

Cas::ChunkerParams testParams()
{
    Cas::ChunkerParams params;
    params.min_bytes = 64 * 1024;
    params.avg_bytes = 128 * 1024;
    params.max_bytes = 256 * 1024;
    params.validate();
    return params;
}

std::string compressCdc(const std::string & uncompressed)
{
    auto codec = CompressionCodecFactory::instance().getDefaultCodec();
    std::vector<char> out_bytes;
    WriteBufferFromVector<std::vector<char>> out(out_bytes);
    {
        CdcCompressedWriteBuffer compressor(out, codec, testParams(), 64 * 1024, false, 64 * 1024);
        compressor.write(uncompressed.data(), uncompressed.size());
        compressor.finalize();
    }
    out.finalize();
    return std::string(out_bytes.begin(), out_bytes.end());
}

std::vector<std::string> splitBlocks(const std::string & compressed)
{
    std::vector<std::string> blocks;
    std::string_view rest(compressed);
    while (!rest.empty())
    {
        size_t n = 0;
        if (!Cas::tryPeekClickHouseCompressedBlockSize(rest, n) || rest.size() < n)
            break;
        blocks.emplace_back(rest.substr(0, n));
        rest.remove_prefix(n);
    }
    EXPECT_TRUE(rest.empty()) << "trailing unparsed compressed bytes";
    return blocks;
}

std::string makePayload(size_t n, UInt64 seed)
{
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>(seed + i * 17);
    return s;
}

struct RecordedMark
{
    size_t offset_in_compressed_file = 0;
    size_t offset_in_decompressed_block = 0;
    size_t uncompressed_pos = 0;
};

}

TEST(CdcCompressedWriteBuffer, ConcatenativePrefixBlocksMatch)
{
    const auto a = makePayload(400 * 1024, 1);
    const auto b = makePayload(400 * 1024, 99);
    const auto left = compressCdc(a);
    const auto both = compressCdc(a + b);
    const auto left_blocks = splitBlocks(left);
    const auto both_blocks = splitBlocks(both);
    ASSERT_FALSE(left_blocks.empty());
    ASSERT_GT(both_blocks.size(), left_blocks.size());
    size_t shared = 0;
    for (size_t i = 0; i < left_blocks.size() && i < both_blocks.size(); ++i)
        shared += static_cast<size_t>(left_blocks[i] == both_blocks[i]);
    EXPECT_GE(shared, left_blocks.size() - 1u)
        << "concatenative CDC compression should reuse all but the stitch block; shared="
        << shared << " left=" << left_blocks.size();
}

TEST(CdcCompressedWriteBuffer, MarksSeekIntoAccumulatedBlock)
{
    const auto payload = makePayload(400 * 1024, 7);
    const auto path = std::filesystem::temp_directory_path()
        / ("gtest_cdc_marks_" + std::to_string(getpid()) + ".bin");
    std::filesystem::remove(path);

    std::vector<RecordedMark> marks;
    {
        WriteBufferFromFile file_out(path.string());
        auto codec = CompressionCodecFactory::instance().getDefaultCodec();
        CdcCompressedWriteBuffer compressor(file_out, codec, testParams(), 64 * 1024, false, 64 * 1024);
        HashingWriteBuffer hashing(compressor);

        constexpr size_t step = 32 * 1024;
        for (size_t i = 0; i < payload.size(); i += step)
        {
            RecordedMark mark;
            mark.offset_in_compressed_file = file_out.count();
            mark.offset_in_decompressed_block = compressor.offsetInCurrentCompressedBlock();
            mark.uncompressed_pos = i;
            marks.push_back(mark);

            const size_t n = std::min(step, payload.size() - i);
            hashing.write(payload.data() + i, n);
        }

        hashing.finalize();
        compressor.finalize();
        file_out.finalize();
    }

    ASSERT_GE(marks.size(), 4u);

    auto file_in = std::make_unique<ReadBufferFromFile>(path.string());
    CompressedReadBufferFromFile reader(std::move(file_in));
    for (const auto & mark : marks)
    {
        reader.seek(mark.offset_in_compressed_file, mark.offset_in_decompressed_block);
        const size_t n = std::min<size_t>(16, payload.size() - mark.uncompressed_pos);
        std::string got(n, '\0');
        reader.readStrict(got.data(), n);
        EXPECT_EQ(got, payload.substr(mark.uncompressed_pos, n))
            << "mark uncompressed_pos=" << mark.uncompressed_pos
            << " compressed=" << mark.offset_in_compressed_file
            << " decompressed=" << mark.offset_in_decompressed_block;
    }

    std::filesystem::remove(path);
}
