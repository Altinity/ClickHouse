---
description: 'How the Antalya fork versions its own wire-protocol changes independently of upstream ClickHouse'
sidebar_label: 'Antalya Protocol Version'
sidebar_position: 40
slug: /antalya/protocol
title: 'Antalya Protocol Version'
doc_type: 'reference'
---

# Antalya protocol version {#antalya-protocol-version}

## Why a separate counter {#why-a-separate-counter}

Antalya carries features that do not exist in upstream ClickHouse, and it rebases onto each new
upstream release. An Antalya-only wire change that takes a slot in an upstream counter -
`DBMS_TCP_PROTOCOL_VERSION` or `DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION` - has to be renumbered
whenever upstream claims that slot for something else. The same number then means two different
things in two shipped builds, and two nodes that negotiate it disagree about the bytes on the wire.

`DBMS_ANTALYA_PROTOCOL_VERSION` (in `src/Core/AntalyaProtocol.h`) is a counter in a number space
that upstream cannot reach, so a rebase can never renumber an Antalya feature. It lives in its own
header rather than in `src/Core/ProtocolDefines.h`, whose tail is where upstream adds its own
constants and therefore where every rebase conflicts.

## The rule {#the-rule}

An Antalya-only wire change bumps `DBMS_ANTALYA_PROTOCOL_VERSION` and gates itself on the
negotiated value. It never bumps an upstream counter and never inserts a slot into
`DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION`.

The client caps the server's value with `min(own, server)`, which is only sound while the counter
describes a cumulative feature set. A backport must therefore take the whole contiguous range up to
the value it needs, or not bump at all.

## How it is advertised {#how-it-is-advertised}

The version travels inside the `ServerHello` *name* string, in one direction only:

```text
server -> client   "ClickHouse (antalya:M)"   (every connection, unconditionally)
```

The client caps it with its own value and strips the marker while parsing, so `server_name` reads
exactly as it would coming from upstream. A value of `0` means the server is not Antalya.

Nothing is appended in the other direction, and the client `Hello` is never marked. The client
writes that packet before it has read a byte from the peer, so it cannot gate a marker on what the
peer is, and every field in it - `client_name`, `default_database`, `user`, `password` - is one an
upstream server acts on. `client_name` is the worst of them: a server persists it and
`validate_tcp_client_information` compares it against the Query packet's `ClientInfo`, so a marker
there fails a `remote()` query with `CLIENT_INFO_DOES_NOT_MATCH` on any peer that does not strip it.
The `ServerHello` has no such field - `server_name` is client-side display text and reaches no
system table - which is why the server is the side that speaks.

The marker lives inside an existing string rather than in appended bytes because neither Hello nor
the Addendum has a length prefix or a terminator: a reader stops after the last field its negotiated
revision knows about, and a peer that does not expect extra bytes reads them as the next packet.

## What version 1 supports {#what-version-1-supports}

Version 1 is the advertisement itself; there is no wire payload beyond the marker. Because only the
server advertises, a feature gated on this version can only be one the *client* decides to use.

A feature that needs the server to know the client's version needs a channel the client writes after
it has read the `ServerHello` - by then it knows the peer is Antalya - and that an upstream server
would never read. The Addendum is not that channel: `TCPHandler::receiveAddendum` reads a fixed
field list, and an Antalya server cannot tell a client that wrote an extra field from one that did
not, so it would read bytes that are not there and desynchronise the stream. A new Antalya-only
client packet type, numbered far above `Protocol::Client::MAX` and sent right after the Addendum,
is: an upstream client never sends it, and an Antalya client sends it only when the negotiated
version is at least the one that introduced it. That costs a version bump when it is first needed
and nothing today.

## Scope {#scope}

Every connection to an Antalya server is advertised to, `clickhouse-client` included. Any Antalya
client reads the value - `clickhouse-client`, Distributed, `*Cluster`, swarm, parallel replicas -
and an upstream client ignores it.

Negotiation is per hop and not transitive: initiator to worker and worker to worker negotiate
independently.

## Observability {#observability}

The negotiated version, when non-zero, is appended to the client's existing connection log line
(`Connected to ... server version ...`). The server logs nothing about it, because it learns nothing
about the peer. Degradation to `0` is otherwise silent.

No Antalya string reaches anything a server stores. `client_name` is never marked, so
`system.query_log` and `system.processes` are unaffected and filters written as
`client_name = 'ClickHouse server'` keep working.

The marker is visible in exactly one place: an **upstream** client's `server_name`, which that
client does not strip. An upstream `clickhouse-client` prints
`Connected to ClickHouse (antalya:1) server version ...` in its interactive banner. An Antalya
client strips the marker and prints what upstream would have printed.

## Implementation {#implementation}

`src/Core/AntalyaProtocol.h` holds the version constant, the marker grammar and the `appendMarker`
/ `parseMarker` / `stripMarker` / `negotiate` helpers. The parse is an anchored suffix scan bounded
to the marker length, because the client runs it on a `server_name` it has not authenticated.
`TCPHandler::sendHello` appends the marker; `Connection::receiveHello` strips it and stores the
capped value.
