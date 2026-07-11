#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <memory>
#include <vector>
#include <string>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; extern const int LOGICAL_ERROR; }

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;

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

/// Read a run in borrowed-memory mode (zero-copy over the caller's bytes).
std::vector<std::pair<String, String>> readRun(const String & bytes)
{
    RunFileReader r{std::string_view(bytes)};
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
    RunFileReader r{std::string_view(bytes)};
    r.seek("k050");
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, "k050");      /// first key >= "k050"
    EXPECT_EQ(p, "50");
    /// Seeking to a key between two stored keys lands on the next-greater.
    RunFileReader r2{std::string_view(bytes)};
    r2.seek("k0509");          /// no exact match; next is k051
    ASSERT_TRUE(r2.next(k, p));
    EXPECT_EQ(k, "k051");
}

TEST(CasRunFile, SeekLandsOnFirstOfDuplicateKeySpanningBlocks)
{
    /// The writer permits EQUAL keys (non-decreasing append order), so an exact key may legitimately
    /// span a block boundary. `seek`'s contract is "position at the FIRST record with key >= target";
    /// picking the LAST block whose min_key <= target lands past the earlier duplicates and silently
    /// skips them (2026-07-11 Phase 3 design consult finding — latent today, since every production
    /// seek uses a strict PREFIX of the stored keys, which no full key can equal).
    /// block_size=1 makes every record exceed the target, sealing one record per block:
    /// blocks [aa] [kk] [kk] [kk] [zz] — the duplicate "kk" spans two block boundaries.
    const std::vector<std::pair<String, String>> recs =
        {{"aa", "0"}, {"kk", "1"}, {"kk", "2"}, {"kk", "3"}, {"zz", "4"}};
    const String bytes = writeRun(recs, /*block_size=*/1);

    RunFileReader r{std::string_view(bytes)};
    r.seek("kk");
    std::vector<std::pair<String, String>> got;
    String k, p;
    while (r.next(k, p))
        got.emplace_back(k, p);
    const std::vector<std::pair<String, String>> expected =
        {{"kk", "1"}, {"kk", "2"}, {"kk", "3"}, {"zz", "4"}};
    EXPECT_EQ(got, expected);
}

TEST(CasRunFile, SeekPastEveryKeyIsExhausted)
{
    /// A target greater than every stored key positions at the end: next() yields nothing.
    const std::vector<std::pair<String, String>> recs = {{"aa", "0"}, {"bb", "1"}, {"cc", "2"}};
    const String bytes = writeRun(recs, /*block_size=*/1);
    RunFileReader r{std::string_view(bytes)};
    r.seek("zz");
    String k, p;
    EXPECT_FALSE(r.next(k, p));
}

TEST(CasRunFile, CorruptedPayloadFailsClosed)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "payload-bytes"}, {"bb", "more"}};
    String bytes = writeRun(recs);
    /// Flip a byte inside the first block payload (well past the header) -> crc mismatch on read.
    bytes[bytes.size() / 2] ^= 0xFF;
    try
    {
        RunFileReader r{std::string_view(bytes)};
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
    RunFileReader r{std::string_view(bytes)};
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
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(std::string_view(a)));
    rs.push_back(std::make_unique<RunFileReader>(std::string_view(b)));
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
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(std::string_view(a)));
    rs.push_back(std::make_unique<RunFileReader>(std::string_view(b)));
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

/// ---- streaming mode (Backend seam) ----

namespace
{

/// Build a run that spans several blocks (small block_size) and store it under `key` in `backend`.
/// Returns the (key, payload) records that were written, for equivalence checks.
std::vector<std::pair<String, String>> buildMultiBlockRunInBackend(
    CountingBackend & backend, const String & key, int n_records, uint32_t block_size)
{
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < n_records; ++i)
    {
        char k[16];
        std::snprintf(k, sizeof(k), "k%06d", i);
        recs.emplace_back(String(k), String("payload-value-") + std::to_string(i));
    }
    const String bytes = writeRun(recs, block_size);
    backend.putIfAbsent(key, bytes);
    return recs;
}

}

TEST(CasRunFileStreaming, MultiBlockStreamMatchesBorrowed)
{
    auto backend = std::make_shared<CountingBackend>();
    const String key = "runs/multi";
    /// block_size = 4096 with ~2000 records forces many blocks.
    const auto recs = buildMultiBlockRunInBackend(*backend, key, 2000, 4096);

    /// Borrowed-mode oracle over the materialized bytes.
    const String materialized = backend->get(key)->bytes;
    const auto oracle = readRun(materialized);
    EXPECT_EQ(oracle, recs);

    /// Streaming read yields the identical record sequence.
    RunFileReader r(*backend, key);
    std::vector<std::pair<String, String>> streamed;
    String k, p;
    while (r.next(k, p))
        streamed.emplace_back(k, p);
    EXPECT_EQ(streamed, recs);
    EXPECT_EQ(r.kind(), RunKind::BlobDelta);
}

TEST(CasRunFileStreaming, SeekUsesOneRangedGet)
{
    auto backend = std::make_shared<CountingBackend>();
    const String key = "runs/seekme";
    const auto recs = buildMultiBlockRunInBackend(*backend, key, 2000, 4096);

    backend->resetCounts();
    RunFileReader r(*backend, key);
    /// Open profile: head=1, tail get=1, body getStream=1.
    ASSERT_EQ(backend->headCount(key), 1u);
    ASSERT_EQ(backend->getCount(key), 1u);
    ASSERT_EQ(backend->getStreamCount(key), 1u);

    const uint64_t gets_before_seek = backend->getCount(key);
    r.seek("k001000");
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, "k001000");
    /// The seek touched exactly one block => exactly one extra ranged get.
    EXPECT_EQ(backend->getCount(key), gets_before_seek + 1);
    EXPECT_EQ(backend->getStreamCount(key), 1u);   /// no new stream opened
}

TEST(CasRunFileStreaming, OpenIsThreeRequestsAndBlockBoundedRanges)
{
    auto backend = std::make_shared<CountingBackend>();
    const String key = "runs/profile";
    buildMultiBlockRunInBackend(*backend, key, 3000, 4096);

    backend->resetCounts();
    {
        RunFileReader r(*backend, key);
        String k, p;
        while (r.next(k, p)) {}   /// drain via the stream (no seeks => no ranged gets)
    }
    /// Exactly head=1, tail get=1, body getStream=1.
    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->getCount(key), 1u);
    EXPECT_EQ(backend->getStreamCount(key), 1u);
    /// No whole-object get of the run key.
    EXPECT_EQ(backend->wholeGetCount(key), 0u);
    /// Every ranged-get window <= kRunHardCapBlockSize + 64KB (the resident-memory bound at the seam).
    EXPECT_LE(backend->maxRangedGetLen(key), static_cast<uint64_t>(kRunHardCapBlockSize) + 64u * 1024u);
}

TEST(CasRunFileStreaming, TruncatedOrCorruptFailsClosed)
{
    /// Truncation mid-block: overwrite the object with a prefix that cuts the body.
    {
        auto backend = std::make_shared<CountingBackend>();
        const String key = "runs/trunc";
        buildMultiBlockRunInBackend(*backend, key, 2000, 4096);
        const String full = backend->get(key)->bytes;
        /// Chop the object in half (well past the header, mid-body). putOverwrite against the token.
        const auto h = backend->head(key);
        backend->putOverwrite(key, full.substr(0, full.size() / 2), h.token);
        expectCorruptedData("truncated stream", [&]
        {
            RunFileReader r(*backend, key);
            String k, p;
            while (r.next(k, p)) {}
        });
    }
    /// Payload-byte flip: block CRC mismatch surfaces on the next() that reaches the damage.
    {
        auto backend = std::make_shared<CountingBackend>();
        const String key = "runs/badblock";
        buildMultiBlockRunInBackend(*backend, key, 2000, 4096);
        String full = backend->get(key)->bytes;
        full[100] ^= 0xFF;   /// inside the first block body
        const auto h = backend->head(key);
        backend->putOverwrite(key, full, h.token);
        expectCorruptedData("block crc", [&]
        {
            RunFileReader r(*backend, key);
            String k, p;
            while (r.next(k, p)) {}
        });
    }
    /// Footer-byte flip: footer CRC mismatch fails on construction.
    {
        auto backend = std::make_shared<CountingBackend>();
        const String key = "runs/badfooter";
        buildMultiBlockRunInBackend(*backend, key, 2000, 4096);
        String full = backend->get(key)->bytes;
        full[full.size() - 8] ^= 0xFF;   /// inside the footer body, before the trailer
        const auto h = backend->head(key);
        backend->putOverwrite(key, full, h.token);
        expectCorruptedData("footer crc", [&]
        {
            RunFileReader r(*backend, key);
            String k, p;
            while (r.next(k, p)) {}
        });
    }
    /// Absent key => CORRUPTED_DATA on construction.
    {
        auto backend = std::make_shared<CountingBackend>();
        expectCorruptedData("absent run", [&]
        {
            RunFileReader r(*backend, "runs/does-not-exist");
        });
    }
}

TEST(CasRunFileStreaming, LargeFooterBeyondTailProbeReadsExactWindow)
{
    /// A footer larger than the fixed tail probe (kRunHardCapBlockSize + allowance) must trigger ONE
    /// extra ranged get of the exact footer window — large multi-GB runs are the design case, not a
    /// pathology. Long boundary keys inflate the per-block index entry so ~600 blocks overflow the
    /// probe without writing gigabytes.
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const String key = "p/big_footer_run";
    {
        String out_bytes;
        DB::WriteBufferFromString out(out_bytes);
        RunHeader header;
        header.kind = RunKind::SourceEdge;
        header.block_size = 4096;
        RunFileWriter writer(out, header);
        String big_key(1024, 'k');
        for (int i = 0; i < 1800; ++i)
        {
            /// Ascending keys: vary a suffix (big_key base keeps footer entries ~2KB each).
            String k = big_key + fmt::format("{:08}", i);
            writer.append(k, "p");
        }
        writer.finish();
        out.finalize();
        backend->putIfAbsent(key, out_bytes);
    }

    backend->resetCounts();
    RunFileReader r(*backend, key);
    String k, p;
    size_t n = 0;
    while (r.next(k, p))
        ++n;
    EXPECT_EQ(n, 1800u);
    /// Open profile for a large-footer run: head + tail probe + exact footer window + body stream.
    EXPECT_EQ(backend->headCount(key), 1u);
    EXPECT_EQ(backend->getCount(key), 2u);
    EXPECT_EQ(backend->getStreamCount(key), 1u);
}

TEST(CasRunFile, MixedWidthKeysAcrossBlockBoundary)      /// spec §9.10 — Phase 3 T3
{
    /// tiny blocks: one record per block; a 33->49-byte width transition lands exactly on a block
    /// boundary; seek by both prefixes; absent prefix positions on the next greater key.
    const String k1(33, 'a'), k2(33, 'b'), k3(49, 'c'), k4(49, 'd');
    const String bytes = writeRun({{k1, "1"}, {k2, "2"}, {k3, "3"}, {k4, "4"}}, /*block_size*/ 1);
    RunFileReader r{std::string_view(bytes)};
    r.seek(k3.substr(0, 17));                            /// a 17-byte prefix of the 49-byte key
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, k3);
}
