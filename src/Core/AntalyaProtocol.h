#pragma once

#include <base/types.h>

#include <string_view>

namespace DB
{

/// Bump for every Antalya-only wire protocol change. See `docs/en/antalya/protocol.md`.
/// 2: the `*Cluster` read-task payload carries Iceberg delete-file paths as absolute URIs.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH = 2;
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION = DBMS_ANTALYA_PROTOCOL_VERSION_WITH_ICEBERG_ABSOLUTE_PATH;

namespace AntalyaProtocol
{

String appendMarker(std::string_view name);

/// Returns the negotiated version, or `0` if there is no marker.
UInt64 parseMarker(std::string_view name);

/// Returns `name` without the marker, or `name` itself if there is no marker.
std::string_view removeMarker(std::string_view name);

}

}
