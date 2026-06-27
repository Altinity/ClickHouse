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
