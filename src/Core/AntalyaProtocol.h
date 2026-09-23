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

/// Removes a valid trailing marker and returns the negotiated version, or `0` if there is no marker.
UInt64 stripMarker(String & name);

}

}
