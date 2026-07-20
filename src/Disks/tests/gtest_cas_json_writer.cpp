#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Formats/FormatSettings.h>
#include <random>

using namespace DB;
using namespace DB::Cas;

TEST(CasJsonWriter, KeyValueSequenceMatchesCanonicalShape)
{
    CasJsonWriter w;
    bool first = true;
    w.key("we", first);
    w.u64StringValue(7);
    w.key("mo", first);
    w.u64Number(3);
    w.key("ok", first);
    w.boolValue(true);
    w.key("o", "me", first);
    w.u64StringValue(1);
    w.closeObject(first);
    w.newline();
    EXPECT_EQ(std::move(w).take(), "{\"we\":\"7\",\"mo\":3,\"ok\":true,\"ome\":\"1\"}\n");
}

TEST(CasJsonWriter, EmptyObjectAndClear)
{
    CasJsonWriter w;
    bool first = true;
    w.closeObject(first);
    EXPECT_EQ(w.view(), "{}");
    w.clear();
    EXPECT_EQ(w.size(), 0u);
}

TEST(CasJsonWriter, Hex128MatchesU128ToHex)
{
    const UInt128 v = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
    CasJsonWriter w;
    w.hex128Value(v);
    EXPECT_EQ(std::move(w).take(), "\"" + u128ToHex(v) + "\"");
}

TEST(CasJsonWriter, U64Extremes)
{
    CasJsonWriter w;
    w.u64Number(0);
    w.appendChar(' ');
    w.u64Number(UINT64_MAX);
    EXPECT_EQ(std::move(w).take(), "0 18446744073709551615");
}

namespace
{
String referenceJson(std::string_view s)
{
    DB::FormatSettings settings;
    settings.json.escape_forward_slashes = false;   /// the pinned CAS canon
    DB::WriteBufferFromOwnString out;
    DB::writeJSONString(s, out, settings);
    out.finalize();
    return out.str();
}

String writerJson(std::string_view s)
{
    DB::Cas::CasJsonWriter w;
    w.stringValue(s);
    return std::move(w).take();
}
}

TEST(CasJsonWriterEscaping, TargetedCorpusMatchesWriteJSONString)
{
    const std::vector<String> corpus = {
        "",
        "plain_safe_ref_name_20260101_0_1_1_1",
        "roots/pin",                                    /// '/' must stay UNESCAPED
        "quote\"inside", "back\\slash", "both\\\"x",
        String("\b\f\n\r\t"),
        String(1, '\0'), String("a") + '\0' + "b",
        String("\x01\x02\x03\x1e\x1f"),
        "\xE2\x80\xA8", "\xE2\x80\xA9",                 /// U+2028 / U+2029 ->   /
        "x\xE2\x80\xA8" "y",
        "\xE2",                                          /// truncated lead byte at end
        "\xE2\x80",                                      /// truncated pair at end
        "\xE2\x21\x21",                                  /// 0xE2 + non-continuation bytes
        "\xE2\x80\x21",
        "\xE2\xE2\x80\xA8",                              /// lead byte immediately before a real sequence
        "\xC3\xA9\xF0\x9F\x98\x80",                      /// ordinary multi-byte UTF-8 passes through
        "\xff\xfe invalid utf8 \x80",
        String(1000, 'a'),                               /// long safe run (vector path)
        String(1000, '"'),                               /// special-dense
    };
    for (const String & s : corpus)
        EXPECT_EQ(writerJson(s), referenceJson(s)) << "input bytes: " << s.size();
}

TEST(CasJsonWriterEscaping, FuzzMatchesWriteJSONString)
{
    std::mt19937 rng(20260720);
    for (int iter = 0; iter < 5000; ++iter)
    {
        const size_t len = rng() % 200;
        String s(len, '\0');
        const int mode = iter % 3;
        for (auto & c : s)
        {
            if (mode == 0)
                c = static_cast<char>(rng() % 256);                     /// full byte range
            else if (mode == 1)
                c = static_cast<char>('a' + rng() % 26);                /// safe-only
            else
            {
                static constexpr char specials[] = {'"', '\\', '\n', '\x01', '\xE2', '\x80', '\xA8', 'z'};
                c = specials[rng() % (sizeof(specials))];               /// special-dense
            }
        }
        ASSERT_EQ(writerJson(s), referenceJson(s)) << "iter " << iter;
    }
}

/// ---- CasJsonWriter overloads of the shared vocabulary (Task 4) ----
///
/// Both the WriteBuffer and CasJsonWriter overload sets coexist until Task 9, so the reference
/// for these tests is the production WriteBuffer vocabulary itself.

TEST(CasJsonWriterVocab, MatchesWriteBufferVocabulary)
{
    using namespace DB::Cas;
    const UInt128 h = (UInt128(0xdeadbeefULL) << 64) | UInt128(42);

    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;
    bool rf = true;
    bool wf = true;

    writeKey(ref, "a", rf);           writeKey(w, "a", wf);
    writeStringValue(ref, "x/\"y");   writeStringValue(w, "x/\"y");
    writeKey(ref, "h", rf);           writeKey(w, "h", wf);
    writeHex128Value(ref, h);         writeHex128Value(w, h);
    writeKey(ref, "u", rf);           writeKey(w, "u", wf);
    writeU64StringValue(ref, UINT64_MAX); writeU64StringValue(w, UINT64_MAX);
    writeKey(ref, "b", rf);           writeKey(w, "b", wf);
    writeBoolValue(ref, false);       writeBoolValue(w, false);
    writeKey(ref, "n", rf);           writeKey(w, "n", wf);
    DB::writeIntText(uint64_t(12345), ref); writeIntText(uint64_t(12345), w);
    closeObject(ref, rf);             closeObject(w, wf);
    DB::writeChar('\n', ref);         writeChar('\n', w);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}

TEST(CasJsonWriterVocab, HeaderTrailerAndManifestFieldsMatch)
{
    using namespace DB::Cas;
    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;

    writeHeaderLine(ref, FormatId::RefLog);   writeHeaderLine(w, FormatId::RefLog);
    bool rf = true;
    bool wf = true;
    writeManifestRefFields(ref, rf, "o", ManifestRef{1, 2, 3});
    writeManifestRefFields(w, wf, "o", ManifestRef{1, 2, 3});
    closeObject(ref, rf);                     closeObject(w, wf);
    DB::writeChar('\n', ref);                 writeChar('\n', w);
    writeTrailerLine(ref, 9);                 writeTrailerLine(w, 9);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}

TEST(CasJsonWriterVocab, TokenFieldsMatch)
{
    using namespace DB::Cas;
    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;
    bool rf = true;
    bool wf = true;

    const Token t{"opaque-etag-value", TokenType::ETag};
    writeTokenFields(ref, rf, t);
    writeTokenFields(w, wf, t);
    closeObject(ref, rf);
    closeObject(w, wf);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}

TEST(CasJsonWriterVocab, BlobRefFieldsMatch)
{
    using namespace DB::Cas;
    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;
    bool rf = true;
    bool wf = true;

    const UInt128 h = (UInt128(0x1122334455667788ULL) << 64) | UInt128(0x99aabbccddeeff00ULL);
    const BlobRef r{BlobHashAlgo::XXH3_128, BlobDigest::fromU128(h)};
    writeBlobRefFields(ref, rf, r);
    writeBlobRefFields(w, wf, r);
    closeObject(ref, rf);
    closeObject(w, wf);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}
