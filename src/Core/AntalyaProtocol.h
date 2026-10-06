#pragma once

#include <base/types.h>

#include <string_view>

namespace DB
{

/// 2 — `ReadTaskResponse` carries optional `DataFileMetaInfo`, then optional `retry_after_us`,
/// after the upstream payload. Both fields ship in the same release.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION_WITH_READ_TASK_EXTENSIONS = 2;
/// Bump for every Antalya-only wire protocol change. See `docs/en/antalya/protocol.md`.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION = DBMS_ANTALYA_PROTOCOL_VERSION_WITH_READ_TASK_EXTENSIONS;

namespace AntalyaProtocol
{

String appendMarker(std::string_view name);

/// Returns the negotiated version, or `0` if there is no marker.
UInt64 parseMarker(std::string_view name);

/// Returns `name` without the marker, or `name` itself if there is no marker.
std::string_view removeMarker(std::string_view name);

}

}
