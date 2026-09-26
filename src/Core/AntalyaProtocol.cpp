#include <Core/AntalyaProtocol.h>

#include <charconv>

#include <algorithm>


namespace DB
{

namespace AntalyaProtocol
{

constexpr std::string_view MARKER_PREFIX = " (antalya:";
constexpr size_t MAX_MARKER_DIGITS = 9;
constexpr size_t MAX_MARKER_SIZE = MARKER_PREFIX.size() + MAX_MARKER_DIGITS + 1;
constexpr UInt64 MAX_MARKER_VERSION = 999999999;

static_assert(
    DBMS_ANTALYA_PROTOCOL_VERSION >= 1 && DBMS_ANTALYA_PROTOCOL_VERSION <= MAX_MARKER_VERSION,
    "DBMS_ANTALYA_PROTOCOL_VERSION does not fit the marker grammar");

String appendMarker(std::string_view name)
{
    String result;
    result.reserve(name.size() + MAX_MARKER_SIZE);
    result.append(name);
    result.append(MARKER_PREFIX);
    result.append(std::to_string(DBMS_ANTALYA_PROTOCOL_VERSION));
    result.push_back(')');
    return result;
}

UInt64 stripMarker(String & name)
{
    if (name.empty() || name.back() != ')')
        return 0;

    const size_t marker_pos = name.rfind(MARKER_PREFIX);
    if (marker_pos == String::npos)
        return 0;

    const size_t first_digit = marker_pos + MARKER_PREFIX.size();
    const size_t digits = name.size() - first_digit - 1;
    if (digits == 0 || digits > MAX_MARKER_DIGITS || name[first_digit] == '0')
        return 0;

    UInt64 version;
    const char * begin = name.data() + first_digit;
    const char * end = name.data() + name.size() - 1;
    const auto result = std::from_chars(begin, end, version);
    if (result.ec != std::errc{} || result.ptr != end)
        return 0;

    name.resize(marker_pos);
    return std::min<UInt64>(version, DBMS_ANTALYA_PROTOCOL_VERSION);
}

}

}
