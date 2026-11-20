#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <base/hex.h>

#include <boost/algorithm/string/trim.hpp>

#include <algorithm>

namespace DB::ErrorCodes
{
extern const int LOGICAL_ERROR;
}

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

/// The bytes of a strong ETag between its quotes. A tag may itself start with `W/`: only the unquoted prefix
/// marks a weak ETag.
bool isOpaqueEtagTag(std::string_view tag)
{
    if (tag.empty() || tag.size() > MAX_TAG_BYTES || tag == "*" || tag.front() == ' ' || tag.back() == ' ')
        return false;
    for (const char c : tag)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == ',' || byte < 0x20 || byte == 0x7F)
            return false;
    }
    return true;
}

bool isOpaqueTag(Dialect dialect, std::string_view tag)
{
    if (dialect != Dialect::Generation)
        return isOpaqueEtagTag(tag);
    if (tag.empty() || tag.size() > MAX_GENERATION_DIGITS || tag.front() == '0')
        return false;
    return std::all_of(tag.begin(), tag.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// The opaque tag of a wire value: a weak value is refused, one balanced pair of quotes is removed.
std::optional<String> opaqueTagOfWireValue(Dialect dialect, std::string_view raw)
{
    if (dialect != Dialect::Generation)
    {
        if (raw.starts_with("W/"))
            return std::nullopt;
        if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
            raw = raw.substr(1, raw.size() - 2);
    }
    if (!isOpaqueTag(dialect, raw))
        return std::nullopt;
    return String(raw);
}

}

std::string_view dialectWord(Dialect dialect)
{
    return kTokenTypeWords.toWord(dialect, "dialectWord");
}

std::optional<Dialect> dialectFromWord(std::string_view word)
{
    for (const auto & entry : kTokenTypeWords.entries)
        if (word == entry.word)
            return entry.value;
    return std::nullopt;
}

std::optional<CanonicalIdentity> canonicalIdentity(Dialect dialect, std::string_view raw_value)
{
    auto tag = opaqueTagOfWireValue(dialect, raw_value);
    if (!tag)
        return std::nullopt;
    return CanonicalIdentity{dialect, hexString(tag->data(), tag->size())};
}

std::optional<String> opaqueTagFromHex(Dialect dialect, std::string_view tag_hex)
{
    if (tag_hex.empty() || tag_hex.size() % 2 != 0 || !std::all_of(tag_hex.begin(), tag_hex.end(), isLowercaseHexChar))
        return std::nullopt;
    String tag;
    tag.reserve(tag_hex.size() / 2);
    for (size_t i = 0; i < tag_hex.size(); i += 2)
        tag.push_back(static_cast<char>(unhex2(tag_hex.data() + i)));
    if (!isOpaqueTag(dialect, tag))
        return std::nullopt;
    return tag;
}

std::optional<PersistedEtag> persistedTokenOf(const CanonicalIdentity & identity)
{
    const std::optional<String> tag = opaqueTagFromHex(identity.dialect, identity.tag_hex);
    if (!tag)
        return std::nullopt;
    /// Quoted: a bare tag that starts with `W/` would read back as a weak ETag.
    String value = identity.dialect == Dialect::Generation ? *tag : "\"" + *tag + "\"";
    return PersistedEtag{String(dialectWord(identity.dialect)), std::move(value)};
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

bool sameVersion(const Etag & read, const Etag & written)
{
    if (read.key() != written.key() || read.backendId() != written.backendId())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "sameVersion compares tokens of two objects: '{}' and '{}'",
                        read.key(), written.key());
    if (identicalToken(read, written))
        return true;
    const std::optional<CanonicalIdentity> identity = read.canonical();
    return identity && identity == written.canonical();
}

bool identicalToken(const Etag & a, const Etag & b)
{
    return a.backendId() == b.backendId() && a.key() == b.key() && a.dialect() == b.dialect() && a.value() == b.value();
}

bool identicalToken(const std::optional<Etag> & a, const std::optional<Etag> & b)
{
    return a && b ? identicalToken(*a, *b) : a.has_value() == b.has_value();
}

}
