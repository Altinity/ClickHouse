---
description: 'How the Antalya fork versions its own wire-protocol changes independently of upstream ClickHouse'
sidebar_label: 'Antalya Protocol Version'
sidebar_position: 40
slug: /antalya/protocol
title: 'Antalya Protocol Version'
doc_type: 'reference'
---

# Antalya protocol version {#antalya-protocol-version}

Antalya versions its own wire-protocol changes with `DBMS_ANTALYA_PROTOCOL_VERSION`, a counter that
upstream ClickHouse cannot reach, defined in `src/Core/AntalyaProtocol.h`. A server advertises it in
the `ServerHello` name string, on every connection:

```text
server -> client   "ClickHouse (antalya:1)"
```

The client strips the suffix, caps the value with `min(own, server)` and keeps the result. `0` means
the peer is not an Antalya build. Negotiation is per hop and not transitive: initiator to worker and
worker to worker negotiate independently.

Version 1 is the advertisement itself. Nothing is gated on it yet.

## Adding an Antalya-only wire change {#adding-a-wire-change}

- Bump `DBMS_ANTALYA_PROTOCOL_VERSION` by one and gate the change on the negotiated value.
- Never bump `DBMS_TCP_PROTOCOL_VERSION`, and never take a slot in
  `DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION` for a feature upstream does not have.
- Keep the counter cumulative. A backport takes the whole contiguous range up to the value it needs,
  or does not bump at all - the `min(own, server)` cap is only sound for a cumulative feature set.
- Gate only what the *client* decides to do. The server never learns the client's version, because
  only the server advertises.
- Document it here. Also document it in `docs/en/interfaces/specs/NativeProtocol.md` if it changes
  the layout of a packet that file describes field by field: a third-party client (`ch-go`,
  `clickhouse-go`, `clickhouse-driver`) that does not know an added field cannot parse the stream
  past it. Everything else stays here, so that the spec keeps tracking upstream through rebases.

## Why a counter of our own {#why-a-counter-of-our-own}

An Antalya-only change that takes a slot in an upstream counter has to be renumbered whenever
upstream claims that slot for something else. The same number then means two different things in two
shipped builds, and two nodes that negotiate it disagree about the bytes on the wire.

The constant lives in its own header rather than in `src/Core/ProtocolDefines.h`, whose tail is
where upstream adds its own constants, and therefore where every rebase conflicts.

## Why the marker rides in `ServerHello` {#why-the-marker-rides-in-serverhello}

**The client `Hello` is never marked.** The client writes that packet before it has read a byte from
the peer, so it cannot gate a marker on what the peer is. Every field in it - `client_name`,
`default_database`, `user`, `password` - is one an upstream server acts on. `client_name` is the
worst: a server persists it, and `validate_tcp_client_information` compares it against the Query
packet's `ClientInfo`, so a marker there fails a `remote()` query with `CLIENT_INFO_DOES_NOT_MATCH`
on any peer that does not strip it.

`ServerHello` has no such field. `server_name` is client-side display text and reaches no system
table, which is why the server is the side that speaks.

**The marker is a suffix, not extra bytes.** Neither Hello nor the Addendum has a length prefix or a
terminator: a reader stops after the last field its negotiated revision knows about, so a peer that
does not expect extra bytes reads them as the next packet.

## What you see {#what-you-see}

An Antalya client appends the negotiated version to its existing connection log line
(`Connected to ... server version ...`). The server logs nothing, and degradation to `0` is silent.

Nothing a server stores changes: `system.query_log` and `system.processes` keep the `client_name`
the peer sent, so filters written as `client_name = 'ClickHouse server'` keep working.

The suffix is visible in one place - an **upstream** client's `server_name`, which that client does
not strip, so its banner reads `Connected to ClickHouse (antalya:1) server version ...`.

## No opt-out {#no-opt-out}

No setting suppresses the marker: a node either speaks this protocol or is not an Antalya build.

`server_name` is already a build-time value - upstream sets it from the CMake project name, and any
fork may set it to anything - so a native client cannot treat it as a fixed string. What a switch
would protect is the displayed name on a peer that does not strip the suffix, which is cosmetic, and
it would cost a state in which two Antalya nodes that both support a feature fail to negotiate it
because one was configured not to say so. If a peer is ever found that breaks on the suffix, the fix
is to stop appending it.

## Implementation {#implementation}

`src/Core/AntalyaProtocol.h` holds the version constant, the marker grammar and the `appendMarker` /
`parseMarker` / `stripMarker` / `negotiate` helpers. `TCPHandler::sendHello` appends the marker;
`Connection::receiveHello` strips it and stores the capped value.
