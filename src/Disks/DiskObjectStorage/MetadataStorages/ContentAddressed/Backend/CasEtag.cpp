#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>

namespace DB::Cas
{

bool isIncarnationValue(Dialect dialect, const String & value)
{
    switch (dialect)
    {
        case Dialect::Generation:
        {
            if (value.empty() || value == "0")
                return false;
            if (value.size() > 1 && value.front() == '0')
                return false;
            return std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
        }
        case Dialect::ETag:
        {
            String trimmed = value;
            boost::algorithm::trim(trimmed);
            return !trimmed.empty() && trimmed != "*" && trimmed.find(',') == String::npos;
        }
        case Dialect::Emulated:
            return !value.empty();
    }
    return false;
}

namespace
{

constexpr size_t MAX_TAG_BYTES = 128;
constexpr size_t MAX_GENERATION_DIGITS = 20;

std::optional<String> opaqueEtagTag(std::string_view raw)
{
    if (raw.starts_with("W/"))
        return std::nullopt;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        raw = raw.substr(1, raw.size() - 2);
    if (raw.empty() || raw.size() > MAX_TAG_BYTES || raw == "*" || raw.front() == ' ' || raw.back() == ' ')
        return std::nullopt;
    for (const char c : raw)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == ',' || byte < 0x20 || byte == 0x7F)
            return std::nullopt;
    }
    return String(raw);
}

std::optional<String> opaqueTag(Dialect dialect, std::string_view raw)
{
    if (dialect != Dialect::Generation)
        return opaqueEtagTag(raw);
    if (raw.empty() || raw.size() > MAX_GENERATION_DIGITS || raw.front() == '0')
        return std::nullopt;
    if (!std::all_of(raw.begin(), raw.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return String(raw);
}

constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

}

std::string_view dialectWord(Dialect dialect)
{
    switch (dialect)
    {
        case Dialect::ETag: return "etag";
        case Dialect::Generation: return "generation";
        case Dialect::Emulated: return "emulated";
    }
    UNREACHABLE();
}

std::optional<Dialect> dialectFromWord(std::string_view word)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
        if (word == dialectWord(dialect))
            return dialect;
    return std::nullopt;
}

std::optional<CanonicalIdentity> canonicalIdentity(Dialect dialect, std::string_view raw_value)
{
    auto tag = opaqueTag(dialect, raw_value);
    if (!tag)
        return std::nullopt;
    String hex;
    hex.reserve(tag->size() * 2);
    for (const char c : *tag)
    {
        const auto byte = static_cast<unsigned char>(c);
        hex.push_back(HEX_DIGITS[byte >> 4]);
        hex.push_back(HEX_DIGITS[byte & 15]);
    }
    return CanonicalIdentity{dialect, std::move(hex)};
}

std::optional<String> opaqueTagFromHex(Dialect dialect, std::string_view tag_hex)
{
    if (tag_hex.empty() || tag_hex.size() % 2 != 0)
        return std::nullopt;
    String tag;
    tag.reserve(tag_hex.size() / 2);
    for (size_t i = 0; i < tag_hex.size(); i += 2)
    {
        const size_t hi = HEX_DIGITS.find(tag_hex[i]);
        const size_t lo = HEX_DIGITS.find(tag_hex[i + 1]);
        if (hi == std::string_view::npos || lo == std::string_view::npos)
            return std::nullopt;
        tag.push_back(static_cast<char>((hi << 4) | lo));
    }
    const auto identity = canonicalIdentity(dialect, tag);
    if (!identity || identity->tag_hex != tag_hex)
        return std::nullopt;
    return tag;
}

std::optional<CanonicalIdentity> Etag::canonical() const
{
    return canonicalIdentity(dialect_, value_);
}

std::optional<CanonicalIdentity> PersistedEtag::canonical() const
{
    const auto parsed = dialectFromWord(dialect);
    if (!parsed)
        return std::nullopt;
    return canonicalIdentity(*parsed, value);
}

bool sameIncarnation(const PersistedEtag & row, const Etag & live)
{
    const auto persisted = row.canonical();
    return persisted && persisted == live.canonical();
}

}
