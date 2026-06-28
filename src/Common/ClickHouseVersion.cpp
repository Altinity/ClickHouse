#include <Common/ClickHouseVersion.h>

#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>

#include <boost/algorithm/string.hpp>

#include <fmt/ranges.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

ClickHouseVersion::ClickHouseVersion(std::string_view version)
{
    Strings split;
    boost::split(split, version, [](char c){ return c == '.'; });
    if (split.empty())
        throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version: {}", version};

    size_t last_component;
    bool last_is_numeric = !split.back().empty();
    if (last_is_numeric)
    {
        ReadBufferFromString buf(split.back());
        last_is_numeric = tryReadIntText(last_component, buf) && buf.eof();
    }

    if (!last_is_numeric)
    {
        /// A trailing non-numeric token is a build suffix (e.g. "26.1.3.20001.altinityantalya").
        /// Only that exact suffix, preceded by exactly 4 numeric components, is accepted
        suffix = split.back();
        split.pop_back();
        if (suffix != "altinityantalya" || split.size() != 4)
            throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version: {}", version};
    }
    else if (split.size() < 2 || split.size() > 4)
    {
        throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version: {}", version};
    }

    components.reserve(split.size());
    for (const auto & token : split)
    {
        size_t component;
        ReadBufferFromString buf(token);
        if (token.empty() || !tryReadIntText(component, buf) || !buf.eof())
            throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version: {}", version};
        components.push_back(component);
    }
}

String ClickHouseVersion::toString() const
{
    String result = fmt::format("{}", fmt::join(components, "."));
    if (!suffix.empty())
        result += "." + suffix;
    return result;
}

std::strong_ordering ClickHouseVersion::operator<=>(const ClickHouseVersion & other) const
{
    if (auto cmp = components <=> other.components; cmp != 0)
        return cmp;
    return suffix <=> other.suffix;
}

}
