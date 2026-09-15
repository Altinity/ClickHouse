#!/usr/bin/env python3
# Tags: no-fasttest, no-llvm-coverage
"""The `server_name` an Antalya server puts in its Hello, read straight off the wire.

`TCPHandler::sendHello` appends the marker to every `ServerHello`. It cannot do otherwise: the
client writes its own `Hello` first, so there is nothing about the peer for the server to condition
on, and the client `Hello` carries no marker to condition on either. Nothing the client sends may
change the reply. See `src/Core/AntalyaProtocol.h`.
"""

import os
import re
import socket
import struct

CLICKHOUSE_PORT = int(os.environ.get("CLICKHOUSE_PORT_TCP", 9000))
CLICKHOUSE_HOST = os.environ.get("CLICKHOUSE_HOST", "127.0.0.1")

CLIENT_HELLO = 0
SERVER_HELLO = 0
SERVER_EXCEPTION = 2
CLIENT_REVISION = 54449

UPSTREAM_SERVER_NAME = "ClickHouse"
MARKED_SERVER_NAME = re.compile(r"^(.*) \(antalya:[1-9][0-9]{0,8}\)$")

# Anything a client might put in the name field, including strings that already look marked and
# strings that nearly parse as one. None of it may change what comes back.
CLIENT_NAMES = [
    "ClickHouse client",
    "ClickHouse server",
    "ClickHouse test",
    "",
    "ClickHouse server (antalya:1)",
    "ClickHouse server (antalya:999999999)",
    "ClickHouse server (antalya:01)",
    "ClickHouse server (ANTALYA:1)",
]


def write_varuint(value):
    result = bytearray()
    while value > 0x7F:
        result.append(0x80 | (value & 0x7F))
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)


def write_string(s):
    data = s.encode("utf-8")
    return write_varuint(len(data)) + data


def recv_exact(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Connection closed")
        data += chunk
    return data


def read_varuint(sock):
    result = 0
    shift = 0
    while True:
        byte = recv_exact(sock, 1)[0]
        result |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return result
        shift += 7


def read_string(sock):
    length = read_varuint(sock)
    return recv_exact(sock, length).decode("utf-8") if length else ""


def server_name_for(client_name):
    """Send a Hello carrying `client_name` and return the `server_name` the server replies with."""
    with socket.create_connection((CLICKHOUSE_HOST, CLICKHOUSE_PORT), timeout=30) as sock:
        pkt = bytearray()
        pkt += write_varuint(CLIENT_HELLO)
        pkt += write_string(client_name)
        pkt += write_varuint(25)  # version_major
        pkt += write_varuint(1)  # version_minor
        pkt += write_varuint(CLIENT_REVISION)
        pkt += write_string("")  # default database
        pkt += write_string("default")  # user
        pkt += write_string("")  # password
        sock.sendall(pkt)

        pkt_type = read_varuint(sock)
        if pkt_type == SERVER_EXCEPTION:
            code = struct.unpack("<I", recv_exact(sock, 4))[0]
            name = read_string(sock)
            raise Exception(f"Server exception {code}: {name}: {read_string(sock)}")
        assert pkt_type == SERVER_HELLO, f"Expected Hello, got {pkt_type}"
        return read_string(sock)


def test_the_reply_does_not_depend_on_the_client_name():
    """A client-gated reply is what would force the client to mark itself first, so pin it shut."""
    replies = {client_name: server_name_for(client_name) for client_name in CLIENT_NAMES}
    distinct = set(replies.values())
    assert len(distinct) == 1, f"the reply varies with the client name: {replies}"
    print("the reply does not depend on the client name")


def test_the_marker_is_appended_to_the_upstream_name():
    """The marker is a suffix, so stripping it yields exactly what upstream would have sent."""
    name = server_name_for("ClickHouse client")
    match = MARKED_SERVER_NAME.match(name)
    assert match, f"got server_name {name!r}"
    assert match.group(1) == UPSTREAM_SERVER_NAME, f"got server_name {name!r}"
    print("the marker is appended to the upstream server name")


def main():
    test_the_reply_does_not_depend_on_the_client_name()
    test_the_marker_is_appended_to_the_upstream_name()


if __name__ == "__main__":
    main()
