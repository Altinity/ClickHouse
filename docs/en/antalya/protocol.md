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
upstream ClickHouse cannot reach, defined in `src/Core/AntalyaProtocol.h`. Both sides advertise it in
the name string of their `Hello`, on every connection:

```text
client -> server   "ClickHouse client (antalya:2)"
server -> client   "ClickHouse (antalya:2)"
```

Each side parses the peer's suffix, caps the value with `min(own, peer)` and keeps the result. `0`
means the peer is not an Antalya build. Negotiation is per hop and not transitive: initiator to
worker and worker to worker negotiate independently.

Version 1 is the advertisement itself.

Version 2 appends optional Iceberg column statistics (`DataFileMetaInfo`) to `ReadTaskResponse`,
after the upstream cluster-processing payload.

## Adding an Antalya-only wire change {#adding-a-wire-change}

- Bump `DBMS_ANTALYA_PROTOCOL_VERSION` by one and gate the change on the negotiated value.
- Never bump `DBMS_TCP_PROTOCOL_VERSION`, and never take a slot in
  `DBMS_CLUSTER_PROCESSING_PROTOCOL_VERSION` for a feature upstream does not have.
- Keep the counter cumulative. A backport takes the whole contiguous range up to the value it needs,
  or does not bump at all - the `min(own, server)` cap is only sound for a cumulative feature set.
- Update this page, and update `docs/en/interfaces/specs/NativeProtocol.md` when the change alters
  a packet layout described there.

## Why a counter of our own {#why-a-counter-of-our-own}

An upstream rebase can reuse the next value of an upstream protocol counter for a different feature.
Keeping the Antalya counter separate prevents the same version from describing two wire layouts.

## Why the marker rides in the `Hello` name {#why-the-marker-rides-in-the-hello-name}

The marker stays inside the existing name string because adding a field would make older peers read
it as the next packet. The client writes its `Hello` before it knows the peer, so it always marks
`client_name`. With `validate_tcp_client_information` enabled, an upstream server rejects an initial
query from an Antalya client with `CLIENT_INFO_DOES_NOT_MATCH`; an Antalya server ignores the marker.
