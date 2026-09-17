#include <Core/AntalyaProtocol.h>

#include <Common/StringUtils.h>
#include <Common/intExp10.h>

#include <algorithm>


namespace DB
{

namespace AntalyaProtocol
{

constexpr UInt64 MAX_VERSION = intExp10(static_cast<int>(MAX_MARKER_DIGITS)) - 1;

static_assert(
    DBMS_ANTALYA_PROTOCOL_VERSION >= 1 && DBMS_ANTALYA_PROTOCOL_VERSION <= MAX_VERSION,
    "DBMS_ANTALYA_PROTOCOL_VERSION does not fit the marker grammar");

String appendMarker(std::string_view name)
{
    String result;
    result.reserve(name.size() + MAX_MARKER_SIZE);
    result.append(name);
    result.append(MARKER_PREFIX);
    result.append(std::to_string(DBMS_ANTALYA_PROTOCOL_VERSION));
    result.push_back(MARKER_TERMINATOR);
    return result;
}

UInt64 parseMarker(std::string_view name) noexcept
{
    if (name.empty() || name.back() != MARKER_TERMINATOR)
        return 0;

    const size_t close = name.size() - 1;

    size_t first_digit = close;
    while (first_digit > 0 && isNumericASCII(name[first_digit - 1]))
    {
        --first_digit;
        if (close - first_digit > MAX_MARKER_DIGITS)
            return 0;
    }

    const size_t digits = close - first_digit;
    /// Exactly one spelling per version: no empty digit run, no leading zero.
    if (digits == 0 || name[first_digit] == '0')
        return 0;

    if (first_digit < MARKER_PREFIX.size())
        return 0;
    if (name.substr(first_digit - MARKER_PREFIX.size(), MARKER_PREFIX.size()) != MARKER_PREFIX)
        return 0;

    UInt64 version = 0;
    for (size_t i = first_digit; i < close; ++i)
        version = version * 10 + static_cast<UInt64>(name[i] - '0');
    return version;
}

UInt64 stripMarker(String & name)
{
    const UInt64 version = parseMarker(name);
    if (version != 0)
    {
        /// Only the canonical spelling parses, so the digits on the wire are the ones `to_string` gives.
        name.resize(name.size() - MARKER_PREFIX.size() - std::to_string(version).size() - 1);
    }
    return version;
}

UInt64 negotiate(UInt64 peer_version) noexcept
{
    return std::min<UInt64>(peer_version, DBMS_ANTALYA_PROTOCOL_VERSION);
}

}

}
