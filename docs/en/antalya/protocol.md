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
- Update this page, and update `docs/en/interfaces/specs/NativeProtocol.md` when the change alters
  a packet layout described there.

## Why a counter of our own {#why-a-counter-of-our-own}

An upstream rebase can reuse the next value of an upstream protocol counter for a different feature.
Keeping the Antalya counter separate prevents the same version from describing two wire layouts.

## Why the marker rides in `ServerHello` {#why-the-marker-rides-in-serverhello}

The client `Hello` cannot advertise the version because it is sent before the peer is known. Its
`client_name` is also stored and validated against the Query packet, so changing it can raise
`CLIENT_INFO_DOES_NOT_MATCH` on an upstream peer.

The server advertises through `server_name`, which is display text. The marker stays inside that
existing string because adding a field would make older peers read it as the next packet.
