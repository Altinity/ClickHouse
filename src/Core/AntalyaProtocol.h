#pragma once

#include <base/types.h>

#include <string_view>


namespace DB
{

/// Counter for Antalya-only wire features, independent of every upstream counter in
/// `Core/ProtocolDefines.h`, so that a rebase can never renumber an Antalya feature. Bump it by one
/// per feature. The client caps the server's value with `min(own, server)`, so the feature set must
/// stay cumulative: a backport takes the whole contiguous range up to the value it needs, or does
/// not bump at all. See `docs/en/antalya/protocol.md`.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION = 1;

namespace AntalyaProtocol
{

/// The version is advertised in the `ServerHello` name string - "ClickHouse (antalya:M)" - and
/// nowhere else: the client `Hello` is never marked, because `client_name` reaches the peer's
/// `system.query_log` and `validate_tcp_client_information`. See `docs/en/antalya/protocol.md`.

/// " (antalya:" <digits> ")", where <digits> is [1-9][0-9]{0,8}.
inline constexpr std::string_view MARKER_PREFIX = " (antalya:";
inline constexpr char MARKER_TERMINATOR = ')';
inline constexpr size_t MAX_MARKER_DIGITS = 9;
inline constexpr size_t MAX_MARKER_SIZE = MARKER_PREFIX.size() + MAX_MARKER_DIGITS + 1;

String appendMarker(std::string_view name);

/// Returns 0 unless `name` ends with exactly one canonical marker. Scans at most `MAX_MARKER_SIZE`
/// bytes back from the end, because the client runs it on a `server_name` it has not authenticated.
UInt64 parseMarker(std::string_view name) noexcept;

/// Removes a trailing canonical marker from `name` and returns the version it spelled; returns 0 and
/// leaves `name` alone when there is none.
UInt64 stripMarker(String & name);

/// The version to speak with a server that advertised `peer_version`; 0 means it is not Antalya.
UInt64 negotiate(UInt64 peer_version) noexcept;

}

}
