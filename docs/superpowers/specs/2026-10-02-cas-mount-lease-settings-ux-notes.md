---
description: 'Problem statement and options: the CAS mount lease settings name mechanism internals, so the configured 30 s lease stops conditional writes after 14 s and the operator cannot tell why. Records the arithmetic, the timeouts behind it, five ways to make the settings say what happens, answers to three questions with a measured latency tail, and what an external study recommends. No decision yet.'
sidebar_label: 'Mount lease settings: UX notes'
sidebar_position: 104
slug: /superpowers/specs/cas-mount-lease-settings-ux-notes
title: 'CAS mount lease settings do not say what happens'
doc_type: 'guide'
---


# CAS mount lease settings do not say what happens {#cas-mount-lease-settings-ux}

An operator reads `cas_mount_lease_ttl_ms = 30000` and expects writes to survive 30 s without the
object store. Conditional writes stop after 14 s, and `system.cas_mounts` shows `live` for 16 more
seconds. The numbers follow from four settings through arithmetic that no setting names. This note
records the problem, why the numbers are what they are, the timeouts behind them, five options and an
external study of them. Nothing is decided.

Related: [mount renewal without a lease deadline](2026-10-01-cas-mount-renewal-without-lease-deadline-design.md),
[external study](../cas/2026-10-02-mount-lease-settings-external-study.md),
[issue #2431](https://github.com/Altinity/ClickHouse/issues/2431).

## What the operator sets and what happens {#set-vs-happens}

Operator-facing settings, with defaults (`docs/en/antalya/cas/configuration.md`,
`CA/ContentAddressedSettings.cpp`; `CA` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`):

| Setting | Default | What the text says | What it controls in fact | Same on all servers? |
|---|---|---|---|---|
| `cas_mount_lease_ttl_ms` | 30000 | how long a lease is valid | when another server may act; conditional writes stop 16 s earlier | yes, changed only with the pool stopped |
| `cas_mount_renew_period_ms` | 10000 | interval between renewals | renewal cadence, and part of the takeover wait | yes |
| `cas_attempt_timeout_ms` | 5000 | budget of one HTTP attempt | socket send and receive timeout of control-plane requests; twice over, how early conditional writes stop | no |
| `cas_lease_safety_margin_ms` | 2000 | "startup-only margin" | part of every write admission | no |

What follows from them, and where. Code references are to commit `e1c38ca7478`.

| Quantity | Formula | Default | A setting? |
|---|---|---|---|
| Attempt envelope | `attempt + 2 × connect cap`, cap = `min(connect_timeout_ms, attempt)`, or `attempt` when `connect_timeout_ms` is 0 (`CasRequestBudget::attemptEnvelopeMs`) | 7 s | no |
| Write reserve | `2 × envelope + margin` (`CasOperation::writeLoop`, `CasMountRuntime::admit`; `budgetAdmits` exists only on the one-lease-thread refactoring branch) | 16 s | no |
| Conditional writes and ref-log appends stop after the last successful renewal started | `TTL − reserve` | 14 s | no |
| Reads and operations through `CasOperation::readLoop` (`remove`, `removeManyWriteOnce`, blob `publish`) stop | `TTL − (envelope + margin)` | 21 s | no |
| Admission slack between a due renewal and the cutoff of conditional writes | `TTL − period − reserve` | 4 s | no |
| Lease expires | `TTL` | 30 s | yes |
| A restarted server may reclaim the slot once the token stayed unchanged on its own clock for | `TTL + TTL/20 + period/2` (`mountObservationThresholdMs`) | 36.5 s | no |
| GC on another server may fence the slot once the token stayed unchanged on its own clock for | `TTL + TTL/20 + period` | 41.5 s | no |

The last two rows are durations an observer measures from its own first sighting of an unchanged
token (`TokenWatch::stableFor`). They are not instants on the holder's timeline.

One outage on that scale. The last renewal started at 0 s; the store fails from 10 s on.

| Holder time | Event | Lease left | Conditional writes | Operations via `readLoop` | `system.cas_mounts` |
|---|---|---|---|---|---|
| 0 s | renewal succeeds | 30 s | admitted | admitted | `live` |
| 10 s | renewal due; requests fail, the renewal retries about once a second | 20 s | admitted | admitted | `live` |
| 14 s | lease left drops to the write reserve | 16 s | refused, "no lease budget" | admitted | `live` |
| 21 s | lease left drops to one envelope plus the margin | 9 s | refused | refused | `live` |
| 30 s | lease expires | 0 | refused | refused | `not_live`, `lease_expired` |

Observer thresholds have their own origin. If a restarted server first saw the token at holder time
0 s, it may reclaim the slot after 36.5 s on its own clock, and GC after 41.5 s. If its first
sighting was at 10 s, the thresholds fall at about 46.5 s and 51.5 s of holder time.

Writes resume under the same `writer_epoch` when a confirmed renewal leaves enough admission budget.
The renewed deadline is anchored at the start of that renewal, so a late answer can leave too little
or nothing: a renewal started at 10 s and answered at 45 s confirms a deadline of 40 s, which has
passed, and a fresh renewal is needed. The outage itself fences nothing and remounts nothing; an
observer that crosses its threshold first does.

The mismatches:

1. The lease setting says 30 s; conditional writes stop at 14 s and the other mutation paths at 21 s.
2. From 14 s to 30 s the mount reads `live` while conditional writes are refused.
3. The admission slack (4 s) is not a setting. One second more of `cas_attempt_timeout_ms` takes two
   seconds off it. It is slack between a due renewal and the cutoff, not an outage the mount survives:
   during a store outage admitted writes fail whatever lease budget is left.
4. `cas_lease_safety_margin_ms` is described as startup-only and takes part in every admission.
5. With `connect_timeout_ms = 0` on the disk the connect cap becomes the attempt timeout, the envelope
   becomes 15 s, and the reserve 32 s: more than the lease. The open is refused with an arithmetic
   message.
6. Two settings must match on every server of a pool. Nothing checks it; a server with a shorter
   threshold can fence a healthy peer.

## Why writes stop before the lease ends {#why-reserve}

A server must not have a write in flight when its authority ends. After the deadline, plus an allowance
for clock drift, an observer is entitled to act: GC drops the protection of the server's unfinished
work, or a successor takes the slot. A ref-log `PUT` that lands after that leaves a reference to blobs
GC may delete, or a second writer on one server root.

So a write is started only if it can finish before the deadline:

| Part of the reserve | Purpose | Needed for safety? |
|---|---|---|
| 7 s, first envelope | the client's wait for the `PUT` ends before the deadline; a request already sent may still be applied later | yes |
| 7 s, second envelope | the read that settles an unclear `PUT` also ends before the deadline, so the outcome is known | not on the audited paths ([answers](#answers)). It buys a settled outcome and, as slack, 7 s for a `PUT` that overruns its envelope |
| 2 s, margin | scheduling and clock granularity | belt |

Without any reserve, a `PUT` sent at 29.9 s can be in flight until about 37 s. Take an observer that
first saw the token at holder time 0 s, at the largest rate difference the `TTL/20` term allows:
a restarted server may act from `36.5 / 1.05 ≈ 34.8 s` and GC from `41.5 / 1.05 ≈ 39.5 s`. That `PUT`
can land after the first instant, not after the second. A request that runs longer can cross both.

Precedents name the state of uncertainty before known expiry. The ZooKeeper Java client sends an idle
ping after about a third of the session timeout, times out a read after about two thirds, and reports
`Disconnected` (a connection state; the session may still be alive on the ensemble) before the
ensemble expires the session (`Expired`). The fractions are idle intervals, not instants on one clock.
The CAS mount has the same shape (renewal due, writes refused, authority ends) and names only the last.

## Timeouts {#timeouts}

Only control-plane requests of CAS use the 5 s timeout. They run on a separate S3 client with one
attempt per call (`S3ObjectStorage::getSingleAttemptClient`); the CAS request engine retries.

| Requests | Connect | Send / receive | Retries |
|---|---|---|---|
| CAS control plane: lease, ref log, `gc/state`, checkpoints (conditional writes and the reads that settle them) | `min(disk connect timeout, attempt timeout)`; the disk connect timeout defaults to 1 s, and 0 means the attempt timeout | 5 s (`cas_attempt_timeout_ms`) | none in the SDK; the CAS engine retries |
| Data (blobs), ordinary ClickHouse S3 client | 1 s (`s3_connect_timeout_ms`) | 30 s (`s3_request_timeout_ms`) | up to 500 attempts |
| Azure in ClickHouse | 1 s (`azure_connect_timeout_ms`) | 30 s (`azure_request_timeout_ms`) | 10, backoff 10 ms to 1 s |

A writable CAS mount exists on S3 and GCS only (`checkConditionalWriteSingleAttemptSupport`).

SDK defaults, from `contrib`:

| SDK | Connect | Request timeout | Retries |
|---|---|---|---|
| AWS SDK for C++ | 1 s | `requestTimeoutMs` is 3 s in the bundled platform configuration; with curl it is a low-speed interval, on Windows a socket read timeout. `httpRequestTimeoutMs = 0` (curl total transfer time) is unlimited | 10 retries, exponential backoff from 25 ms; 3 attempts in `standard` mode |
| Azure SDK for C++ | 5 min (curl transport) | none found | 3 retries, 800 ms to 60 s |

ClickHouse replaces both with its own HTTP client and the values of the first table. The SDK
`requestTimeoutMs` is not equivalent to ClickHouse's per-socket-operation timeout.

The request timeout bounds one socket operation, not the request: see [answers](#answers), question 2.

## What the current work changes {#current-work}

| Before | After the mount-renewal change |
|---|---|
| a renewal gave up 4 s after it was due; the give-up fenced the mount and forced a remount (36.5 s or more) | the renewal retries until the store answers; nothing is fenced by an outage |
| an outage of 8 s that starts when a renewal is due cost a remount cycle | conditional writes are refused from 14 s until the store answers at 18 s: 4 s, plus the latency of the first successful renewal request |
| silent | `lifecycle_reason = 'lease_expired'`, the counter `CASMountLeaseExpired`, one `WARNING` |

The `WARNING` is logged by the first renewal-request notification after the expiry, so a blocked
request can delay it. `CASMountLeaseExpired` is incremented when a renewal restores an expired lease.
Neither is driven by a timer at 30 s.

Write admission, the reserve and the settings are unchanged. The table above is today's behaviour.

## Options {#options}

Common to every option, and worth doing whichever is chosen:

- One line at mount, in words: renewal every 10 s; conditional writes stop 14 s after the last
  renewal; the lease expires at 30 s; another server may take over once it has seen the token
  unchanged for 36.5 s (restarted server) or 41.5 s (GC).
- `system.cas_mounts` shows the instants: last renewed, writes stop at, lease expires at.
- The state changes when writes stop, not 16 s later: `lease_not_renewed` from the write stop,
  `lease_expired` from the expiry.
- The refusal names the cause and the setting, not "no lease budget".
- Config validation in words: what would happen, not which sum exceeds which.
- Fix the text of `cas_lease_safety_margin_ms`.

| Option | The operator sets | Conditional writes stop (defaults) | Takeover (observer's own clock) | Protocol change | Cost |
|---|---|---|---|---|---|
| A. Keep | four internals | 14 s | 36.5 s | no | the problem stays; only the common items help |
| B. One envelope | four internals | 21 s | 36.5 s | writer side only | proof that the settling read needs no reserve; CAS-343 first |
| C. Reserve on the observer | lease length = write stop | 30 s | about 43.5 s | writer and observer | new safety argument and models; which reserve moves and how drift applies to it are not defined; old observers must not meet new writers: two releases or a stopped pool |
| D. Name the outcome | `cas_mount_writes_stop_after_ms`; the lease length is derived (`+ reserve`) | whatever is set; 14 s keeps today's lease | unchanged at 14 s | no | one more name; a larger value lengthens the lease, which is a stopped-pool change |
| E. Tick | `cas_tick_ms`; everything is a whole number of ticks | 20 s with a 5 s tick, for every mutation | 36.5 s | no | the request timeout becomes part of the tick |

### E. One tick, everything in ticks {#option-tick}

ZooKeeper's `tickTime` model: one base unit, every other time a multiple of it.

`cas_tick_ms` is the longest one control-plane request may take, connect and its share of the margin
included. The other times are fixed multiples of it:

| Quantity | Ticks | With `cas_tick_ms = 5000` | Today |
|---|---|---|---|
| one request | 1 | 5 s: connect 2 × 0.5 s, send and receive 3 s, margin share 1 s | 7 s + margin |
| renew every | 2 | 10 s | 10 s |
| new mutations stop when not renewed for | 4 | 20 s | 14 s (conditional writes), 21 s (via `readLoop`) |
| local authority duration | 6 | 30 s | 30 s |
| admission slack between a due renewal and the cutoff | 2 | 10 s | 4 s |
| a restarted server may reclaim once the token stayed unchanged for | 6 × 1.05 + 1 | 36.5 s | 36.5 s |
| GC may fence once the token stayed unchanged for | 6 × 1.05 + 2 | 41.5 s | 41.5 s |

The authority duration is configuration on every server. The mount object carries only a diagnostic
`expires_at_ms`, which no server treats as authority.

Properties:

- The lease length and the renewal period keep today's values, so the two settings that must match
  across a pool do not change and no pool has to be stopped.
- One reserve policy: two ticks, the write and the read that settles it, with the cutoff at four ticks
  of six. The operator never sees the reserve as arithmetic; they see "mutations stop at 4 ticks".
  A one-tick reserve would put the cutoff at five ticks. It is a separate, later decision, and it
  cannot keep the four-tick explanation.
- One cutoff for every mutation. Operations that stop at 21 s today stop at 20 s; the classification of
  operations needs an audit.
- The cost is a tighter request: 3 s send and receive instead of 5 s and a 0.5 s connect instead of
  1 s. On the first attempt of a `PUT` the adaptive timeouts already apply 3 s and 0.5 s, and the
  [measured tail](#answer-latency) shows what they cut; reissues would be bound by them too.
  The evidence covers one stand.
- A slower store is handled by one number: a larger tick scales everything, and then the pool is
  stopped for the change because the lease length moves.
- The multipliers stay fixed (2, 4, 6) initially. Settings for them (`cas_lease_renew_ticks`,
  `cas_lease_ticks`) would bring back the interacting settings this option removes. ZooKeeper's
  `initLimit` and `syncLimit` are tick multiples for ensemble members, not for client renewal or
  write stop, and are no precedent for these multipliers.

D and E combine: the tick is what the operator sets, the outcome names are what the operator reads.

## Answers to the first three questions {#answers}

Three read-only studies, in full in
[the studies file](../cas/2026-10-02-mount-lease-settings-studies.md). Code was read at `e1c38ca7478`;
nothing was built or run.

### 1. The second envelope is not needed for ref-ledger safety {#answer-second-envelope}

The finding covers the audited paths: the engine, the ref ledger and the wedge. It does not cover
conditional writes outside the ref ledger (checkpoints, catalog, blob metadata), callers above the CAS
directory, or late application of a request the client has abandoned.

- With one envelope, an unclear `PUT` near the deadline ends as a give-up that says "sent, outcome not
  observed". The engine returns that same value today on three paths, one pinned by a test
  (`CASRequestsConnectHint.GatesRefuseTheReissue`).
- All ten consumers of a give-up were read: none acknowledges the write, none treats it as "did not
  land". A ref append becomes a wedged lane that keeps the exact key and bytes; it is settled later by a
  conditional create, never by a bare read.
- `remove` (through `readLoop`), `removeManyWriteOnce` and blob `publish` already reserve one envelope;
  `removeCurrent` reserves two. No observer uses an envelope.
- Cost of one envelope: a `PUT` that goes unclear 9 to 16 s before the deadline wedges its lane, where
  today it is refused cleanly.
- Not proven: late landing of conditional writes outside the ref ledger. The TLA+ models have no
  request duration.
- The second envelope has a side effect that matters: today every conditional `PUT` ends at least 9 s
  before the deadline; with one envelope, 2 s. Those 7 s are slack against an attempt that overruns its
  envelope (next answer). The margin and the one-envelope paths give less. None gives a finite bound.

### 2. One attempt is not bounded by a wall-clock deadline {#answer-request-deadline}

The connect, send and receive timeouts each bound one socket operation; every `recv` or `send` restarts
the timeout (`SocketImpl::receiveBytes`, `sendBytes`). No deadline for the whole attempt exists in
`PocoHTTPClient`, the AWS SDK path, `S3::Client`, the CAS backend or the CAS request engine;
`CasOperation::fits` checks the deadline only before an attempt is issued. GCS takes the same path.

| Case | Duration of one attempt |
|---|---|
| one phase stalls completely | up to 7 s: the envelope holds, assuming warm DNS, no client-side throttler, no credential fetch, and a TLS handshake bound that the code implies but the studies did not observe |
| several phases each near its timeout | 12 s for a `GET` with a stalled body; 17 s or more for a `PUT` with `Expect: 100-continue` |
| a response that trickles, one byte every 4 s | unbounded |
| `307` redirects or the `S3::Client::doRequest` loop | up to 11 requests in one attempt |
| client-side throttler (`s3_max_put_rps`, off by default) | unbounded |
| credentials fetched at signing (STS, GCP OAuth) | not bounded by the envelope |

The reserve rests on two properties: the client's wait for one attempt ends within one envelope, and a
request that was sent and abandoned does no harm if it lands later. The stall cases stay inside the
reserve plus the observer's margin by a small amount; the others do not. Tracked as CAS-343
(`docs/superpowers/cas/backlog`). The smallest fix found: one absolute deadline per attempt, set by the
CAS backend, to which the HTTP session clamps every connect, send and receive. It touches shared code
(`base/poco`, `src/IO/S3`).

A client-side deadline bounds the client's wait, not the time at which an accepted mutation is applied:
a conditional `PUT` already sent may land after the deadline. The ref ledger tolerates that (conditional
create, epoch seal). For keys outside it the argument is open, and it is needed before any reserve is
reduced.

### 3. Measured latency of control-plane requests {#answer-latency}

A few stands, each in one place with one kind of store: the figures are indicative only. They differ by
more than an order of magnitude between a server next to its store and one a continent away.

| Store | Typical | Bad moments |
|---|---|---|
| AWS S3, demo stand, 7 days to 2026-10-02, 5,674,215 control-plane `PUT`s (`system.blob_storage_log`, failed requests included) | p50 33.5 ms, p90 44.5 ms, p99 83.1 ms, p99.9 187.5 ms, p99.99 431 ms | max 3003 ms; above 0.5 s: 88 requests; above 1 s: 42; above 3 s: 10; above 5 s: none |
| MinIO in CI under ASan | mean 8 to 10 ms; p99.9 of the per-second mean 148 ms | one write took 10.7 s during a host stall |
| GCS over a WAN | `GET` 50 to 100 ms, write 150 to 400 ms, connect 40 to 85 ms | a ten-minute storm of connect timeouts; a two-minute DNS outage |

Reading the demo-stand tail:

- The ten requests at 3.00 s are client `Timeout` errors. Three connect timeouts at 0.5 s appear among
  all `Upload` rows of the week, control and data together. Both values are ClickHouse's adaptive
  first-attempt timeouts (3 s response wait of a `PUT`, 0.5 s connect), so the true duration of those
  requests is unknown: they are censored. No request ended between 3.0 and 5 s; whether the ten would
  have finished within 5 s cannot be told.
- The share of conditional writes that ended unresolved (0.032 % over the same week in `metric_log`:
  3,861 of 12,135,956) mixes conflicts, timeouts, connection loss and 5xx. It is not a timeout rate.
- Percentiles of per-second means, as in the MinIO row, are a different statistic from request
  percentiles.

Two short soak runs from one machine, 2026-10-02, 15 minutes of load each, control-plane `PUT`s from
`system.blob_storage_log` (every key outside `blobs/`, up to 64 KiB). The duration covers the whole call,
retries inside the client included, so it is not the latency of one attempt:

| Store | Network round trip | `PUT`s | p50 | p99 | p99.9 | max | above 0.5 s | above 1 s | above 3 s | above 5 s |
|---|---|---|---|---|---|---|---|---|---|---|
| AWS S3, `us-east-1` | 113 ms | 8,962 | 152 ms | 3.9 s | 8.5 s | 8.6 s | 7.0 % | 2.5 % | 1.3 % | 0.31 % |
| the same without one burst | | 8,509 | | 1.7 s | 3.9 s | 4.1 s | 5.7 % | 1.7 % | 0.60 % | 0 |
| GCS | 10 ms | 8,729 | 163 ms | 0.76 s | 1.3 s | 2.9 s | 4.3 % | 0.30 % | 0 | 0 |

- AWS: every request above 5 s fell in one burst of about 70 `PUT`s of 2.8 to 8.6 s on one server, with
  connect timeouts in the log. Shorter bursts of 2 to 4 s also occurred.
- New connections slower than 0.5 s: 25 to 37 % on AWS and 4 to 17 % on GCS by the connect histogram
  (it appears to include the TLS handshake; not verified in the source).
- GCS: the soak driver stopped at its first GC checkpoint on a harness bound; the load before it is what
  the row measures. About 485 checkpoint writes met the store's per-object mutation rate limit.

What the three measurements say together:

- Next to its store (the demo stand) a control-plane `PUT` almost never takes 0.5 s. From another
  continent 1.3 % of whole calls take more than 3 s, and a quarter or more of new connections take more
  than 0.5 s, which is the connect limit of a first attempt: part of the long calls are retries after
  that limit fired.
- A 0.5 s connect timeout cannot be a universal default. For the request timeout the data is not
  enough: it needs latency per attempt, which no source records.
- So either the tick is chosen per deployment, or the fractions of a tick given to a connect and to a
  request are larger than in the table of option E.
- Not measured anywhere: `GET` and `HEAD` latency per request (the reads that settle a write), and
  latency per attempt (the first-byte histograms stamp before the response).

## External study {#external-study}

An external reviewer studied this note and the studies file, and compared them with other systems.
Full text: [external study](../cas/2026-10-02-mount-lease-settings-external-study.md). It recommends E
with fixed multipliers:

- `cas_tick_ms = 5000` with fixed multipliers: renew at 2 ticks, new mutations stop at 4, authority ends at 6.
- Both request reservations stay; one cutoff for every mutation; an explicit `lease_not_renewed` state.
- Legacy settings select the legacy profile; setting both families is refused.
- Order of work: visibility first, then CAS-343, then the late-application argument for keys outside
  the ref ledger, then tests, then measurements, then the tick.
- Fallback if one tick proves too rigid: a named trio of renewal cadence, mutation-stop age and
  authority duration.
- Closest precedents: Kubernetes leader election (`leaseDuration`, `renewDeadline`, `retryPeriod`);
  Chubby (jeopardy, grace period); the DynamoDB lock client (danger zone); Redlock (validity time);
  AWS Java SDK v2 (`apiCallAttemptTimeout`, a per-attempt deadline).

## Open questions {#open-questions}

1. Late landing of conditional writes outside the ref ledger, before any reserve is reduced or one
   tick cutoff replaces today's 14 s and 21 s cutoffs (answers 1 and 2).
2. Where the per-attempt deadline lives: in the HTTP session, in the S3 client, or in both (CAS-343).
3. Per-request `GET` and `HEAD` latency, other stores, the connect-time tail, and the soak-stand
   measurement (answer 3).
4. Nothing verifies that the servers of a pool agree on the lease length and the renewal period.
   Should the mount body carry them, so that a mismatch is refused at mount?
5. Defaults: keep the 30 s lease (no stopped-pool change) or lengthen it.
6. Which operations a single cutoff for every mutation changes (E).

## Recommendation {#recommendation}

In this order:

1. The common items for what the operator reads: they need no protocol change and remove the worst
   mismatch (`live` while conditional writes are refused).
2. CAS-343. It bounds the client's wait for one attempt. Until then "a request ends within one
   envelope" is a hope, and neither the reserve nor a tick may shrink.
3. The late-application argument for conditional writes outside the ref ledger. A client deadline does
   not say when an accepted mutation is applied.
4. E for what the operator sets, with two reservations, one cutoff at four ticks and the reserve kept
   on the writer's side.
5. The one-envelope reserve stays deferred. It is a separate decision after steps 2 and 3 and after
   the measurements of answer 3.

It is a separate change after the renewal work, because it touches write admission and reporting that
the renewal work restructures.
