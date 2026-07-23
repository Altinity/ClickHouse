---
description: 'Design for the CAS fetch-handoff retention pin: a sender-created, receiver-build-owned pin cleaned by the server''s `min_active` heartbeat build-watermark floor, sealing the commit-before-release gap in same-pool fetch-by-relink (#42/#43) by keeping a fetched part''s blobs alive across the sender-release → receiver-commit handoff, reusing the GC retention-overlay primitive shared with the read-only replica design. Generalizes to bulk write-replica warm-up.'
sidebar_label: 'CAS Fetch-Handoff Retention Pin'
sidebar_position: 20260715
slug: /superpowers/specs/cas-fetch-handoff-retention-pin-design
title: 'CAS Fetch-Handoff Retention Pin — commit-before-release for relink'
doc_type: 'reference'
---

# CAS fetch-handoff retention pin — commit-before-release for relink {#cas-fetch-handoff-pin}

> **SUPERSEDED (2026-07-23).** Replaced by
> [`2026-07-23-cas-fetch-handoff-reserved-precommit-design.md`](2026-07-23-cas-fetch-handoff-reserved-precommit-design.md):
> the same commit-before-release seal is achieved with a **reserved precommit** in the receiver's own
> journal plus sender-side manifest materialization before part release — pure event ordering, no
> pool-global pin objects, no GC retention overlay, no removal-deferral. The transport analysis
> (§Motivation: half-duplex rules out sender-side ACKs) remains valid and is inherited. The shared
> GC retention-overlay primitive (§The shared primitive) now has a single prospective consumer — the
> read-only replica design — and should be justified there alone. Kept for history.

**Date:** 2026-07-15
**Branch:** `cas-gc-rebuild`
**Status:** design (extracted 2026-07-15 from the read-replica spec §8.1 after the mechanism converged in
discussion; single-part protocol is v1, bulk warm-up is a marked future extension)
**Depends on / interacts with:** the GC retention-overlay primitive introduced by the read-only replica
design ([`2026-07-14-cas-readonly-replica-snapshot-pin-design.md`](2026-07-14-cas-readonly-replica-snapshot-pin-design.md)
§5); the ref snapshot+log model ([`2026-07-11-cas-ref-table-snapshot-log-design.md`](2026-07-11-cas-ref-table-snapshot-log-design.md));
same-pool fetch-by-relink (`DataPartsExchange` `Service::processQuery` / `Fetcher::relinkPartToDisk` /
`adoptPartFromManifest`); the durable-monotone `writer_epoch` / `build_sequence` identity.

## Motivation {#motivation}

Same-pool replication uses **fetch-by-relink**: instead of streaming a part's bytes, the source sends the
part's manifest and the receiver publishes its own ref pointing at the already-shared blobs (zero byte
cost). The correctness hole (`#42`/`#43`, reports `2026-07-14-cas-adoptevidence-relink-lifecycle-exposure`):

- The relink **sender is fire-and-forget**: `Service::processQuery` writes the manifest and returns
  (`DataPartsExchange.cpp:256-259`), releasing its `DataPartPtr` **before** the receiver's
  `adoptPartFromManifest` commit (`:720`). There is **no `in-degree` overlap** between the two refs.
- Data loss requires the receiver's `precommitAdd` edge-PUT to **stall across ≥ 2 GC folds** while the
  source's now-`Outdated` part is concurrently collected and the blob has no other ref — a **deep tail**.
- `deleteExact` already covers every token-**CHANGE** recovery (a resurrect displaces to a fresh token →
  `TokenMismatch` no-op). Only this **same-token** tail (relink re-references at the condemn-time token)
  remains, and fsck detects the resulting dangle.

We want a seal that is **airtight** (not merely deep-tail-improbable), **cheap** (no byte cost added to
relink), **observation-based** (no wall-clock TTL — consistent with the rev.6 liveness model), and
**reuses existing GC machinery** rather than adding bespoke handshake protocol to the replication path.

We explored and rejected transport-level seals: a blocking sender-ACK, an in-band response suffix, HTTP
trailers, and a full-duplex reverse channel are all impossible or unreliable — the Poco interserver
transport is **half-duplex** (client sends request params, then only reads the response), and any
response-side signal (bytes/suffix/trailer) is sender→receiver and buffered, so it carries neither a
reverse ACK nor backpressure. The seal therefore lives in the **GC/pin layer**, not the transport.

## Relationship to the read-only replica design {#relationship}

This mechanism is **orthogonal** to the read-only replica / WORM feature. They share **exactly one**
thing — the GC retention-overlay primitive (§[The shared primitive](#primitive)) — and differ on every
other axis:

| Axis | Reader / WORM pin (read-replica spec) | Fetch-handoff pin (this spec) |
|---|---|---|
| Owner | a reader **mount** (a node serving SELECTs) | a **build** of the receiving writer-replica |
| Payload | snapshot-window `{ns, S_min, L_max}` | a manifest **closure** (blob set); many closures / a window for warm-up |
| Lifetime | long — `max_execution_time`; heartbeated | short — one fetch; no heartbeat |
| Self-clean | reader-lease heartbeat (TTL) | **`min_active`** heartbeat build-watermark floor |
| Publisher | the reader itself | the **sender**, on behalf of the receiver's build |
| Extra deps | reader-mount mode + readonly-refresh | **none** beyond the primitive |

Consequence: the fetch-handoff fix needs **only** the retention-overlay primitive plus a small fetch
protocol extension — **not** the reader-mount mode, the snapshot-window pin, the heartbeat-lease, or the
readonly-refresh reuse. It is therefore a much smaller, **independently shippable** change than the
read-replica feature, and can land as soon as the primitive exists. The two are siblings on top of one
primitive, not one inside the other.

## The shared GC retention-overlay primitive {#primitive}

Both consumers rely on one GC capability (fully specified in read-replica spec §5; summarized here so
this spec is self-contained):

- A **pin** is a durable, pool-global object (under `gc/`, where the blob-reclaiming GC already scans),
  outside any writer `srid` ref subtree. Each pin carries (a) a **pinned edge-set** and (b) a **liveness
  owner**.
- GC folds `current refs ∪ ⋃(pinned edge-sets of live-owner pins)`. A blob is condemnable **iff** its
  source-edge set is empty across that union. Pins whose owner GC observes as dead are **dropped before
  folding** (they contribute nothing).
- The fold is incremental, not O(closure)-per-round: because `in-degree` is already a set of source
  edges, a `-1` edge whose `+1` lies inside a pinned set is **deferred** and applied once the pin lifts
  (read-replica §5.2 "incremental removal-deferral"). Per-round cost is O(newly-applicable removals).
- A **pinned edge-set may be expressed two ways**, both resolving to edges: an **explicit
  manifest-closure** (the natural fetch shape) or a **snapshot-window** (the natural reader shape, and
  the cheap warm-up shape). The primitive is agnostic to which; ownership and liveness are the axes that
  distinguish consumers, not the payload shape.

This spec adds a second **owner kind** to that primitive: a **build-owned** pin cleaned by the server's
`min_active` heartbeat build-watermark floor (§[Liveness](#liveness)), alongside the reader's mount-owned,
lease-TTL-cleaned pin.

## Fetch-handoff protocol — single part (v1) {#protocol}

A minimal extension to the existing relink request/response — **one field each way**, both riding the
current half-duplex round-trip; **no second interserver request**:

1. **Request** (receiver → sender): the existing relink request additionally carries the receiver's
   `{server_root_id, build_id}` (its durable-monotone build/epoch identity). It rides the URI params the
   relink request already sends.
2. **Sender pins before release.** While it still holds the `DataPartPtr` (so `in-degree ≥ 1` via the
   sender's own ref), the sender computes the part's manifest closure and PUTs a **build-owned pin**
   `gc/fetch-pins/<receiver_srid>/<build_id>` = `{blob-ref closure}`. Self-contained: it lists the blobs
   directly, so GC overlays them **without re-reading the manifest**.
3. **Reply** (sender → receiver): the manifest address and the pin id, in the **same response** that
   already carries the manifest. Then `Service::processQuery` returns and the part is released.
4. **Receiver commits, then releases.** The receiver reads the manifest, runs `adoptPartFromManifest`
   (its own ref now durable, `in-degree ≥ 1` via the receiver's ref), then **DELETEs the pin**.

**Zero-gap ordering.** Because the sender creates the pin **on its own thread, before releasing the
part**, protection is continuous at every instant:

```
[sender-ref] → [sender-ref + pin] → [pin] → [pin + receiver-ref] → [receiver-ref]
```

There is no window in which the blob's protector count drops to zero — unlike a receiver-published pin,
which cannot be established until after the manifest arrives (i.e. after the sender has already
released). This is the same commit-before-release discipline as zero-copy's `lockSharedDataTemporary`,
done by the party that holds the data.

**Cost.** One small control-plane PUT (sender) + one DELETE (receiver) per relink. This is **not** byte
cost — relink's zero-data-transfer advantage is intact; the receiver already does a durable ref commit,
so this is two additional small metadata ops on an already-metadata operation.

**Failure handling (fail-close / fail-loud):**

- Pin PUT **fails** at the sender → abort the relink for this part and fall back to byte-stream fetch
  (fail-loud, existing path). Never proceed relink without the pin.
- Sender **crashes** after the PUT → the pin is reaped later by the `min_active` heartbeat floor
  (§[Liveness](#liveness)); the fetch fails and the receiver retries. Extra retention until collection,
  never a loss.
- Receiver **crashes** after receiving the reply, before the DELETE → reaped by the `min_active` floor
  (its build falls below the floor, or its `srid` is fenced/expired).
- Pin PUT **stalls** → the sender is still holding the part during the stall, so blobs stay protected;
  the fetch is merely slow.

## Liveness / self-cleaning — the heartbeat build-watermark floor (`min_active`) {#liveness}

Cleanup rides an **existing published watermark**, so GC reaps stale pins with ~zero new machinery. Each
server already stamps a **build-watermark floor, `min_active`, onto its mount-lease heartbeat**
(`gc/server-roots/<srid>/mount` — merged into the same `SingleWriterSlot` renewal PUT that stamps the
lease clock; `CasServerRoot.h` `MountLeaseKeeper`, `min_active_fn_`; `min_active == UINT64_MAX` is the
graceful-farewell sentinel). GC's per-round heartbeat gate `computeHeartbeatFloor` **already** LISTs
`gc/server-roots/` and GETs every mount body for liveness classification, so it **already holds each
server's `min_active`** — no new published state, no extra read.

In that same pass, GC **reaps (deletes)** a fetch-pin `gc/fetch-pins/<srid>/<build_id>` when either:

- `build_id < srid.min_active` — the build that took the pin is no longer active on its server (committed
  or aborted); or
- the `srid` is terminated / fenced / lease-expired — the whole server is gone (the coarse backstop).

This is **observation-based** — the floor is a build sequence, not a clock (no wall-clock TTL, no skew
dependency; consistent with the rev.6 liveness model) — and **prompt even within a still-live mount**:
`min_active` advances as the server's builds complete, with **no** dependence on a remount / epoch bump.
There is therefore no "leaked pin lingers until remount" limitation. The receiver's happy-path DELETE
after its ref commit becomes a mere latency optimization; GC's heartbeat-floor pass is the authoritative
reaper.

**Integration point (verify in the plan).** The receiver's `min_active` must **cover its in-flight
fetch/relink build for the whole fetch**, so the floor keeps `build_id ≥ min_active` (pin honored) until
that build commits or aborts. If a fetch/relink build is not already reflected in the Store's
`min_active_fn_`, wire it in — this is the single integration point to confirm during implementation.

## Bulk write-replica warm-up (future extension) {#warmup}

Fast warm-up of a **new write replica** relinks the table's whole active part-set at once rather than
one part per request. It is the same build-owned pin, generalized:

- **Payload:** one pin covers **many** manifest closures (the union of the batch), or — cheaper when the
  batch is "the whole snapshot" — the source's **snapshot-window** directly. This is why the primitive
  admits a window payload for build-owned pins, not only single-closure. On the payload axis the warm-up
  pin converges toward the reader shape; it stays distinct on ownership (build, not mount) and liveness
  (the writer's `min_active` build-watermark floor, not a reader-lease TTL).
- **Lifetime:** created by the source while it can enumerate its current snapshot's parts, released once
  the receiver has relinked the batch. Still short-lived relative to a reader query.

Not built now. The single-part protocol (§[Protocol](#protocol)) is v1; this section records the intended
generalization so v1's pin object and GC integration are designed not to preclude it.

## Detached / relink cluster (housed here) {#detached}

Kept with the fetch-handoff work to keep the detached/relink cluster together:

- **[B66b] relink-into-detached** — today `to_detached` byte-streams even for same-pool parts
  (`DataPartsExchange.cpp:523` gates relink off for `to_detached`, staging at the ACTIVE path). Extending
  `Fetcher::relinkPartToDisk` to honor `to_detached` via the `detached/<part>` fold (B181) closes the
  RPL-4 perf cliff. It **inherits the same commit-before-release handoff**, so it takes the same
  fetch-handoff pin — do it together with the pin work.
- **[B66a] concurrent-fetch torn read of shared `detached` ref on local storage** — a shared-`detached`
  read-modify-write of a shared ref is non-atomic on `LocalObjectStorage` (safe on S3's atomic PUT).
  **Orthogonal to the pin** (a local-backend atomicity item), housed here only for cluster cohesion; the
  fix is per-ref / per-frozen-part writes (as freeze already chose, see `cas/CONSOLIDATION-COVERAGE.md`)
  or a local put-if-absent atomicity shim — not a pin.

## Interim status — accept + document (option C) {#interim}

Until the primitive + fetch-handoff pin land, the gap is **accepted and documented** (option C): the tail
is deep, `deleteExact` covers all token-change recoveries, and fsck detects the residual same-token
dangle. A comment at `adoptPartFromManifest` records it (already committed, `c254cf24729`). **Do NOT**
build a bespoke relink handshake in the meantime — the transport-level seals are all ruled out
(§[Motivation](#motivation)).

## Testing {#testing}

- **Seal regression:** stall the receiver's `precommitAdd` edge-PUT across ≥ 2 GC folds while the source
  drops and `grabOldParts`-collects the now-`Outdated` part; assert the blob **survives** with the pin,
  and (pin disabled, reproducing the pre-fix state) that fsck reports the dangle. This is the
  counterexample the pin closes.
- **Zero-gap ordering:** assert the pin PUT is durable before `Service::processQuery` releases the part
  (the ordering the seal depends on).
- **Epoch-floor cleanup:** crash the receiver mid-fetch (pin present, never DELETEd), remount with a
  bumped epoch, assert GC collects the stale pin; assert a live-owner pin is **not** collected.
- **Fallback:** force the pin PUT to fail, assert the relink falls back to byte-stream (fail-loud), not a
  pin-less relink.

## Non-goals {#non-goals}

- Reader-mount / snapshot-isolated SELECT serving — that is the read-replica spec.
- The bulk warm-up implementation — §[Warm-up](#warmup) is a future extension; v1 is single-part.
- Wall-clock TTL pins — rejected in favor of the `min_active` heartbeat build-watermark floor
  (observation-based; the floor is a build sequence, not a clock).
- Any change to byte-streaming fetch (cross-pool / non-relink) — it stays fire-and-forget; the receiver
  copies bytes and holds no dependency on the source's post-send part lifetime.

## TLA note {#tla}

The core retention invariant — *a blob in any live pin's edge-set is never reclaimed* — is covered by the
read-replica's `CaReadPinCore` gate over the shared primitive. The fetch-handoff adds two properties
worth a small model extension before implementation: (a) **commit-before-release ordering** — the sender
pins before releasing, so `in-degree ≥ 1` holds continuously across the handoff; (b) **`min_active`-floor
liveness** — a pin is reapable exactly when its owning build has dropped below its server's published
`min_active` (or the `srid` is fenced/expired), and never while the build is still active. Model the
sender-release / receiver-commit / GC-fold interleavings against a stalled receiver commit.
