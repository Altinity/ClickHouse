#pragma once

#include <base/types.h>

#include <string_view>


namespace DB
{

/// ============================== ANTALYA ONLY ==============================
/// One counter for all Antalya-only wire features, independent of every upstream counter in
/// `Core/ProtocolDefines.h`. It is advertised inside the `ServerHello` name string, which a client
/// only displays, so a rebase can never renumber an Antalya feature.
///
/// Bump it by exactly one per feature and never reuse a value; never bump an upstream counter for
/// an Antalya-only change. The client caps the server's value with `min(own, server)`, so the
/// feature set must stay cumulative: a backport takes the whole contiguous range up to the value it
/// needs, or does not bump at all. See `docs/en/antalya/protocol.md`.
///
/// Version 1: the advertisement itself. No wire payload beyond the marker.
static constexpr auto DBMS_ANTALYA_PROTOCOL_VERSION = 1;
/// ==========================================================================

namespace AntalyaProtocol
{

/// The version travels inside the `ServerHello` name string, in one direction only:
///
///     server -> client   "ClickHouse (antalya:M)"   (every connection, unconditionally)
///
/// The client strips the marker while parsing, so it never reaches anything that stores or displays
/// the name. The client `Hello` is never marked: `client_name` is the one field a peer persists to
/// `system.query_log` and compares in `validateClientInfo`, and the client writes it before it
/// knows anything about the peer. See `docs/en/antalya/protocol.md`.

/// " (antalya:" <digits> ")", where <digits> is [1-9][0-9]{0,8}.
inline constexpr std::string_view MARKER_PREFIX = " (antalya:";
inline constexpr char MARKER_TERMINATOR = ')';
inline constexpr size_t MAX_MARKER_DIGITS = 9;
inline constexpr size_t MAX_MARKER_SIZE = MARKER_PREFIX.size() + MAX_MARKER_DIGITS + 1;

/// Returns `name` with our own version's marker appended.
String appendMarker(std::string_view name);

/// Strict, anchored suffix parse: returns 0 unless `name` ends with exactly one canonical marker.
/// Scans at most `MAX_MARKER_SIZE` bytes back from the end - the client runs this on a `server_name`
/// it has not authenticated, where an unbounded scan would be a CPU amplifier.
UInt64 parseMarker(std::string_view name) noexcept;

/// If `name` ends with a canonical marker, removes it and returns the version it spelled; otherwise
/// leaves `name` alone and returns 0. The marker is negotiation metadata rather than part of the
/// name, so the receiver strips it before the name is stored or displayed.
UInt64 stripMarker(String & name);

/// The version to speak with a server that advertised `peer_version`; 0 means the server is not
/// Antalya.
UInt64 negotiate(UInt64 peer_version) noexcept;

}

}
