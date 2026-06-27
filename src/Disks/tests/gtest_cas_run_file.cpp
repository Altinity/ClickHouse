#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromMemory.h>
#include <Common/Exception.h>
#include <memory>
#include <vector>
#include <string>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; extern const int LOGICAL_ERROR; }

using namespace DB::Cas;

namespace
{

/// Encode a vector of (key,payload) into a RunFile and return the bytes.
String writeRun(const std::vector<std::pair<String, String>> & recs, uint32_t block_size = kRunTargetBlockSize)
{
    DB::WriteBufferFromOwnString out;
    RunHeader h;
    h.kind = RunKind::BlobDelta;
    h.key_schema = 0;
    h.block_size = block_size;
    RunFileWriter w(out, h);
    for (const auto & [k, p] : recs)
        w.append(k, p);
    w.finish();
    return out.str();
}

std::vector<std::pair<String, String>> readRun(const String & bytes)
{
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    RunFileReader r(in);
    std::vector<std::pair<String, String>> out;
    String k, p;
    while (r.next(k, p))
        out.emplace_back(k, p);
    return out;
}

}

TEST(CasRunFile, RoundTripSingleBlock)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "1"}, {"bb", "22"}, {"cc", "333"}};
    const String bytes = writeRun(recs);
    EXPECT_EQ(readRun(bytes), recs);
}

TEST(CasRunFile, RoundTripManyBlocks)
{
    /// Force many small blocks (block_size = 32 bytes) so the footer index has > 1 entry.
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 200; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%05d", i);
        recs.emplace_back(String(k), String("v") + std::to_string(i));
    }
    const String bytes = writeRun(recs, /*block_size*/ 32);
    EXPECT_EQ(readRun(bytes), recs);
}

TEST(CasRunFile, ByteDeterminism)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "x"}, {"bb", "y"}, {"cc", "z"}};
    EXPECT_EQ(writeRun(recs, 32), writeRun(recs, 32));   /// encode twice -> identical bytes
}

TEST(CasRunFile, KeysMustBeNonDecreasing)
{
    DB::WriteBufferFromOwnString out;
    RunFileWriter w(out, RunHeader{});
    w.append("bb", "1");
    try
    {
        w.append("aa", "2");   /// out of order
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}

TEST(CasRunFile, SeekToKeyRange)
{
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 100; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%03d", i);
        recs.emplace_back(String(k), std::to_string(i));
    }
    const String bytes = writeRun(recs, /*block_size*/ 24);
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    RunFileReader r(in);
    r.seek("k050");
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, "k050");      /// first key >= "k050"
    EXPECT_EQ(p, "50");
    /// Seeking to a key between two stored keys lands on the next-greater.
    DB::ReadBufferFromMemory in2(bytes.data(), bytes.size());
    RunFileReader r2(in2);
    r2.seek("k0509");          /// no exact match; next is k051
    ASSERT_TRUE(r2.next(k, p));
    EXPECT_EQ(k, "k051");
}

TEST(CasRunFile, CorruptedPayloadFailsClosed)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "payload-bytes"}, {"bb", "more"}};
    String bytes = writeRun(recs);
    /// Flip a byte inside the first block payload (well past the header) -> crc mismatch on read.
    bytes[bytes.size() / 2] ^= 0xFF;
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    try
    {
        RunFileReader r(in);
        String k, p;
        while (r.next(k, p)) {}
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

namespace
{

/// Run `body`, assert it throws a DB::Exception with code CORRUPTED_DATA. No std::out_of_range /
/// std::length_error / UB may escape: any non-DB::Exception is an explicit test failure.
template <typename F>
void expectCorruptedData(const char * what, F && body)
{
    try
    {
        body();
        FAIL() << "expected CORRUPTED_DATA for " << what;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA) << what << ": " << e.message();
    }
    catch (const std::exception & e)
    {
        FAIL() << what << ": escaping std::exception (not fail-closed): " << e.what();
    }
}

/// Construct a reader over `bytes` and drain it fully (forces footer + every block to be parsed).
void constructAndDrain(const String & bytes)
{
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    RunFileReader r(in);
    String k, p;
    while (r.next(k, p)) {}
}

}

TEST(CasRunFile, EmptyBufferFailsClosed)
{
    expectCorruptedData("empty buffer", [] { constructAndDrain(String{}); });
}

TEST(CasRunFile, GarbageBytesFailClosed)
{
    /// Random-ish bytes with no valid "CARN" magic.
    String garbage;
    for (int i = 0; i < 257; ++i)
        garbage.push_back(static_cast<char>((i * 37 + 11) & 0xFF));
    expectCorruptedData("garbage bytes", [&] { constructAndDrain(garbage); });
}

TEST(CasRunFile, CorruptedFooterLenTrailerFailsClosed)
{
    const String valid = writeRun({{"aa", "1"}, {"bb", "22"}, {"cc", "333"}});

    /// footer_len trailer is the last 4 bytes (LE u32). Set it huge.
    {
        String b = valid;
        const size_t off = b.size() - 4;
        b[off + 0] = static_cast<char>(0xFF);
        b[off + 1] = static_cast<char>(0xFF);
        b[off + 2] = static_cast<char>(0xFF);
        b[off + 3] = static_cast<char>(0xFF);
        expectCorruptedData("huge footer_len", [&] { constructAndDrain(b); });
    }
    /// Set it tiny (smaller than the minimum footer body).
    {
        String b = valid;
        const size_t off = b.size() - 4;
        b[off + 0] = static_cast<char>(0x01);
        b[off + 1] = 0;
        b[off + 2] = 0;
        b[off + 3] = 0;
        expectCorruptedData("tiny footer_len", [&] { constructAndDrain(b); });
    }
    /// Set it to zero.
    {
        String b = valid;
        const size_t off = b.size() - 4;
        b[off + 0] = 0;
        b[off + 1] = 0;
        b[off + 2] = 0;
        b[off + 3] = 0;
        expectCorruptedData("zero footer_len", [&] { constructAndDrain(b); });
    }
}

TEST(CasRunFile, CorruptedBlockCountInFooterFailsClosed)
{
    /// Force several small blocks so the footer index is non-trivial.
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 40; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%05d", i);
        recs.emplace_back(String(k), String("v") + std::to_string(i));
    }
    const String valid = writeRun(recs, /*block_size*/ 32);

    /// footer_len is the trailing u32; footer body starts at size()-footer_len. block_count is the
    /// first u32 of the footer body. Set it absurdly large -> would over-read parsing per-block
    /// entries, must fail closed (and the footer CRC must catch the mutation regardless).
    String b = valid;
    const size_t footer_len = static_cast<uint8_t>(b[b.size() - 4])
        | (static_cast<uint32_t>(static_cast<uint8_t>(b[b.size() - 3])) << 8)
        | (static_cast<uint32_t>(static_cast<uint8_t>(b[b.size() - 2])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(b[b.size() - 1])) << 24);
    const size_t footer_start = b.size() - footer_len;
    b[footer_start + 0] = static_cast<char>(0xFF);
    b[footer_start + 1] = static_cast<char>(0xFF);
    b[footer_start + 2] = static_cast<char>(0xFF);
    b[footer_start + 3] = static_cast<char>(0x7F);
    expectCorruptedData("huge block_count", [&] { constructAndDrain(b); });
}

TEST(CasRunFile, TruncationSweepFailsClosed)
{
    /// A multi-block run; truncate at many lengths past the header and require every prefix to
    /// fail closed (never crash, never throw std::out_of_range).
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 60; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%05d", i);
        recs.emplace_back(String(k), String("payload-") + std::to_string(i));
    }
    const String valid = writeRun(recs, /*block_size*/ 32);
    constexpr size_t header_len = 13;
    for (size_t k = header_len + 1; k < valid.size(); ++k)
        expectCorruptedData("truncated prefix", [&] { constructAndDrain(valid.substr(0, k)); });
}

TEST(CasRunFile, MergeTwoDisjointRuns)
{
    const String a = writeRun({{"a", "1"}, {"c", "3"}, {"e", "5"}});
    const String b = writeRun({{"b", "2"}, {"d", "4"}, {"f", "6"}});
    DB::ReadBufferFromMemory ia(a.data(), a.size());
    DB::ReadBufferFromMemory ib(b.data(), b.size());
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(ia));
    rs.push_back(std::make_unique<RunFileReader>(ib));
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    std::vector<String> seen;
    while (m.next(k, vs))
    {
        EXPECT_EQ(vs.size(), 1u);
        seen.push_back(k);
    }
    EXPECT_EQ(seen, (std::vector<String>{"a", "b", "c", "d", "e", "f"}));
}

TEST(CasRunFile, MergeCoalescesSameKeyAcrossRuns)
{
    /// Same key "k" present in both runs -> one merged key with both payloads.
    const String a = writeRun({{"k", "from-a"}, {"z", "9"}});
    const String b = writeRun({{"k", "from-b"}});
    DB::ReadBufferFromMemory ia(a.data(), a.size());
    DB::ReadBufferFromMemory ib(b.data(), b.size());
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(ia));
    rs.push_back(std::make_unique<RunFileReader>(ib));
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    ASSERT_TRUE(m.next(k, vs));
    EXPECT_EQ(k, "k");
    ASSERT_EQ(vs.size(), 2u);
    /// Payloads come in reader order: run a first, then run b.
    EXPECT_EQ(vs[0], "from-a");
    EXPECT_EQ(vs[1], "from-b");
    ASSERT_TRUE(m.next(k, vs));
    EXPECT_EQ(k, "z");
    EXPECT_FALSE(m.next(k, vs));
}

TEST(CasRunFile, MergeEmptyInput)
{
    std::vector<std::unique_ptr<RunFileReader>> rs;   /// no readers
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    EXPECT_FALSE(m.next(k, vs));
}
