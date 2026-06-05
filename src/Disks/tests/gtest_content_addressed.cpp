#include <gtest/gtest.h>
#include <cstring>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Codec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/RefPayload.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>

using namespace DB::ContentAddressed;

// ==== Task 1: the shared little-endian codec + versioned format header ====

// The codec serialises integers explicitly little-endian regardless of host byte order, so a content-
// addressed object written on one architecture is byte-identical to one written on another. Assert the
// exact bytes for a known value (the high byte lands LAST), and that varint/string round-trip including
// high values and embedded NULs.
TEST(ContentAddressedCodec, LittleEndianAndVarintAndStringRoundTrip)
{
    std::string out;
    {
        DB::WriteBufferFromString buf(out);
        DB::writeBinaryLittleEndian(static_cast<uint32_t>(0x01020304u), buf);
        DB::writeBinaryLittleEndian(static_cast<uint64_t>(0x0102030405060708ull), buf);
        DB::writeVarUInt(static_cast<uint64_t>(300), buf);
        DB::writeStringBinary(std::string("hi\0there", 8), buf);
    }
    // u32 0x01020304 little-endian == 04 03 02 01; u64 high byte 0x01 last.
    const std::string expected =
        std::string("\x04\x03\x02\x01", 4) +
        std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8) +
        std::string("\xAC\x02", 2) /* varint 300 */ +
        std::string("\x08", 1) /* varint length 8 */ + std::string("hi\0there", 8);
    EXPECT_EQ(out, expected);

    DB::ReadBufferFromString in(out);
    uint32_t a = 0;
    uint64_t b = 0;
    uint64_t v = 0;
    std::string s;
    DB::readBinaryLittleEndian(a, in);
    DB::readBinaryLittleEndian(b, in);
    DB::readVarUInt(v, in);
    DB::readStringBinary(s, in);
    EXPECT_EQ(a, 0x01020304u);
    EXPECT_EQ(b, 0x0102030405060708ull);
    EXPECT_EQ(v, 300u);
    EXPECT_EQ(s, std::string("hi\0there", 8));
}

TEST(ContentAddressedCodec, FormatHeaderRoundTrips)
{
    constexpr FormatMagic kMagic = makeMagic("CAXX");
    std::string out;
    {
        DB::WriteBufferFromString buf(out);
        FormatHeader{kMagic, 3}.write(buf);
    }
    EXPECT_EQ(out, std::string("CAXX\x03", 5)); // 4 magic + 1 version byte
    DB::ReadBufferFromString in(out);
    EXPECT_EQ(FormatHeader::readAndValidate(in, kMagic, /*max=*/5, "test"), 3);
}

TEST(ContentAddressedCodec, FormatHeaderRejectsWrongMagic)
{
    constexpr FormatMagic kMagic = makeMagic("CAXX");
    DB::ReadBufferFromString in(std::string("ZZZZ\x01", 5));
    EXPECT_THROW(FormatHeader::readAndValidate(in, kMagic, /*max=*/5, "test"), DB::Exception);
}

TEST(ContentAddressedCodec, FormatHeaderFailsClosedOnUnknownVersion)
{
    constexpr FormatMagic kMagic = makeMagic("CAXX");
    // A future version (above what this build understands) must fail closed, not be misparsed.
    DB::ReadBufferFromString future(std::string("CAXX\x09", 5));
    EXPECT_THROW(FormatHeader::readAndValidate(future, kMagic, /*max=*/5, "test"), DB::Exception);
    // Version 0 is never written and is rejected too.
    DB::ReadBufferFromString zero(std::string("CAXX\x00", 5));
    EXPECT_THROW(FormatHeader::readAndValidate(zero, kMagic, /*max=*/5, "test"), DB::Exception);
}

TEST(ContentAddressedPartManifest, RoundTripBasic)
{
    PartManifest f;
    f.blobs["col_a.bin"] = BlobEntry{BlobHash("hashA"), 100, "ckA"};
    f.blobs["col_b.bin"] = BlobEntry{BlobHash("hashB"), 200, "ckB"};
    f.inlined["columns.txt"] = "a b";
    f.inlined["count.txt"] = std::string("100\n\0binary", 11); // embedded NUL

    std::string bytes = f.serialize();
    PartManifest g = PartManifest::deserialize(bytes);

    EXPECT_EQ(g.blobs.size(), 2u);
    EXPECT_EQ(g.blobs.at("col_a.bin").key, BlobHash("hashA"));
    EXPECT_EQ(g.blobs.at("col_a.bin").size, 100u);
    EXPECT_EQ(g.inlined.at("columns.txt"), "a b");
    EXPECT_EQ(g.inlined.at("count.txt"), std::string("100\n\0binary", 11));
}

TEST(ContentAddressedPartManifest, StableHashIsCanonical)
{
    PartManifest a; a.blobs["y"] = {BlobHash("hy"), 2, "c2"}; a.blobs["x"] = {BlobHash("hx"), 1, "c1"};
    PartManifest b; b.blobs["x"] = {BlobHash("hx"), 1, "c1"}; b.blobs["y"] = {BlobHash("hy"), 2, "c2"};
    EXPECT_EQ(a.serialize(), b.serialize());
}

TEST(ContentAddressedPartManifest, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(PartManifest::deserialize("XXXX"), std::exception);
    std::string ok = PartManifest{}.serialize();
    EXPECT_THROW(PartManifest::deserialize(ok.substr(0, ok.size() - 1)), std::exception);
}

TEST(ContentAddressedPartManifest, RejectsForgedHugeLength)
{
    std::string b;
    DB::WriteBufferFromString buf(b);
    FormatHeader{PartManifest::MAGIC, PartManifest::VERSION}.write(buf);
    DB::writeVarUInt(1, buf);                          // blobs count = 1
    DB::writeVarUInt(0xFFFFFFFFFFFFFFFFull, buf);      // forged key length -> must throw, not wrap
    buf.finalize();
    EXPECT_THROW(PartManifest::deserialize(b), DB::Exception);
}

// Task 2 (B19): a fixed manifest serialises to a FIXED, pinned byte string. This pins the on-object
// format little-endian and locks cross-arch determinism (CI runs amd64 and arm64): the SAME logical
// content must produce the SAME bytes on every architecture, and a format change must update this
// golden value on purpose.
TEST(ContentAddressedPartManifest, GoldenBytes)
{
    PartManifest f;
    f.blobs["a.bin"] = BlobEntry{BlobHash("h1"), 0x0102u, "ck1"};
    f.inlined["count.txt"] = std::string("5\0", 2); // embedded NUL

    const std::string expected =
        std::string("CAMF\x01", 5)                // magic(4) + version(1)
        + std::string("\x01", 1)                  // varint blobs count = 1
        + std::string("\x05", 1) + "a.bin"        // name "a.bin"
        + std::string("\x02", 1) + "h1"           // blob key "h1"
        + std::string("\x02\x01\x00\x00\x00\x00\x00\x00", 8) // size 0x0102 LE u64
        + std::string("\x03", 1) + "ck1"          // checksum "ck1"
        + std::string("\x01", 1)                  // varint inlined count = 1
        + std::string("\x09", 1) + "count.txt"    // name "count.txt"
        + std::string("\x02", 1) + std::string("5\0", 2); // value "5\0"
    EXPECT_EQ(f.serialize(), expected);
    // And it round-trips back to the same logical content.
    PartManifest g = PartManifest::deserialize(f.serialize());
    EXPECT_EQ(g.blobs.at("a.bin").size, 0x0102u);
    EXPECT_EQ(g.inlined.at("count.txt"), std::string("5\0", 2));
}

// Task 2: a future (unknown) version fails closed instead of being misparsed.
TEST(ContentAddressedPartManifest, RejectsUnknownVersion)
{
    std::string ok = PartManifest{}.serialize();
    ASSERT_GE(ok.size(), 5u);
    ok[4] = static_cast<char>(PartManifest::VERSION + 1); // bump the version byte
    EXPECT_THROW(PartManifest::deserialize(ok), DB::Exception);
}

// Task 2 invariant (CRITICAL): part_id is computed over (filename, checksum), NOT over the manifest
// bytes, so re-implementing the manifest format must NOT change part_id. This golden value pins it:
// if this changes, the format change altered identities/dedup and is wrong.
TEST(ContentAddressedPartManifest, GoldenPartIdUnchanged)
{
    std::map<std::string, BlobEntry> blobs;
    blobs["a.bin"] = {BlobHash("h1"), 3, "ck1"};
    blobs["b.bin"] = {BlobHash("h2"), 6, "ck2"};
    // Mutable per-part files must NOT affect the identity.
    blobs["uuid.txt"] = {BlobHash("u"), 1, "u"};
    EXPECT_EQ(computePartId(blobs).string(), "8d45de9b773149cfb2e02c23e01d1fdf");
}

// ==== Task 3 (B28): the ref payload is a versioned struct, parsed by ONE function ====

// The ref payload is `MAGIC+version+part_id` (length-prefixed). `serializeRefPayload` writes it and
// `partIdFromRefPayload` reads it back exactly. Pin the on-object bytes (cross-arch determinism).
TEST(ContentAddressedRefPayload, GoldenBytesAndRoundTrip)
{
    const PartId pid("8d45de9b773149cfb2e02c23e01d1fdf");
    const std::string payload = serializeRefPayload(pid);
    const std::string expected =
        std::string("CARF\x01", 5)                          // magic(4) + version(1)
        + std::string("\x20", 1) + pid.string();            // length-prefixed part_id (32 bytes)
    EXPECT_EQ(payload, expected);
    EXPECT_EQ(partIdFromRefPayload(payload), pid);
}

// A future (unknown) version must fail closed, not be misparsed.
TEST(ContentAddressedRefPayload, RejectsUnknownVersion)
{
    std::string payload = serializeRefPayload(PartId("abc"));
    ASSERT_GE(payload.size(), 5u);
    payload[4] = static_cast<char>(payload[4] + 1); // bump version byte
    EXPECT_THROW(partIdFromRefPayload(payload), DB::Exception);
}

// A bad magic / truncation is rejected (fail close). The old "first hex run" parse is gone, so a bare
// hex string with no header is no longer accepted as a payload.
TEST(ContentAddressedRefPayload, RejectsBadMagicAndBareHex)
{
    EXPECT_THROW(partIdFromRefPayload("XXXX"), DB::Exception);
    EXPECT_THROW(partIdFromRefPayload(""), DB::Exception);
    // A bare hex part id (the OLD unversioned payload) is no longer a valid payload: the parser is
    // now exact and requires the versioned header.
    EXPECT_THROW(partIdFromRefPayload("8d45de9b773149cfb2e02c23e01d1fdf"), DB::Exception);
}

// B23 Task 1: the canonical predicate for mutable per-part files. The set is the SINGLE source of
// truth shared with computePartId's exclusion: anything excluded from the part identity is exactly a
// mutable per-part file (they live per-ref, never in the shared manifest).
TEST(ContentAddressedMutablePerPartFile, PredicateMatchesPartIdExclusion)
{
    EXPECT_TRUE(isMutablePerPartFile("uuid.txt"));
    EXPECT_TRUE(isMutablePerPartFile("txn_version.txt"));
    EXPECT_TRUE(isMutablePerPartFile("metadata_version.txt"));
    EXPECT_FALSE(isMutablePerPartFile("a.bin"));
    EXPECT_FALSE(isMutablePerPartFile("columns.txt"));
    EXPECT_FALSE(isMutablePerPartFile("count.txt"));

    // The set is derived from one shared constant: assert the predicate agrees with the part-id
    // exclusion for every name in the canonical list (single source of truth).
    for (const auto & name : mutablePerPartFiles())
        EXPECT_TRUE(isMutablePerPartFile(name)) << name;
    EXPECT_EQ(mutablePerPartFiles().size(), 3u);
}

// B23 Task 1: the per-ref sidecar is a tiny versioned {filename -> bytes} blob. Round-trip preserves
// names and bytes (including embedded NULs), and a bad magic / truncation is rejected (fail-close).
TEST(ContentAddressedRefSidecar, RoundTripsNamesAndBytes)
{
    RefSidecar s;
    s.files["uuid.txt"] = "0c9d8e7f-1234-5678-9abc-def012345678\n";
    s.files["txn_version.txt"] = std::string("42\n\0binary", 9); // embedded NUL
    s.files["metadata_version.txt"] = "7";

    std::string bytes = s.serialize();
    RefSidecar g = RefSidecar::deserialize(bytes);

    EXPECT_EQ(g.files.size(), 3u);
    EXPECT_EQ(g.files.at("uuid.txt"), "0c9d8e7f-1234-5678-9abc-def012345678\n");
    EXPECT_EQ(g.files.at("txn_version.txt"), std::string("42\n\0binary", 9));
    EXPECT_EQ(g.files.at("metadata_version.txt"), "7");
}

TEST(ContentAddressedRefSidecar, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(RefSidecar::deserialize("XXXX"), DB::Exception);
    RefSidecar s; s.files["uuid.txt"] = "abc";
    std::string ok = s.serialize();
    EXPECT_THROW(RefSidecar::deserialize(ok.substr(0, ok.size() - 1)), DB::Exception);
}

// Task 4: the sidecar is on the shared header now. A future (unknown) version fails closed, and the
// on-object bytes are pinned (cross-arch determinism).
// CA GC S3 (#6): v2 appends manifest_generation (varint) and the (blob-hash -> g) map count (varint).
// Both are zero for a sidecar with no pinned blobs, adding two \x00 bytes after the files section.
TEST(ContentAddressedRefSidecar, GoldenBytesAndRejectsUnknownVersion)
{
    RefSidecar s;
    s.files["uuid.txt"] = "ab";
    const std::string expected =
        std::string("CASC\x02", 5)               // magic(4) + version(2)
        + std::string("\x01", 1)                 // varint files count = 1
        + std::string("\x08", 1) + "uuid.txt"    // name
        + std::string("\x02", 1) + "ab"          // bytes
        + std::string("\x00", 1)                 // varint manifest_generation = 0
        + std::string("\x00", 1);                // varint pin_generations size = 0
    EXPECT_EQ(s.serialize(), expected);

    std::string future = s.serialize();
    future[4] = static_cast<char>(RefSidecar::VERSION + 1); // bump the version byte
    EXPECT_THROW(RefSidecar::deserialize(future), DB::Exception);
}

TEST(ContentAddressedBlobRefIndex, DeltaCountAndDedup)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;
    PartManifest p1; p1.blobs["a.bin"] = {BlobHash("hA"), 1, "hA"}; p1.blobs["b.bin"] = {BlobHash("hShared"), 1, "hShared"};
    PartManifest p2; p2.blobs["a.bin"] = {BlobHash("hZ"), 1, "hZ"}; p2.blobs["b.bin"] = {BlobHash("hShared"), 1, "hShared"};
    idx.addPart(PartId("part1"), p1);
    idx.addPart(PartId("part2"), p2);
    EXPECT_EQ(idx.refcount(BlobHash("hShared")), 2);
    EXPECT_EQ(idx.refcount(BlobHash("hA")), 1);
    idx.removePart(PartId("part1"), p1);
    EXPECT_EQ(idx.refcount(BlobHash("hShared")), 1);
    EXPECT_EQ(idx.refcount(BlobHash("hA")), 0);
    auto dead = idx.unreferenced();
    EXPECT_TRUE(dead.count(BlobHash("hA")));
    EXPECT_FALSE(dead.count(BlobHash("hShared")));
}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <unordered_map>

TEST(ContentAddressedReachability, ReconcileMarksOnlyLiveRoots)
{
    using namespace DB::ContentAddressed;
    std::unordered_map<PartId, PartManifest> parts;
    PartManifest pm; pm.blobs["a.bin"] = {BlobHash("hA1"), 1, "hA1"}; pm.blobs["b.bin"] = {BlobHash("hB0"), 1, "hB0"}; parts[PartId("all_1_1_0_1")] = pm; // mutation: new a, carried b
    PartManifest src; src.blobs["a.bin"] = {BlobHash("hA0"), 1, "hA0"}; src.blobs["b.bin"] = {BlobHash("hB0"), 1, "hB0"}; parts[PartId("all_1_1_0")] = src;   // outdated source

    auto resolve = [&](const PartId & id) { return parts.at(id); };
    std::set<PartId> roots = {PartId("all_1_1_0_1")}; // only the mutated part is a live root
    // markReachableBlobs returns FULL blob object keys (blobKey fan-out of the bare manifest hash),
    // matching what the GC sweep lists under blobsPrefix; assert against the projected keys.
    std::set<BlobObjectKey> reachable = markReachableBlobs("", roots, resolve);

    EXPECT_TRUE(reachable.count(blobKey("", BlobHash("hA1"))));
    EXPECT_TRUE(reachable.count(blobKey("", BlobHash("hB0"))));   // carried forward → still reachable
    EXPECT_FALSE(reachable.count(blobKey("", BlobHash("hA0"))));  // replaced column → unreachable
}

TEST(ContentAddressedReachability, SweepUsesTimeSinceUnreachableNotAge)
{
    using namespace DB::ContentAddressed;
    std::set<std::string> unreferenced = {"old_blob"};
    std::unordered_map<std::string, int64_t> first_unreachable; // empty: just became unreachable

    auto r1 = selectForSweep(unreferenced, first_unreachable, /*now*/ 1000, /*grace*/ 300);
    EXPECT_TRUE(r1.to_delete.empty());                 // first sighting → not yet
    EXPECT_EQ(r1.first_unreachable.at("old_blob"), 1000);

    auto r2 = selectForSweep(unreferenced, r1.first_unreachable, /*now*/ 1250, 300);
    EXPECT_TRUE(r2.to_delete.empty());                 // 250 < 300

    auto r3 = selectForSweep(unreferenced, r2.first_unreachable, /*now*/ 1400, 300);
    EXPECT_EQ(r3.to_delete.size(), 1u);                // 400 >= 300 → delete
    EXPECT_EQ(r3.to_delete.at(0), "old_blob");

    // becoming reachable again clears the timer:
    auto r4 = selectForSweep(/*unreferenced*/ {}, r2.first_unreachable, /*now*/ 1400, 300);
    EXPECT_TRUE(r4.first_unreachable.empty());
}

TEST(ContentAddressedScenario, MutationCarryForwardThenGC)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;

    PartManifest base;
    base.blobs["a.bin"] = {BlobHash("A0"), 1, "A0"};
    base.blobs["b.bin"] = {BlobHash("B0"), 1, "B0"};
    base.blobs["c.bin"] = {BlobHash("C0"), 1, "C0"};
    idx.addPart(PartId("all_1_1_0"), base);

    PartManifest mut; // mutation rewrites only a.bin; b/c carried forward by reference
    mut.blobs["a.bin"] = {BlobHash("A1"), 1, "A1"};
    mut.blobs["b.bin"] = {BlobHash("B0"), 1, "B0"};
    mut.blobs["c.bin"] = {BlobHash("C0"), 1, "C0"};
    idx.addPart(PartId("all_1_1_0_1"), mut);

    idx.removePart(PartId("all_1_1_0"), base); // lifecycle drops the covered source ref

    auto dead = idx.unreferenced();
    EXPECT_EQ(dead.size(), 1u);          // only the replaced column is dead
    EXPECT_TRUE(dead.count(BlobHash("A0")));

    /// selectForSweep operates on raw object-key strings; reduce the typed dead set to that space.
    std::set<std::string> dead_keys;
    for (const auto & h : dead)
        dead_keys.insert(h.string());
    auto r = selectForSweep(dead_keys, {}, /*now*/ 1000, /*grace*/ 0);
    EXPECT_EQ(r.to_delete, std::vector<std::string>{"A0"}); // A0 swept; B0/C0 kept by reachability
    EXPECT_EQ(idx.refcount(BlobHash("B0")), 1);
    EXPECT_EQ(idx.refcount(BlobHash("C0")), 1);
}
