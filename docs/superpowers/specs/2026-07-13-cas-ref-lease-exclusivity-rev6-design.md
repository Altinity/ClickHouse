---
description: 'Approved design: lease-boundary exclusivity for the CAS ref protocol — observation-based lease liveness, conditional T_mat, epoch-closing recovery seal, grace-machinery removal'
sidebar_label: 'CAS Ref Lease Exclusivity (rev.6 design)'
sidebar_position: 20260713
slug: /superpowers/specs/cas-ref-lease-exclusivity-rev6-design
title: 'CAS Ref Protocol rev.6 Design: Lease-Boundary Exclusivity'
doc_type: 'reference'
---

# CAS Ref Protocol rev.6 Design: Lease-Boundary Exclusivity {#cas-ref-lease-exclusivity-rev6-design}

**Date:** 2026-07-13
**Status:** APPROVED design (user-reviewed brainstorm, 2026-07-13). Supersedes the
[rev.6 proposal](2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md); amends
[CAS Ref Table Snapshot and Log Design](2026-07-11-cas-ref-table-snapshot-log-design.md) (rev.5).

## Principle {#principle}

Exclusivity is solved **once, at the mount-lease boundary**. Past that boundary the ref protocol is
single-writer by construction and sheds every "someone else may still be writing" complication:

- The writer regularly asks "may I still write" via **local, zero-request checks** (the existing
  `CLOCK_BOOTTIME` fence: `mayMutate`, `refAppendFenceOk` — `CasStore.cpp:181-206`). These stay.
- No hot path spends a single S3 request detecting or fighting a foreign write. Only **incidental
  checks** — signals that arrive for free on operations we perform anyway (a conditional-PUT token
  mismatch, unexpected bytes at our own key).
- When an incidental check detects an "impossible" interference, the reaction is `LOGICAL_ERROR` +
  fail-closed remount (see [Anomaly policy](#anomaly-policy)). Diagnosis may spend a few requests,
  but only in background, outside all critical paths.
- The lease *acquisition* side is maximally hardened instead: a writable mount begins only after the
  predecessor's writes provably cannot appear, and safety decisions never compare wall clocks across
  nodes.

One sentence: **a writable mount begins only after the predecessor's writes provably cannot appear,
and the first snapshot published at an unclean mount seals every dead epoch; from that moment the
protocol is single-writer by construction.**

## Decision log {#decision-log}

Decisions taken during the 2026-07-13 brainstorm, with rationale:

1. **Clock trust is asymmetric.** For *declining* to take a lease (back-off, `LiveDoubleStart`,
   `ForeignOwner`) we may trust the other node's wall-clock `expires_at_ms` — a clock error only
   lengthens a wait. For *taking* a lease and for *fencing* one (both aggressive), wall clocks are
   never compared: liveness is established by **observing token stability** on the observer's own
   monotonic clock. This replaces a clock-*synchronization* assumption (NTP) with a clock-*rate*
   assumption (bounded quartz drift, ~10⁻⁴), which hardware actually guarantees.
2. **GC fence-out becomes observation-based**, and a `gc_fenced` lease is then a transferable
   **certificate of observed death**: the successor may reclaim instantly (the observation was
   already served by the GC leader) and pays only T_mat.
3. **T_mat (`materialization_grace_ms`) = 30 000 ms default, configurable**, with a documented
   warning that lowering it increases the risk of a late-materializing predecessor write being
   dropped (contract-clean, but debris). Store research below.
4. **Self-remount pays a conditional T_mat**: the live process drains its own in-flight requests
   (waits for responses, bounded by `attempt_timeout_ms`); if every request resolved, no T_mat is
   owed — recovery stays fast for the P3.1/S13 class (externally fenced live mount). Only an
   unresolved (timed-out) ref-log PUT forces the T_mat wait.
5. **The recovery seal stays** (it survived an explicit "do we even need it if we trust the lease"
   challenge): T_mat is the one promise held by the *store*, not by us, and no store documents an
   upper bound. Without the seal a T_mat violation is **undetectable silent divergence** — no
   incidental check can fire, because a late log is indistinguishable from a legitimate tail to
   every reader. With the seal a violation is deterministic-invisible *and* detectable as debris.
   Cost: one snapshot PUT per unclean mount, zero hot-path cost, no new machinery.
6. **Seal id closes all dead epochs** — a correction to the rev.6 proposal, see
   [Seal](#recovery-seal).
7. **Seal only on unclean handover.** A clean release drains all lanes first, so no in-flight PUT
   exists physically; the inherited tail is bounded by the ordinary publish threshold; clean restart
   stays completely free (the happy path).
8. **Wedge invariant is a hard contract**: `chassert` in debug; in release the lane fail-closes
   (refuses to allocate new ids) — routed into the standard anomaly policy. Operators do nothing;
   the bounded self-remount is the built-in "retry up to 3 times".
9. **Interim mechanical patch already landed** (`3c7003ce190`: aged+uncovered trigger, copy-once
   replay, threshold 256) — rev.6 deletes the grace machinery on top of it.
10. **Scope**: mount-lease hardening + ref-protocol simplification + an audit of GC-side defenses
    against the "incidental checks only" principle. GC leadership itself is *not* redesigned — it is
    linearized by a single CAS on `gc/state` per round, involves no clocks, and a deposed leader
    simply fails its commit. The orphan-sweep epoch gate is likewise kept: it is the cleanup
    mechanism *for* legitimately dead epochs, not a race defense.

## Lease acquisition {#lease-acquisition}

The lease object, its token-conditional writes, the epoch allocation, and the claim decision table
(`CasServerRoot.h:143-159`, `CasServerRoot.cpp:337-415`) are unchanged except for the branches
below.

### Taking over an unclean predecessor (same `server_uuid`) {#unclean-takeover}

Today the reclaim branch takes immediately when `expires_at_ms <= now_ms` — a cross-node wall-clock
comparison. Replaced by **token-stability observation**:

```text
first GET of the lease:  remember (token T0, local monotonic time t0)
poll every poll_interval_ms:
  token changed        -> the holder is alive; restart observation
                          (bounded restarts, then LiveDoubleStart abort as today)
  token stable through  t0 + mount_lease_ttl_ms * 1.05 + poll_interval_ms
                       -> the renewal that produced T0 happened at or before t0;
                          the holder's local BOOTTIME fence has provably expired;
                          reclaim via token-guarded putOverwrite (unchanged mechanics)
```

The 5 % factor covers quartz drift of both nodes (~10⁻⁴ each) with two orders of magnitude to
spare; the added `poll_interval_ms` covers observation discreteness. No wall-clock value is read
for the decision.

**Fast paths, in priority order:**

1. `released_clean` marker present → predecessor drained and said farewell: **no observation, no
   T_mat, no seal.** Ordinary restarts pay nothing.
2. `gc_fenced` → certificate of observed death (see [GC fence-out](#gc-fence-out)): **no
   observation**, T_mat only. This also covers the long-downtime case — a node dead for hours has
   long since been fenced by GC, so the fresh-crash observation wait mostly hits only quick
   restarts after a hard kill.
3. Otherwise: full observation + T_mat.

Every wait logs explicitly, e.g.:

```text
Attempting to mount content-addressed disk after node change or hard restart;
waiting ~31s (token-stability observation) + 30s (materialization grace) to confirm
the previous incarnation's operations are all finalized.
```

Honest cost accounting (accepted): an unclean restart whose lease was not yet GC-fenced pays
~TTL + T_mat ≈ 60 s before the mount opens writable, where today it can pay 0–30 s. Clean restarts
and certificated takeovers are faster than today or equal.

### Clean release {#clean-release}

`MountLeaseKeeper` termination (`CasServerRoot.cpp:807-848`) is extended: before stamping the
farewell marker (`min_active = UINT64_MAX`, already the terminated signal — `CasServerRoot.h:222-228`),
the store **drains all ref-append lanes**: stop admitting new appends, wait for every in-flight
conditional PUT to resolve within the ordinary request budget. Only a successful drain writes the
marker. If an unresolved PUT remains at the shutdown deadline, the marker is **skipped** — the
successor then treats the end as unclean (observation + T_mat), which is the safe direction.

### Self-remount {#self-remount}

`tryRemountOnce` (`CasStore.cpp:597-694`) gains the conditional-T_mat step: quiesce waits for lane
leaders to conclude their current attempt (≤ `attempt_timeout_ms`) and collects the unresolved set
of **ref-log** conditional PUTs (blob-side and manifest-body uploads do not gate the remount: their
late materialization is already handled by the in-degree/orphan machinery and never affects the
ref-log seal).

- Unresolved set empty (the common case): proceed immediately — recovery in seconds.
- Non-empty: wait T_mat before the recovery LIST, with the explicit log line.

The `quiesceRefTablesForRemount` conversion of a wedged PUT into the "accepted Late Predecessor"
case (`CasStore.h:746-755`) is deleted: the seal makes the conversion unnecessary.

## GC fence-out {#gc-fence-out}

`computeHeartbeatFloor` (`CasServerRoot.cpp:477-556`) stops comparing the lease's wall-clock
`expires_at_ms` against the GC leader's clock (`now_ms > expires_at_ms + ttl/2`). Instead the GC
leader keeps an in-memory observation map `(srid -> token, first-seen local monotonic time)` across
rounds and fences only a lease whose token stayed stable for the same
`mount_lease_ttl_ms * 1.05 + poll_interval_ms` threshold on the **leader's own clock** (for GC the
"poll interval" is the round cadence, so in practice one full round of stability past the TTL). Fence mechanics
(token-guarded `putOverwrite` with `gc_fenced = true`, bounded reclassify on `PreconditionFailed`)
are unchanged.

- GC leadership change: the new leader starts observation afresh; fencing is delayed by one round.
  Harmless — fencing is liveness/cleanup, not safety; safety is carried by token-conditional writes.
- This removes the clock-skew-triggered premature fence of a *live* mount (the P3.1/S13 incident
  class) as a failure mode of clocks; only a genuinely non-renewing mount can be fenced.
- `skew_margin_ms` disappears as a safety concept. Wall-clock fields in `MountLease` remain for
  diagnostics and `system.content_addressed_mounts` display only.

## T_mat: materialization grace {#t-mat}

The one hazard no client-side timeout bounds is **server-side late materialization**: a store may
commit an accepted PUT after the client died. `materialization_grace_ms` (default **30 000**,
configurable) is the allowance we give the store to finish or drop accepted requests after the
predecessor provably stopped issuing them.

What the stores document (researched 2026-07-13 — the key finding is that **no store documents an
upper bound**, which is why the default is generous and the seal exists as the correctness
backstop):

| Store | Documented bound | Note |
|---|---|---|
| AWS S3 | None numeric. `RequestTimeout` is text-only ("socket connection … not read from or written to within the timeout period"); ~20 s observed by community. `CompleteMultipartUpload` is documented to take "several minutes" to finalize after the last client byte. | [API_CompleteMultipartUpload](https://docs.aws.amazon.com/AmazonS3/latest/API/API_CompleteMultipartUpload.html), [ErrorResponses](https://docs.aws.amazon.com/AmazonS3/latest/API/ErrorResponses.html), [conditional writes](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html) |
| Azure Blob | The only hard cap: 30 s max server timeout; `Put Block List` ≤ 60 s; writes allowed 10 min/MB. | [Setting timeouts for Blob service operations](https://learn.microsoft.com/en-us/rest/api/storageservices/setting-timeouts-for-blob-service-operations) |
| GCS | Resumable-upload session lives **7 days**; only a completed upload appears in the bucket. | [Resumable uploads](https://docs.cloud.google.com/storage/docs/resumable-uploads) |
| MinIO | Source: rolling 30 s idle kill (`--idle-timeout`), no total-request cap. | `internal/http/server.go`, `cmd/server-main.go` |
| rustfs | Source: 75 s header timeout, **300 s body-stall watchdog**, no total cap. | `crates/config/src/constants/tls.rs` |
| Ceph RGW | `request_timeout_ms` default 65 s, **0 = disabled** (unbounded). | [Beast frontend](https://docs.ceph.com/en/latest/radosgw/frontends/) |

Mitigating context: the seal protects only the ref-log key space, which uses small **single-shot**
conditional PUTs — the minutes/days-scale documented tails belong to multipart/resumable paths the
ref log never uses. The long stall windows (e.g. rustfs 300 s) are "server waits for more body,
then **drops**" cases: process death closes the socket and an incompletely received body never
materializes. Only a fully received PUT can commit late, and that is seconds of processing —
30 s covers it generously. Still: not documented, hence configurable + seal.

Documentation for the setting must state: lowering `materialization_grace_ms` increases the risk
that a late-materializing predecessor write is dropped by the seal (contract-clean — its author
never received an ACK — but it becomes debris for the orphan sweep and is reported as an anomaly).

The request-budget tuning invariant is extended: `unclean handover wait >= all client timeouts +
materialization_grace_ms` (`validateCasRequestBudget`, `CasRequestControl.h:109-118`).

## The recovery seal {#recovery-seal}

At an **unclean** mount, after the waits, the writer performs the recovery LIST, replays what it
found, and publishes the resulting state as an immediate snapshot — the seal — **before the mount
opens writable**:

```text
1. observation (unless certificated/clean) + T_mat (unless drained-clean)
2. recovery LIST of the _log region            <- LIST first
3. replay of everything listed -> state S
4. conditional PUT of _snap at seal_id with S  <- the seal
5. mount opens writable
```

### Seal id closes all dead epochs (correction to the proposal) {#seal-id}

`RefTxnId = (writer_epoch, ref_sequence)` orders epoch-major (`CasRefIds.h:27-33`) and the rendered
key preserves that order lexically (`CasRefIds.h:35-47`). The proposal sealed at the greatest
*listed* id `V`. That is insufficient: the wedge discipline places the predecessor's one possible
in-flight PUT at an id `W` **strictly above** everything it resolved, so if `W` had not
materialized by LIST time, `W > V` — it would land *above* a seal at `V` and remain visible to cold
folds. The seal must therefore be published at the **upper bound of the dead-epoch region**:

```text
seal_id = (my_epoch - 1, 0xFFFFFFFFFFFFFFFF)
```

`my_epoch` is freshly CAS-allocated and strictly greater than every dead epoch, so *any* late PUT
from *any* dead epoch `(e, seq), e < my_epoch` is born covered — `<= seal_id` — for every observer
uniformly, forever. This also closes a chain of several crashed epochs with no intervening
snapshot, which a seal at `V` would not. The writer's own transactions `(my_epoch, 1…)` sit above
the seal and live normally.

Snapshot-id semantics are accordingly restated from "the id of the last log the snapshot covers"
(`CasLayout.h:125-130`) to "the upper bound of the covered region"; every reader's coverage test
remains the same `<=` comparison. The seal is the only snapshot whose id is not a real transaction
id.

The seal body additionally records the **greatest listed txn id** (`sealed_from`, diagnostic
metadata): a log whose id lies in `(sealed_from, seal_id]` provably materialized after the recovery
LIST — this is what lets the sweep report a T_mat violation (see
[Anomaly policy](#anomaly-policy)) instead of mistaking the late log for an ordinary covered one.

### Soundness {#seal-soundness}

- **No fillable holes below the seal.** The lane admits one leader per namespace
  (`CasStore.cpp:1373-1406`) and forbids allocating ids past an unresolved PUT
  (`CasStore.h:578-587`, `CasStore.cpp:1503-1554`), so at most one id per namespace is uncertain at
  predecessor death and it is the greatest allocated id of its epoch. Everything below it is
  resolved and, if durable, LISTed. History under the seal is complete. This invariant was
  re-verified against the lane code (single `leader_active` authority, controller-held leadership
  across retries, resolve-before-reissue of the exact `(key, bytes)`).
- **Layering.** The local fence bounds when the predecessor can *issue* requests; observation
  proves the fence expired; T_mat bounds when issued requests can *materialize*; the seal makes any
  violation of T_mat deterministic-invisible instead of silently divergent. Each layer is
  independent.
- **Divergence window closed.** Without the seal, a log materializing after the recovery LIST is
  visible to cold folds (they read "newest snapshot + logs above") but permanently absent from the
  writer's memory — two truths until the writer's first lazy snapshot lands (observed in the wild
  as the `delete_pending retired entry recovered in-degree N — sparing` warnings). With the seal
  the window is zero: from the first writable instant every observer reads "seal + logs above it".

### Failure and retry {#seal-failure}

The seal PUT rides the standard request controller: retries with backoff inside the 90 s operation
envelope, resolve-before-reissue of the **same** `(key, bytes)` on ambiguity. Rebuilding different
bytes for the same `seal_id` within one mount attempt is forbidden (same-key-different-bytes breaks
exact-resolution). If the envelope fails, the mount does not open writable; the next mount attempt
allocates a **new epoch** and therefore a new, strictly larger `seal_id` — moving the seal across
attempts is safe. An abandoned earlier seal that materializes late is inert debris: a lower
snapshot id is never read once a newer snapshot exists (T11 monotonic adoption), and the sweep
removes it.

A namespace born in the current epoch has no predecessor region: no seal, first snapshot stays
lazy. A clean mount never seals (decision 7).

## Ref-protocol simplification {#ref-simplification}

Deleted (all of it existed solely as Late-Predecessor insurance):

- `snapshot_min_log_age_ms` and both age gates — dispatch (`CasStore.cpp:1840-1881`) and publish
  replay (`CasStore.cpp:1990-2019`).
- The base+tail replay under `state_mutex` and the resident `snapshot_base_state` /
  `tail_since_snapshot` bookkeeping (`CasStore.h:630-647`, `CasStore.cpp:1131-1140`) to the extent
  it exists only to hold young entries back.
- `CasRefLatePredecessorObserved` (`CasStore.cpp:52,2009`, `ProfileEvents.cpp:770`).
- The wedge→LatePredecessor conversion in `quiesceRefTablesForRemount` (`CasStore.h:746-755`).

Snapshot publication becomes copy-once-from-live:

```text
after applying txn N under the flush leader (single-writer by construction):
  if trigger fires (count > snapshot_log_count_threshold (256) or bytes threshold):
    copy the live state once, at the txn boundary; hand to the async publisher
  continue with N+1 immediately
publisher: encode + conditional PUT of _snap off the hot path
```

No replay, no per-entry copies, no `state_mutex` held across encode.

Explicitly staying (defenses against the writer's own past self, not against "someone else"):

- `fence_ok` pre-attempt + post-write verify on every conditional log PUT (own stalled thread /
  frozen process / externally fenced live mount). `CasConditionalWriteFenceLostPostWrite` stays.
- Resolve-before-reissue + wedge discipline (also load-bearing for seal soundness).
- T11 monotonic snapshot adoption (own async publishes may finish out of order).
- Writer-side linearization, batching lanes, request-budget validation.
- All intra-node GC-vs-writer machinery (in-degree recompute, sparing, clamp barriers) and
  blob-side dedup-race tolerance (`putBlob` ABORTED retry, adopt-if-byte-equal, promote/resurrect
  paths): the mount lease is a per-node exclusivity boundary, not an intra-node serialization.

## Anomaly policy {#anomaly-policy}

Incidental-only detection, fail-closed reaction, background diagnosis:

- **Detection is free.** Signals that arrive on operations we already perform: a token mismatch on
  a key the lease makes exclusively ours, foreign bytes found by `resolveByExactGet` at our wedge
  key, a `foreign_writer` renewal mismatch classification, a divergent-bytes deterministic
  artifact, a wedge-contract violation. No path adds requests to look for interference.
- **Reaction.** The operation fails with `LOGICAL_ERROR`; the mount trips its fence (all writes
  fail closed) and enters the existing bounded remount (`max_fence_recoveries = 3`). The full claim
  protocol on remount is itself the second-writer detector: a real concurrent writer surfaces as
  `ForeignOwner`/`LiveDoubleStart` and aborts loudly. Operators act only on recurrence (bug
  report).
- **Diagnosis off the critical path.** A background task may spend a few requests: GET the
  offending object, extract the writer identity carried in its body, emit a rich log line and a
  `content_addressed_log` event.
- **T_mat-violation detector for free**: the orphan sweep already LISTs the `_log` region; a log
  below the seal that is not covered by seal content is structurally recognizable debris — report
  it ("store materialized a write after the grace window"), then remove it as ordinary debris.
  Never GET/read it to "revive" it (the resurrect invariant).
- **Wedge contract**: `chassert` in debug; in release the lane refuses to allocate new ids past an
  unresolved PUT (true fail-closed, not an assert) and the violation routes into the policy above.

## GC-defense audit {#gc-defense-audit}

In scope as an audit, not a redesign. Each site is checked against the principle "incidental
checks only; zero S3 budget spent fighting a foreign writer on hot paths"; only violations found
are cleaned up:

- Re-read of `gc/state` confirming round ownership; zombie-steal protection (committed pair
  threaded into retire, never re-read — `CasGc.h:243`); deposed-concurrent-leader debris handling
  (`CasGc.cpp:1682-1695`). Expected verdict: compliant — linearized by the single round CAS, no
  clocks, deposed leader fails its commit; document, keep.
- Orphan-sweep prior-epoch eligibility gate (`CasOrphanManifestSweep.cpp:146-163`) and
  TokenMismatch/404 tolerance on delete (`:200-206`, `:283-302`): the former is the cleanup
  mechanism for legitimately dead epochs (keep); the latter tolerates intra-node races (keep).
- Fold-lag machinery (restart-on-vanish, clamp barriers, clamp suppression): intra-node, keep.

The audit's deliverable is a short compliance note per site in the implementation plan, plus fixes
for any site found spending non-incidental requests on foreign-writer defense.

## Configuration {#configuration}

- Add `materialization_grace_ms` (default 30 000) with the documented lowering risk.
- Delete `snapshot_min_log_age_ms`.
- `snapshot_log_count_threshold` already 256 (`3c7003ce190`).
- Observation parameters derived, not new knobs: threshold `mount_lease_ttl_ms * 1.05 +
  poll_interval_ms`; `poll_interval_ms` stays derived from `mount_renew_period`.
- Extend `validateCasRequestBudget` with the handover-wait invariant.

## TLA+ {#tla}

Extend the ref model:

- Invariant `NoDivergentFold`: all observers agree on the folded set at every step under the new
  mount rule.
- The predecessor's in-flight PUT is a message deliverable at any time **including after T_mat**
  (the violation case) — verify the seal makes it inert for every observer.
- Model observation causally (renewal happened-before first observation), with no global clock —
  the safety argument must not mention wall time.
- Seal id as the dead-epoch upper bound; several dead epochs in a row as a test scenario.
- Keep the stalled-own-thread action to re-verify `fence_ok`.

## Testing {#testing}

- gtest, claim protocol: observation wait and restart-on-token-change; `released_clean` fast path
  (drained vs undrained shutdown); `gc_fenced` certificate path; conditional T_mat on self-remount
  (resolved vs unresolved in-flight set); seal published before writable; seal-envelope failure →
  mount retry with a fresh epoch.
- gtest, seal semantics: late log injected below `seal_id` after LIST → invisible to fold, sweep
  reports+removes it; multiple dead epochs sealed at once.
- gtest, wedge contract: violation → release fail-closed + anomaly policy.
- e2e: self-remount latency for the drained case (seconds, not T_mat); clean-restart zero-wait.
- Soak: repeat the S13/S15/S18 scenario cards; add a late-PUT injection scenario asserting
  inertness + the sweep report.

## Amendments to rev.5 {#amendments}

1. §Late Predecessor PUT: rewritten — closed by construction (observation + T_mat + seal); the
   honest caveat stays that T_mat is an operational bound on store behavior, with the seal turning
   any violation into a deterministic, reportable drop.
2. §Startup And Recovery: unclean-handover observation wait, `released_clean` drain semantics,
   `gc_fenced` certificate, conditional self-remount T_mat, eager seal at `(my_epoch − 1, MAX)`
   before writable.
3. §Snapshot Publication: copy-once-from-live; snapshot id semantics restated as covered-region
   upper bound.
4. §S3 Request Budget: `unclean handover wait >= all client timeouts + materialization_grace_ms`.
5. §Failure Handling: seal PUT failure ⇒ mount fails to open writable (fail-closed; next attempt =
   fresh epoch, fresh seal).
6. GC: fence-out decision moves to token-stability observation on the leader's clock;
   `skew_margin_ms` removed from safety logic.
