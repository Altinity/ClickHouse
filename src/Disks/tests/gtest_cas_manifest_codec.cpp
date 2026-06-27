#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

using namespace DB::Cas;

namespace
{

PartManifest sample()
{
    PartManifest m;
    m.ref = ManifestRef{"srv-a:7", 1042, UInt128(0x7f3a)};
    m.root_namespace_id = RootNamespace("srv-a/uuid@cas@");
    m.payload_digest = UInt128(0xDEAD);
    ManifestEntry blob;
    blob.path = "columns/data.bin";
    blob.placement = EntryPlacement::Blob;
    blob.blob_hash = (UInt128(1) << 64) | UInt128(2);
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
    EXPECT_EQ(got.entries[1].blob_hash, (UInt128(1) << 64) | UInt128(2));
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
    ManifestRef wrong_writer = m.ref; wrong_writer.writer_instance_id = m.ref.writer_instance_id + "x";
    ManifestRef wrong_seq = m.ref;    wrong_seq.build_sequence = m.ref.build_sequence + 1;
    ManifestRef wrong_inst = m.ref;   wrong_inst.manifest_instance_id = m.ref.manifest_instance_id + UInt128(1);
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
