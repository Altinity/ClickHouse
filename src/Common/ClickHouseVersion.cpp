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
    components.reserve(split.size());
    if (split.empty())
        throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version here: {}", version};

    for (size_t i = 0; i < split.size(); ++i)
    {
        size_t component;
        ReadBufferFromString buf(split[i]);
        if (!tryReadIntText(component, buf) || !buf.eof())
        {
            /// A non-numeric part is only allowed as a non-empty terminal suffix after at least
            /// one numeric component (e.g. "altinityantalya" in "26.1.3.20001.altinityantalya").
            const bool is_terminal = (i + 1 == split.size());
            if (components.empty() || !is_terminal || split[i].empty())
                throw Exception{ErrorCodes::BAD_ARGUMENTS, "Cannot parse ClickHouse version here: {}", version};
            suffix = split[i];
            break;
        }
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
