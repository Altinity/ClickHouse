#pragma once

#include <base/types.h>

#include <string_view>

namespace DB
{

/// Bump for every Antalya-only wire protocol change. See `docs/en/antalya/protocol.md`.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION = 1;

namespace AntalyaProtocol
{

String appendMarker(std::string_view name);

/// Returns the negotiated version, or `0` if there is no marker.
UInt64 parseMarker(std::string_view name);

}

}
