#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <crc32c/crc32c.h>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

using namespace DB::Cas;

namespace
{

PartManifest sample()
{
    PartManifest m;
    m.ref = ManifestRef{7, 1042, 1};
    m.root_namespace_id = RootNamespace("srv-a/uuid@cas@");
    m.payload_digest = UInt128(0xDEAD);
    ManifestEntry blob;
    blob.path = "columns/data.bin";
    blob.placement = EntryPlacement::Blob;
    blob.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128((UInt128(1) << 64) | UInt128(2))};
    blob.blob_size = 4096;
    ManifestEntry inl;
    inl.path = "checksums.txt";
    inl.placement = EntryPlacement::Inline;
    inl.inline_bytes = "hello-inline";
    m.entries = {blob, inl};   /// deliberately NOT path-sorted on input
    return m;
}

}

TEST(CasManifestCodec, RoundTrip)
{
    const PartManifest m = sample();
    const String bytes = encodePartManifest(m);
    const PartManifest got = decodePartManifest(bytes);
    EXPECT_EQ(got.ref, m.ref);
    EXPECT_EQ(got.root_namespace_id, m.root_namespace_id);
    EXPECT_EQ(got.payload_digest, m.payload_digest);
    ASSERT_EQ(got.entries.size(), 2u);
}

TEST(CasManifestCodec, EntriesInCanonicalPathOrder)
{
    const PartManifest m = sample();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    /// "checksums.txt" < "columns/data.bin" byte-wise -> inline entry comes first after sorting.
    EXPECT_EQ(got.entries[0].path, "checksums.txt");
    EXPECT_EQ(got.entries[0].placement, EntryPlacement::Inline);
    EXPECT_EQ(got.entries[0].inline_bytes, "hello-inline");
    EXPECT_EQ(got.entries[1].path, "columns/data.bin");
    EXPECT_EQ(got.entries[1].placement, EntryPlacement::Blob);
    EXPECT_EQ(got.entries[1].blob_size, 4096u);
    EXPECT_EQ(got.entries[1].ref.digest.toU128(), (UInt128(1) << 64) | UInt128(2));
}

TEST(CasManifestCodec, ByteDeterminism)
{
    const PartManifest m = sample();
    /// Encode twice -> identical bytes. Also encode a copy with entries pre-shuffled into the other
    /// order -> still identical, because the encoder sorts canonically.
    PartManifest m2 = m;
    std::swap(m2.entries[0], m2.entries[1]);
    EXPECT_EQ(encodePartManifest(m), encodePartManifest(m));
    EXPECT_EQ(encodePartManifest(m), encodePartManifest(m2));
}

TEST(CasManifestCodec, DuplicatePathRejectedOnEncode)
{
    PartManifest m = sample();
    m.entries.push_back(m.entries[0]);   /// duplicate path
    try
    {
        encodePartManifest(m);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, BadMagicFailsClosed)
{
    String bytes = encodePartManifest(sample());
    bytes[0] ^= 0xFF;   /// corrupt the magic
    try
    {
        decodePartManifest(bytes);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, TruncatedFailsClosed)
{
    const String bytes = encodePartManifest(sample());
    const String truncated = bytes.substr(0, bytes.size() / 2);
    try
    {
        decodePartManifest(truncated);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, TruncationSweepFailsClosed)
{
    /// Sweep many truncation lengths over a valid encoded PartManifest. Every prefix must fail
    /// closed with CORRUPTED_DATA — never crash, never let a std::out_of_range escape (the embedded
    /// RunFile footer parse must be hardened against truncated/corrupt length fields).
    const String bytes = encodePartManifest(sample());
    for (size_t k = 1; k < bytes.size(); ++k)
    {
        const String truncated = bytes.substr(0, k);
        try
        {
            decodePartManifest(truncated);
            FAIL() << "expected CORRUPTED_DATA for truncation length " << k;
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA) << "length " << k << ": " << e.message();
        }
        catch (const std::exception & e)
        {
            FAIL() << "length " << k << ": escaping std::exception (not fail-closed): " << e.what();
        }
    }
}

TEST(CasManifestCodec, EmptyEntriesRoundTrips)
{
    PartManifest m = sample();
    m.entries.clear();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    EXPECT_TRUE(got.entries.empty());
    EXPECT_EQ(got.ref, m.ref);
}

TEST(CasManifestCodec, PayloadDigestStableAndContentSensitive)
{
    const PartManifest m = sample();
    /// Stable for identical bodies, and independent of the payload_digest field itself.
    PartManifest with_digest = m;
    with_digest.payload_digest = UInt128(0x1234);
    EXPECT_EQ(computePayloadDigest(m), computePayloadDigest(m));
    EXPECT_EQ(computePayloadDigest(m), computePayloadDigest(with_digest));
    /// Changing real content (an entry's blob size) changes the digest.
    PartManifest changed = m;
    changed.entries[0].blob_size += 1;
    EXPECT_NE(computePayloadDigest(m), computePayloadDigest(changed));
}

TEST(CasManifestCodec, RefMatchesBodyAcceptsExactRef)
{
    const PartManifest m = sample();
    /// The journal ref equals the body ref -> true.
    EXPECT_TRUE(refMatchesBody(m.ref, m));
}

TEST(CasManifestCodec, RefMatchesBodyRejectsEachFieldMismatch)
{
    const PartManifest m = sample();
    ManifestRef wrong_writer = m.ref; wrong_writer.writer_epoch = m.ref.writer_epoch + 1;
    ManifestRef wrong_seq = m.ref;    wrong_seq.build_sequence = m.ref.build_sequence + 1;
    ManifestRef wrong_inst = m.ref;   wrong_inst.manifest_ordinal = m.ref.manifest_ordinal + 1;
    EXPECT_FALSE(refMatchesBody(wrong_writer, m));
    EXPECT_FALSE(refMatchesBody(wrong_seq, m));
    EXPECT_FALSE(refMatchesBody(wrong_inst, m));
}

TEST(CasManifestCodec, ManifestNamespaceMatchesAcceptsOwningNs)
{
    const PartManifest m = sample();
    EXPECT_TRUE(manifestNamespaceMatches(m.root_namespace_id, m));
}

TEST(CasManifestCodec, ManifestNamespaceMatchesRejectsForeignNs)
{
    const PartManifest m = sample();
    EXPECT_FALSE(manifestNamespaceMatches(RootNamespace("srv-b/other@cas@"), m));
    /// A namespace that is a prefix but not equal is still a mismatch (no loose comparison).
    EXPECT_FALSE(manifestNamespaceMatches(RootNamespace("srv-a/uuid"), m));
}

namespace
{

DB::Cas::PartManifest makeTwoEntryManifestForOrderTest()
{
    DB::Cas::PartManifest m;
    m.ref = DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 3};
    m.root_namespace_id = DB::Cas::RootNamespace{"srv/tbl"};
    DB::Cas::ManifestEntry a;
    a.path = "path_alpha_0001";
    a.placement = DB::Cas::EntryPlacement::Inline;
    a.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(DB::UInt128(1))};
    a.blob_size = 1;
    a.inline_bytes = "x";
    DB::Cas::ManifestEntry b = a;
    b.path = "path_bravo_0002";
    b.ref.digest = DB::Cas::BlobDigest::fromU128(DB::UInt128(2));
    b.inline_bytes = "y";
    m.entries = {a, b};
    m.payload_digest = DB::Cas::computePayloadDigest(m);
    return m;
}

/// ---- forging helpers ----
///
/// A naive text-level byte swap does not work here: the embedded RunFile stores each path THREE
/// times (the block header's own min/max key, the record's own key inside the block payload —
/// the ONLY copy `RunFileReader::next` actually reads — and the footer's block-index min/max
/// key), and the footer bytes are covered by a footer crc32c while the payload bytes are covered
/// by a separate per-block crc32c (spec 2026-07-08-cas-part-folder-cache; format documented in
/// `CasRunFile.cpp`). Touching the footer's copy breaks the footer CRC before the decoder's new
/// ordering check ever runs. These helpers edit ONLY the payload copy and re-bless the block CRC,
/// so the forged body is genuinely out-of-order yet passes every RunFile-level integrity check —
/// isolating the test to the decoder's own ordering check.

uint32_t le32At(const String & s, size_t off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(static_cast<unsigned char>(s[off + i])) << (8 * i);
    return v;
}

void putLe32At(String & s, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s[off + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

/// Locate [payload_start, payload_end) of the embedded RunFile's single data block (true for
/// these small forge fixtures — every entry fits in one block): the exact span the block's own
/// crc32c field covers. Anchored on the RunFile's own magic "CARN" (a fixed 4-byte literal) since
/// the embedded RunFile's byte offset inside the outer PartManifest encoding is an implementation
/// detail this test should not otherwise hardcode.
std::pair<size_t, size_t> singleBlockPayloadRange(const String & encoded)
{
    const size_t run_header_pos = encoded.find("CARN");
    EXPECT_NE(run_header_pos, String::npos);
    constexpr size_t kRunHeaderLen = 13;   /// magic(4)+format_version(2)+kind(1)+key_schema(1)+codec(1)+block_size(4)
    const size_t block_len_pos = run_header_pos + kRunHeaderLen;
    const uint32_t block_len = le32At(encoded, block_len_pos);
    const size_t records_region_start = block_len_pos + 4;   /// record_count..payload, per block_len's own definition
    size_t pos = records_region_start;
    pos += 4;                                                 /// record_count
    const uint32_t min_key_len = le32At(encoded, pos); pos += 4 + min_key_len;
    const uint32_t max_key_len = le32At(encoded, pos); pos += 4 + max_key_len;
    pos += 4;                                                 /// crc32c field itself
    const size_t payload_start = pos;
    const size_t payload_end = records_region_start + block_len;
    return {payload_start, payload_end};
}

/// Recompute + patch the block's crc32c (the 4 bytes immediately before `payload_start`) after an
/// in-place, length-preserving edit inside [payload_start, payload_end).
void patchBlockCrc(String & encoded, size_t payload_start, size_t payload_end)
{
    const uint32_t crc = crc32c::Crc32c(
        reinterpret_cast<const uint8_t *>(encoded.data() + payload_start), payload_end - payload_start);
    putLe32At(encoded, payload_start - 4, crc);
}

/// Swap the on-disk positions of two equal-total-length manifest entry RECORDS inside the single
/// data block, then re-bless the block CRC — producing a CRC-consistent but canonically
/// out-of-order `PartManifest` body.
String forgeSwappedRecordOrder(String encoded, const String & key1, const String & key2)
{
    const auto [payload_start, payload_end] = singleBlockPayloadRange(encoded);
    const size_t key1_pos = encoded.find(key1, payload_start);
    const size_t key2_pos = encoded.find(key2, payload_start);
    EXPECT_NE(key1_pos, String::npos);
    EXPECT_NE(key2_pos, String::npos);
    EXPECT_LT(key1_pos, payload_end);
    EXPECT_LT(key2_pos, payload_end);
    const size_t rec1_start = key1_pos - 4;   /// -4: the record's own key_len u32 prefix
    const size_t rec2_start = key2_pos - 4;
    EXPECT_LT(rec1_start, rec2_start);
    const size_t rec_len = rec2_start - rec1_start;   /// records are contiguous: the gap IS one record's length
    const String rec1 = encoded.substr(rec1_start, rec_len);
    const String rec2 = encoded.substr(rec2_start, rec_len);
    encoded.replace(rec1_start, rec_len, rec2);
    encoded.replace(rec2_start, rec_len, rec1);
    patchBlockCrc(encoded, payload_start, payload_end);
    return encoded;
}

/// Rename ONE entry's in-payload record key (only the copy `next()` reads) to `new_key` (equal
/// length, so no byte shifts) and re-bless the block CRC. Used to forge a non-adjacent duplicate
/// path without disturbing RunFile's own block/footer integrity checks.
String forgeRenamedRecordKey(String encoded, const String & old_key, const String & new_key)
{
    EXPECT_EQ(old_key.size(), new_key.size());
    const auto [payload_start, payload_end] = singleBlockPayloadRange(encoded);
    const size_t pos = encoded.find(old_key, payload_start);
    EXPECT_NE(pos, String::npos);
    EXPECT_LT(pos, payload_end);
    encoded.replace(pos, old_key.size(), new_key);
    patchBlockCrc(encoded, payload_start, payload_end);
    return encoded;
}

}

TEST(CasManifestCodec, DecodeRejectsOutOfOrderEntries)
{
    const auto m = makeTwoEntryManifestForOrderTest();
    const String forged = forgeSwappedRecordOrder(
        DB::Cas::encodePartManifest(m), "path_alpha_0001", "path_bravo_0002");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { DB::Cas::decodePartManifest(forged); });
}

TEST(CasManifestCodec, DecodeRejectsNonAdjacentDuplicatePath)
{
    /// Three entries a < b < c; forging c := a yields (a, b, a) — the OLD adjacent-only duplicate
    /// check missed this shape; the ordering check must reject it.
    auto m = makeTwoEntryManifestForOrderTest();
    DB::Cas::ManifestEntry c = m.entries[0];
    c.path = "path_delta_0003";
    c.ref.digest = DB::Cas::BlobDigest::fromU128(DB::UInt128(3));
    c.inline_bytes = "z";
    m.entries.push_back(c);
    m.payload_digest = DB::Cas::computePayloadDigest(m);
    const String encoded = DB::Cas::encodePartManifest(m);
    ASSERT_NE(encoded.find("path_delta_0003"), String::npos);
    const String forged = forgeRenamedRecordKey(encoded, "path_delta_0003", "path_alpha_0001");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { DB::Cas::decodePartManifest(forged); });
}

TEST(CasManifestCodec, MixedAlgoEntriesRoundTrip)
{
    PartManifest m;
    m.ref = ManifestRef{7, 1, 1};
    m.root_namespace_id = RootNamespace("srv/x@cas@");
    ManifestEntry a;                                   /// carried-forward old-algo entry
    a.path = "a.bin"; a.placement = EntryPlacement::Blob;
    a.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(0x11))};
    a.blob_size = 3;
    ManifestEntry b;                                   /// fresh new-algo entry
    b.path = "b.bin"; b.placement = EntryPlacement::Blob;
    b.ref.algo = BlobHashAlgo::Sha256;
    for (size_t i = 0; i < 32; ++i) b.ref.digest.bytes[i] = static_cast<uint8_t>(0xC0 + i);
    b.blob_size = 4;
    m.entries = {a, b};
    m.payload_digest = computePayloadDigest(m);
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    ASSERT_EQ(got.entries.size(), 2u);
    EXPECT_EQ(got.entries[0], a);
    EXPECT_EQ(got.entries[1], b);                      /// all 32 sha256 bytes survive next to a 16-byte sibling
}

TEST(CasManifestCodec, UnknownEntryAlgoFailsClosed)
{
    PartManifest m;
    m.ref = ManifestRef{7, 1, 1};
    m.root_namespace_id = RootNamespace("srv/x@cas@");
    ManifestEntry e; e.path = "a"; e.placement = EntryPlacement::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(UInt128(1))};
    m.entries = {e};
    String bytes = encodePartManifest(m);
    /// entry payloads live inside the embedded RunFile; corrupt the ONE algo byte by searching for
    /// the encoded entry: placement(0x02) followed by algo(0x01) — flip algo to 99.
    const size_t pos = bytes.find(String("\x02\x01", 2));
    ASSERT_NE(pos, String::npos);
    bytes[pos + 1] = static_cast<char>(99);
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ decodePartManifest(bytes); });
}

TEST(CasManifestCodec, FindEntryBinarySearch)
{
    std::vector<DB::Cas::ManifestEntry> entries;
    for (const char * p : {"a.txt", "b/inner.txt", "b/z.txt", "c.txt"})
    {
        DB::Cas::ManifestEntry e;
        e.path = p;
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.inline_bytes = "v";
        entries.push_back(e);
    }
    EXPECT_NE(DB::Cas::findEntry(entries, "a.txt"), nullptr);
    EXPECT_EQ(DB::Cas::findEntry(entries, "a.txt")->path, "a.txt");
    EXPECT_NE(DB::Cas::findEntry(entries, "c.txt"), nullptr);          /// last element
    EXPECT_EQ(DB::Cas::findEntry(entries, "b"), nullptr);              /// prefix of a path, not a path
    EXPECT_EQ(DB::Cas::findEntry(entries, "zzz"), nullptr);            /// past the end
    EXPECT_EQ(DB::Cas::findEntry({}, "a"), nullptr);                   /// empty
}

TEST(CasManifestCodec, EntryRangeContiguousPrefix)
{
    std::vector<DB::Cas::ManifestEntry> entries;
    for (const char * p : {"a.txt", "p.proj/data.bin", "p.proj/x.txt", "q.txt"})
    {
        DB::Cas::ManifestEntry e;
        e.path = p;
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.inline_bytes = "v";
        entries.push_back(e);
    }
    auto [first, last] = DB::Cas::entryRange(entries, "p.proj/");
    ASSERT_EQ(last - first, 2);
    EXPECT_EQ(first->path, "p.proj/data.bin");
    EXPECT_EQ((last - 1)->path, "p.proj/x.txt");

    auto [w1, w2] = DB::Cas::entryRange(entries, "");                  /// empty prefix = whole span
    EXPECT_EQ(w2 - w1, 4);

    auto [n1, n2] = DB::Cas::entryRange(entries, "zzz/");              /// no match
    EXPECT_EQ(n1, n2);
}
