#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <Storages/ObjectStorage/DataLakes/PuffinDeletionVectorReader.h>
#include <roaring/roaring.hh>
#include <zlib.h>

#include <string>
#include <string_view>

using namespace DB;

namespace DB
{
namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
}
}

namespace
{

/// deletion-vector-v1 blob for positions {2, 5} (cardinality 2).
constexpr UInt8 two_position_dv_blob[] = {
    0x00, 0x00, 0x00, 0x24, 0xD1, 0xD3, 0x39, 0x64, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x3A, 0x30, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x05, 0x00, 0x2C, 0xDB, 0x9F, 0xC1,
};

constexpr Int64 large_declared_length = 64 * 1024 * 1024;

void appendLittleEndian(std::string & out, UInt64 value, size_t width)
{
    for (size_t i = 0; i < width; ++i)
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void appendBigEndian32(std::string & out, UInt32 value)
{
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

/// deletion-vector-v1 envelope around one portable roaring bitmap at key 0.
std::string makeDeletionVectorBlob(const roaring::Roaring & bitmap)
{
    const size_t roaring_size = bitmap.getSizeInBytes(/*portable=*/true);
    std::string roaring_bytes(roaring_size, '\0');
    const size_t written = bitmap.write(roaring_bytes.data(), /*portable=*/true);
    EXPECT_EQ(written, roaring_size);

    std::string payload;
    appendLittleEndian(payload, /*bitmap_count=*/1, sizeof(UInt64));
    appendLittleEndian(payload, /*key=*/0, sizeof(UInt32));
    payload.append(roaring_bytes);

    std::string crc_input;
    crc_input.append(reinterpret_cast<const char *>(DELETION_VECTOR_MAGIC), sizeof(DELETION_VECTOR_MAGIC));
    crc_input.append(payload);
    const auto crc = static_cast<UInt32>(crc32_z(0L, reinterpret_cast<const unsigned char *>(crc_input.data()), crc_input.size()));

    std::string blob;
    appendBigEndian32(blob, static_cast<UInt32>(crc_input.size()));
    blob.append(crc_input);
    appendBigEndian32(blob, crc);
    return blob;
}

}

TEST(PuffinDeletionVectorCardinality, RejectsCardinalityAboveMaterializationLimitBeforeParse)
{
    /// Ceiling is checked before payload validation, so even an empty blob must fail closed.
    try
    {
        deserializeDeletionVectorV1Blob(std::string_view{}, PUFFIN_DV_MAX_MATERIALIZED_POSITIONS + 1);
        FAIL() << "Expected exception";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(e.message().find("exceeds materialization limit"), std::string::npos);
    }
}

TEST(PuffinDeletionVectorCardinality, RejectsCardinalityAboveLimitBeforeFullAllocate)
{
    /// Huge declared length with only a tiny buffer: without an early ceiling check this would
    /// allocate `large_declared_length` (or fail mid-read after that allocate). ReadBufferFromMemory
    /// does not expose file size, so bounds checks alone do not stop this.
    /// The SQL `Puffin` format applies the same early check in `readDeletionVectorBlobBytes`.
    const char header[8] = {};
    ReadBufferFromMemory file(header, sizeof(header));

    try
    {
        readDeletionVectorFromPuffin(
            file, 0, large_declared_length, PUFFIN_DV_MAX_MATERIALIZED_POSITIONS + 1);
        FAIL() << "Expected exception";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(e.message().find("exceeds materialization limit"), std::string::npos);
    }
}

TEST(PuffinDeletionVectorCardinality, RejectsBitmapExceedingDeclaredCardinality)
{
    const std::string_view blob(
        reinterpret_cast<const char *>(two_position_dv_blob), sizeof(two_position_dv_blob));

    try
    {
        deserializeDeletionVectorV1Blob(blob, /*expected_cardinality=*/1);
        FAIL() << "Expected exception";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(e.message().find("exceeds declared cardinality"), std::string::npos);
    }
}

TEST(PuffinDeletionVectorCardinality, AcceptsMatchingCardinality)
{
    const std::string_view blob(
        reinterpret_cast<const char *>(two_position_dv_blob), sizeof(two_position_dv_blob));

    const auto positions = deserializeDeletionVectorV1Blob(blob, /*expected_cardinality=*/2);
    ASSERT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions[0], 2u);
    EXPECT_EQ(positions[1], 5u);
}

TEST(PuffinDeletionVectorCardinality, AcceptsOmittedCardinalityBelowMaterializationLimit)
{
    const std::string_view blob(
        reinterpret_cast<const char *>(two_position_dv_blob), sizeof(two_position_dv_blob));

    const auto positions = deserializeDeletionVectorV1Blob(blob);
    ASSERT_EQ(positions.size(), 2u);
    EXPECT_EQ(positions[0], 2u);
    EXPECT_EQ(positions[1], 5u);
}

TEST(PuffinDeletionVectorCardinality, RejectsOmittedCardinalityAboveMaterializationLimit)
{
    /// A run-length bitmap stays small on disk and expands only when positions are materialized.
    /// The ceiling must reject it before that expansion, even with no declared cardinality.
    roaring::Roaring bitmap;
    bitmap.addRange(0, PUFFIN_DV_MAX_MATERIALIZED_POSITIONS + 1);
    const std::string blob = makeDeletionVectorBlob(bitmap);

    try
    {
        deserializeDeletionVectorV1Blob(blob);
        FAIL() << "Expected exception";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(e.message().find("exceeds materialization limit"), std::string::npos);
    }
}
