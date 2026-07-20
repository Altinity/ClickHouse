#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/Exception.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <base/hex.h>
#include <base/scope_guard.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <zstd.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int CANNOT_PARSE_INPUT_ASSERTION_FAILED;
    extern const int CANNOT_PARSE_QUOTED_STRING;
    extern const int CANNOT_PARSE_NUMBER;
    extern const int CANNOT_READ_ALL_DATA;
    extern const int ATTEMPT_TO_READ_AFTER_EOF;
    extern const int INCORRECT_DATA;
}
}

namespace DB::Cas
{

namespace
{
const FormatSettings & jsonWriteSettings()
{
    /// CAS text is persisted and, for deterministic objects, compared byte-for-byte on retries.
    /// Keep slash-containing values such as ref paths and fold-seal map keys rendered as `/`,
    /// independently of the process-wide JSON setting; otherwise the same logical object could
    /// produce different bytes and deterministic adoption would reject an equivalent artifact.
    static const FormatSettings settings = []
    {
        FormatSettings s;
        s.json.escape_forward_slashes = false;
        return s;
    }();
    return settings;
}

const FormatSettings::JSON & jsonReadSettings()
{
    static const FormatSettings::JSON settings;
    return settings;
}
}

/// ---- CasJsonWriter ----

namespace
{
constexpr bool isSpecialJsonByte(unsigned char c)
{
    return c < 0x20 || c == '"' || c == '\\' || c == 0xE2;
}
}

const char * findNextSpecialJsonByteScalar(const char * pos, const char * end)
{
    for (; pos != end; ++pos)
        if (isSpecialJsonByte(static_cast<unsigned char>(*pos)))
            return pos;
    return end;
}

const char * findNextSpecialJsonByte(const char * pos, const char * end)
{
#if defined(__SSE2__)
    const __m128i quote = _mm_set1_epi8('"');
    const __m128i backslash = _mm_set1_epi8('\\');
    const __m128i e2 = _mm_set1_epi8('\xE2');
    const __m128i ctl_bound = _mm_set1_epi8(0x1F);
    for (; pos + 16 <= end; pos += 16)
    {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(pos));
        /// unsigned c <= 0x1F  <=>  min(c, 0x1F) == c
        const __m128i is_ctl = _mm_cmpeq_epi8(_mm_min_epu8(chunk, ctl_bound), chunk);
        const __m128i hit = _mm_or_si128(
            _mm_or_si128(is_ctl, _mm_cmpeq_epi8(chunk, quote)),
            _mm_or_si128(_mm_cmpeq_epi8(chunk, backslash), _mm_cmpeq_epi8(chunk, e2)));
        if (const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(hit)))
            return pos + std::countr_zero(mask);
    }
#elif defined(__aarch64__)
    const uint8x16_t quote = vdupq_n_u8('"');
    const uint8x16_t backslash = vdupq_n_u8('\\');
    const uint8x16_t e2 = vdupq_n_u8(0xE2);
    const uint8x16_t ctl_bound = vdupq_n_u8(0x20);
    for (; pos + 16 <= end; pos += 16)
    {
        const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t *>(pos));
        const uint8x16_t hit = vorrq_u8(
            vorrq_u8(vcltq_u8(chunk, ctl_bound), vceqq_u8(chunk, quote)),
            vorrq_u8(vceqq_u8(chunk, backslash), vceqq_u8(chunk, e2)));
        /// Narrow each byte lane to a nibble: any hit -> nonzero nibble in the u64 mask.
        const uint64_t mask
            = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(hit), 4)), 0);
        if (mask)
            return pos + (std::countr_zero(mask) >> 2);
    }
#endif
    return findNextSpecialJsonByteScalar(pos, end);
}

void CasJsonWriter::stringValue(std::string_view s)
{
    appendChar('"');
    const char * pos = s.data();
    const char * const end = s.data() + s.size();
    while (pos != end)
    {
        const char * next = findNextSpecialJsonByte(pos, end);
        if (next != pos)
        {
            buf.append(pos, static_cast<size_t>(next - pos));
            pos = next;
            if (pos == end)
                break;
        }
        const unsigned char c = static_cast<unsigned char>(*pos);
        switch (c)
        {
            case '\b': append("\\b");   ++pos; break;
            case '\f': append("\\f");   ++pos; break;
            case '\n': append("\\n");   ++pos; break;
            case '\r': append("\\r");   ++pos; break;
            case '\t': append("\\t");   ++pos; break;
            case '\\': append("\\\\");  ++pos; break;
            case '"':  append("\\\"");  ++pos; break;
            case 0xE2:
                if (end - pos >= 3 && pos[1] == '\x80' && (pos[2] == '\xA8' || pos[2] == '\xA9'))
                {
                    append(pos[2] == '\xA8' ? std::string_view{"\\u2028"} : std::string_view{"\\u2029"});
                    pos += 3;
                }
                else
                {
                    appendChar('\xE2');
                    ++pos;
                }
                break;
            default:
            {
                /// A control byte without a named escape: \u00XY with writeJSONString's exact
                /// nibble rendering (uppercase A-F for the low nibble).
                const unsigned char lower_half = c & 0xF;
                append("\\u00");
                appendChar(static_cast<char>('0' + (c >> 4)));
                appendChar(static_cast<char>(lower_half <= 9 ? '0' + lower_half : 'A' + lower_half - 10));
                ++pos;
                break;
            }
        }
    }
    appendChar('"');
}

/// ---- write-side JSON micro-vocabulary ----

void writeKey(WriteBuffer & out, std::string_view key, bool & first)
{
    writeChar(first ? '{' : ',', out);
    first = false;
    writeChar('"', out);
    out.write(key.data(), key.size());
    writeChar('"', out);
    writeChar(':', out);
}

void writeStringValue(WriteBuffer & out, std::string_view s)
{
    writeJSONString(s, out, jsonWriteSettings());
}

void writeHex128Value(WriteBuffer & out, const UInt128 & v)
{
    writeChar('"', out);
    const String hex = u128ToHex(v);
    out.write(hex.data(), hex.size());
    writeChar('"', out);
}

void writeU64StringValue(WriteBuffer & out, uint64_t v)
{
    writeChar('"', out);
    writeIntText(v, out);
    writeChar('"', out);
}

void writeBoolValue(WriteBuffer & out, bool v)
{
    writeCString(v ? "true" : "false", out);
}

void closeObject(WriteBuffer & out, bool & first)
{
    if (first)
        writeChar('{', out);
    first = false;
    writeChar('}', out);
}

/// ---- read-side pull cursor ----

/// A canonical-text parse failure is CORRUPTED_DATA regardless of which ReadHelpers primitive
/// noticed it first; the primitives themselves throw a handful of parse-specific codes (assertion
/// failure, quoted-string, number, EOF flavors). This is the single place that narrows all of them
/// down to the two codes this format speaks: CORRUPTED_DATA (translated here) and
/// UNKNOWN_FORMAT_VERSION (thrown deliberately by skipUnknown and passed through unchanged).
template <typename F>
auto JsonObjectReader::guarded(F && f)
{
    try
    {
        return f();
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::CORRUPTED_DATA || e.code() == ErrorCodes::UNKNOWN_FORMAT_VERSION)
            throw;
        if (e.code() == ErrorCodes::CANNOT_PARSE_INPUT_ASSERTION_FAILED
            || e.code() == ErrorCodes::CANNOT_PARSE_QUOTED_STRING
            || e.code() == ErrorCodes::CANNOT_PARSE_NUMBER
            || e.code() == ErrorCodes::CANNOT_READ_ALL_DATA
            || e.code() == ErrorCodes::ATTEMPT_TO_READ_AFTER_EOF
            || e.code() == ErrorCodes::INCORRECT_DATA)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: {}", what, e.message());
        throw;
    }
}

JsonObjectReader::JsonObjectReader(ReadBuffer & in_, KeyStrictness strictness_, std::string_view what_)
    : in(in_), strictness(strictness_), what(what_)
{
    guarded([&] { assertChar('{', in); });
}

bool JsonObjectReader::nextKey(String & key)
{
    return guarded([&]() -> bool
    {
        if (done)
            return false;
        if (first)
        {
            first = false;
            if (checkChar('}', in))
            {
                done = true;
                return false;
            }
        }
        else
        {
            if (checkChar('}', in))
            {
                done = true;
                return false;
            }
            assertChar(',', in);
        }
        readJSONString(key, in, jsonReadSettings());
        assertChar(':', in);
        if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: duplicate key '{}'", what, key);
        seen_keys.push_back(key);
        return true;
    });
}

String JsonObjectReader::readString()
{
    return guarded([&]
    {
        String s;
        readJSONString(s, in, jsonReadSettings());
        return s;
    });
}

UInt128 JsonObjectReader::readHex128()
{
    return guarded([&]
    {
        const String hex = readString();
        if (hex.size() != 32
            || std::any_of(hex.begin(), hex.end(), [](char c) { return unhex(c) == 0xff || (c >= 'A' && c <= 'F'); }))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: expected 32 lowercase hex chars, got '{}'", what, hex);
        return unhexUInt<UInt128>(hex.data());
    });
}

uint64_t JsonObjectReader::readU64String()
{
    return guarded([&]
    {
        const String s = readString();
        ReadBufferFromMemory buf(s.data(), s.size());
        uint64_t v = 0;
        readIntText(v, buf);
        if (s.empty() || !buf.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: expected decimal u64 string, got '{}'", what, s);
        return v;
    });
}

uint64_t JsonObjectReader::readU64Number()
{
    return guarded([&]
    {
        uint64_t v = 0;
        readIntText(v, in);
        return v;
    });
}

uint32_t JsonObjectReader::readU32Number()
{
    const uint64_t v = readU64Number();
    if (v > std::numeric_limits<uint32_t>::max())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: value out of uint32 range", what);
    return static_cast<uint32_t>(v);
}

bool JsonObjectReader::readBool()
{
    return guarded([&]
    {
        if (checkString("true", in))
            return true;
        assertString("false", in);
        return false;
    });
}

void JsonObjectReader::skipUnknown(const String & key)
{
    guarded([&]
    {
        if (!key.empty() && key[0] == '!')
            throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
                "CAS {}: critical key '{}' is not understood by this build", what, key);
        if (strictness == KeyStrictness::Strict)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown key '{}' in a strict format", what, key);
        skipJSONField(in, key, jsonReadSettings());
    });
}

/// ---- header line / trailer line / raw line access ----

void writeHeaderLine(WriteBuffer & out, FormatId id)
{
    const FormatTraits & t = traitsFor(id);
    bool first = true;
    writeKey(out, "type", first);
    writeStringValue(out, t.type);
    writeKey(out, "v", first);
    writeIntText(currentCompatibilityVersion(), out);
    closeObject(out, first);
    writeChar('\n', out);
}

void writeTrailerLine(WriteBuffer & out, uint64_t n)
{
    bool first = true;
    writeKey(out, "n", first);
    writeIntText(n, out);
    closeObject(out, first);
    writeChar('\n', out);
}

String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what)
{
    String line;
    while (true)
    {
        if (in.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: truncated object (line without terminator)", what);
        const char c = *in.position();
        ++in.position();
        if (c == '\n')
            return line;
        line.push_back(c);
        if (line.size() > line_cap)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: line exceeds the {}-byte cap", what, line_cap);
    }
}

namespace
{
TextHeader parseHeaderObject(std::string_view line, std::string_view what)
{
    ReadBufferFromMemory buf(line.data(), line.size());
    JsonObjectReader r(buf, KeyStrictness::Tolerant, what);
    String key;
    if (!r.nextKey(key) || key != "type")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: header line must start with \"type\"", what);
    TextHeader h;
    h.type = r.readString();
    if (!r.nextKey(key) || key != "v")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: header line must carry \"v\" second", what);
    h.v = r.readU32Number();
    while (r.nextKey(key))
        r.skipUnknown(key);
    if (!buf.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: junk after the header object", what);
    return h;
}
}

TextHeader expectHeaderLine(ReadBuffer & in, FormatId id)
{
    const FormatTraits & t = traitsFor(id);
    const String line = readLine(in, t.line_cap, t.type);
    const TextHeader h = parseHeaderObject(line, t.type);
    if (h.type != t.type)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: object is a '{}', not a '{}'", t.type, h.type, t.type);
    checkCompatibility(h.v, t.type);
    return h;
}

std::optional<TextHeader> sniffHeaderLine(std::string_view bytes)
{
    constexpr uint64_t kSniffLineCap = 64 * 1024;
    try
    {
        ReadBufferFromMemory buf(bytes.data(), bytes.size());
        const String line = readLine(buf, kSniffLineCap, "sniff");
        TextHeader h = parseHeaderObject(line, "sniff");
        if (traitsForType(h.type) == nullptr)
            return std::nullopt;
        return h;
    }
    catch (const Exception &)
    {
        return std::nullopt;
    }
}

/// ---- the zstd arm ----

bool looksZstd(std::string_view bytes)
{
    static constexpr char kZstdFramePrefix[4] = {'\x28', '\xB5', '\x2F', '\xFD'};
    return bytes.size() >= 4 && memcmp(bytes.data(), kZstdFramePrefix, 4) == 0;
}

namespace
{
constexpr int kZstdLevel = 3;
}

String sealObject(FormatId id, String text)
{
    const FormatTraits & t = traitsFor(id);
    if (t.compression != CompressionPolicy::Always)
        return text;

    ZSTD_CCtx * cctx = ZSTD_createCCtx();
    if (cctx == nullptr)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: cannot create zstd context", t.type);
    SCOPE_EXIT({ ZSTD_freeCCtx(cctx); });
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, kZstdLevel);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);

    String out;
    out.resize(ZSTD_compressBound(text.size()));
    const size_t written = ZSTD_compress2(cctx, out.data(), out.size(), text.data(), text.size());
    if (ZSTD_isError(written))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd compression failed: {}", t.type, ZSTD_getErrorName(written));
    out.resize(written);
    return out;
}

String openObject(FormatId id, std::string_view stored)
{
    const FormatTraits & t = traitsFor(id);
    if (!looksZstd(stored))
        return String(stored);
    if (t.compression != CompressionPolicy::Always)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: compressed object in a format whose policy is raw", t.type);

    const uint64_t content = ZSTD_getFrameContentSize(stored.data(), stored.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd frame without a declared content size", t.type);
    if (t.object_cap != 0 && content > t.object_cap)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: declared decompressed size {} exceeds the {}-byte cap", t.type, content, t.object_cap);

    String out;
    out.resize(content);
    const size_t got = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
    if (ZSTD_isError(got) || got != content)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd decompression failed: {}",
            t.type, ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output");
    return out;
}

}
