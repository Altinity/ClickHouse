---
description: 'Live backlog — CAS on Google Cloud Storage: what the 2026-09-02 live campaign proved, what it broke, the fix options with their trade-offs, and the order we intend to take them in.'
sidebar_label: 'GCS'
sidebar_position: 12
slug: /superpowers/cas/backlog/gcs
title: 'CAS Backlog — Google Cloud Storage'
doc_type: 'guide'
---

# CAS Backlog — Google Cloud Storage {#gcs}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for everything that is specific to
running CAS on GCS: the live release gate, the two GCS-only failure classes found on 2026-09-02, the
environment findings of the same day, and the fix options with their trade-offs. The run record with
timestamps and evidence paths is the
[2026-09-02 live validation ledger](/superpowers/cas/gcs-live-validation-ledger-2026-09-02); this file
carries the decisions, so nothing in the ledger has to be re-derived.

## What holds on GCS today {#what-holds}

Proved on the real bucket with the `gcs_hmac` client, binary of 2026-09-02:

- The live gate `tests/integration/test_gcs_live`: 13 passed, 0 failed in four consecutive runs.
  Google accepts every ordinary and CAS request shape, including batch `DeleteObjects` (no singular
  fallback), multipart, native staged copy, the condemned-staged retag; every incarnation token is a
  numeric generation.
- Two replicas over one pool survive a ten-minute storm of connect timeouts to Google with zero
  failed `INSERT`/`SELECT` and both mount leases held (renewal retried and recovered).
- The mount refuses a bucket verified as versioned and proceeds with a warning when it cannot read
  the versioning configuration (`bd4cfc8d76f`); soft delete remains an operator precondition.

## Release gate: what is still unrun {#gate}

- **[gcs-live-gate-oauth-and-ambiguity] the `gcp_oauth` groups and the TLS ambiguity arms have never
  executed against Google** — GATE — 10 `gcp_oauth` cases need an ADC triple
  (`google_adc_client_id/secret/refresh_token` from `gcloud auth application-default login`) or a GCE
  host; 4 ambiguity cases need the TLS-terminating fault driver with the control contract documented
  in the [live-results gate](/superpowers/cas/unconditional-blob-publication-live-results). The driver
  does not exist; `utils/ca-soak/proxy/s3_fault_proxy.py` is plain HTTP and cannot sit in front of
  `storage.googleapis.com` without switching `deduceProviderType` off. Also still open from the older
  follow-ups: the ordinary `test_storage_s3` lane once its historical image resolves,
  generation-aware LIST discovery (re-listing is cost-only today), signed `x-goog-*` `extra_headers`
  for `gcs_hmac`. Decision needed: build the driver (days) or descope the four arms in the gate
  document with a stated reason.

## Failure class 1: relink-confirm liveness (F11) {#relink-confirm-lane-livelock}

- **[relink-confirm-lane-livelock] two replicas starve each other's fetch-by-relink confirms** — HARD /
  RELEASE GATE (data divergence, not data loss).
  Design approved 2026-09-02: [relink confirm liveness design](/superpowers/specs/cas-relink-confirm-liveness-design)
  (rule 3 refuses only a broken lane; gate 0 plus the part state machine carry the removal argument).

**What happened.** After the connect storm both replication queues wedged with
`NO_REPLICA_HAS_PART: Source ... did not prove it still holds the manifest it offered ... by relink`;
the source answered `Relink confirm is unproven (unknown)` about 4.5k times per ten minutes per node
for forty minutes after the network recovered; replicas diverged to 123k vs 166k rows; the soak died on
`SYSTEM SYNC REPLICA`. 27,647 `build_abort abandoned` events in that window, each a precommit-removal
appended to the receiver's own lane and debris left for GC.

**Mechanism (confirmed by experiment).** `CasRefLedger::confirmExactRef` rule 3 answers `Unknown`
whenever the source namespace's ref lane has any pending append or an active leader tenure
(`rt.lane_state != Ready || !rt.pending.empty() || rt.leader_active`). Every replica is also a receiver;
each failing fetch costs it a precommit at T1 and a precommit-removal at abort, both appends to its own
lane; merges, mutations and inserts append too. On GCS a tenure is long because every flush ends with a
token-CAS on `_ckpt` that hits the one-mutation-per-second object limit and backs off. So neither side
ever sees the other's lane quiescent. `SYSTEM STOP FETCHES` on one replica made it answer zero
`Unknown` and let the other drain 1701 → 48 in two minutes; with fetches still stopped but 13 merges
running the same replica answered `Unknown` to 891 of 898 confirms. On RustFS in a LAN tenures are
milliseconds and quiescent gaps are common, which is why no soak saw this before.

**Why rule 3 cannot simply go.** The TLA+ model `docs/superpowers/models/CaRelinkConfirmCore.tla`
(`SabotageStaleCache`) shows the counterexample: the leader has made a transaction carrying a removal
durable but not yet applied it to the in-memory row (the leader holds `state_mutex` only for copy-out
and apply-after-commit, not for the `PUT`); a confirm in that gap reads the stale row, answers `Yes`,
the receiver promotes over blobs whose only protection is already durably gone, and GC can reclaim
them. The hazard is exactly the interval "durable, not yet applied". A `pending` item not yet carved
and a `PUT` still in flight are not durable and do not break the T1 < T2 argument; today's rule
refuses far more than the argument requires.

**Decision (2026-09-02, spec above).** Rule 3 refuses only a broken lane (`Wedged`, `NeedsRecovery`,
`Closed`, `Faulted`). The removal hazard is carried by gate 0 and the part state machine: the only
entry to physical removal is `asMutableDeletingPart` (`Deleting`/`DeleteOnDestroy` only), the ref drop
lives inside `remove()`, and renames go through `republishRef` publish-then-drop. Verified path by
path in code before the decision; the TLA+ model must carry the part state and the ordering before
code, and the two-model consult is mandatory.

**Options considered.**

1. **Narrow rule 3 to a "durable-but-unapplied" flag.** Superseded: `RefLaneState::Writing` already
   spans the `PUT`, the `_ckpt` frontier publication with its 429 backoff, and the apply, so a flag
   raised before the `PUT` would have kept the same long window on GCS. Kept here as the reasoning
   trail.
2. **Bounded wait for quiescence inside the confirm** on `rt.cv` (the leader already notifies).
   Pros: predicate unchanged, tiny. Cons: under continuous load the next tenure starts the instant the
   previous ends, so there is no gap to wait for; holds an interserver handler thread; must not wait
   under `ref_queue_mutex`. Complement at best.
3. **Receiver-side damping.** A per-source circuit breaker (N `Unknown` in a row → pause every relink
   attempt to that source for T) instead of 16 concurrent fetches each on per-entry backoff; possibly
   keep the prepared relink across retries instead of paying precommit + removal per attempt. Pros:
   safety-neutral, stops the self-feeding half, shrinks GC debris. Cons: does not help when the source
   lane is busy with its own merges and inserts, which on GCS is most of the time; keeping the handle
   across queue executions needs state beyond one fetch. **Defense in depth after option 1.**
4. **Let the confirm read the durable journal.** Rejected: the zero-I/O contract exists so a remote
   peer cannot make this writer do work.
5. **Byte-fetch fallback after N failed confirms.** Rejected by taxonomy row 3 in
   `DataPartsExchange.cpp`: the byte request goes to the very source whose state is in doubt.
6. **Shorten tenures on GCS** (the `_ckpt` items below). Necessary anyway, not a liveness guarantee.

**Operational workaround that works today:** alternate `SYSTEM STOP FETCHES` on one replica while the
other drains, then swap; converged the stand to 244,024 = 244,024 rows on 2026-09-02.

**Verification gates for any fix:** the TLA variant green under all sabotage flags; `CAS*` gtest; a
two-hour phase-3 soak on the GCS stand (the only reproduction), read through the confirm-refusal
counters below.

## Failure class 2: hot control keys and the GCS mutation limit (F3, F12) {#gcs-hot-control-keys-429}

- **[gcs-hot-control-keys-429] single-key control objects exceed GCS's one-mutation-per-second object
  limit** — HARD / RELEASE GATE for GCS.

**What happened.** GCS allows about one mutation per second per object name and answers
`429 SlowDown` above it (reads are not limited). Two CAS objects are mutated faster than that.

- `cas/ns/state/<ns>/_ckpt`, one per namespace: a merge-CAS singleton of four fields (`life_epoch`,
  `committed_through`, `checkpoint_snapshot_id`, `last_epoch_seal`), published synchronously inside the
  lane tenure after every durable ref-log chunk as the committed frontier (`CasRefLedger.cpp`, the
  `commit_contribution` publish), plus at birth, epoch seal and snapshot. The path is controlled: 429
  classifies as `Unresolved`, then exact-read, backoff and retry within the request budget
  (`operation_deadline_ms` 90 s, 16 attempts, 0.2 → 5 s backoff). Result: no failed query in the soak,
  1.5k 429s per node in ninety minutes, commit rate per table capped near one per second, tenures
  stretched by backoff (which is what feeds F11), and a `NeedsRecovery` runtime if a budget ever runs
  out.
- `cas/ref_catalog`, one per pool: two CAS writes per `CREATE TABLE` (`Creating`, then `Live`) and two
  per `DROP`. The path is not controlled: `casUpdateImpl` calls `backend.casPut` directly, retries only
  `Conflict` (up to 100 attempts, no backoff) and rethrows any S3 error. The parallel stateless lane
  exceeded the limit at once (1,694 `SlowDown` on the catalog) and about 40% of tests failed at
  `CREATE TABLE` with `S3_ERROR`.

**Options for the catalog.**

- **B1. Route catalog writes through the controlled path** with the ledger's budget. Pros: small,
  symmetric with the lane, DDL stops failing. Cons: `CREATE` may take seconds under contention; the pool
  keeps a ceiling of roughly one lifecycle transition per second; `Conflict` re-read semantics must
  stay. **Preferred first step.**
- **B2. Jittered backoff in the conflict loop** instead of 100 back-to-back attempts. Pros: trivial,
  removes the herd. Cons: no effect on the ceiling. **Do with B1.**
- **B3. Group-commit the catalog** through its own lane, one CAS write for many concurrent transitions.
  Pros: DDL throughput decoupled from the object limit. Cons: new lane machinery with its own liveness
  questions (F11 again); pays off only under mass DDL, which is test suites rather than production.
  Only if needed.
- **B4. Shard the catalog** by namespace hash. Rejected for now: the catalog is the atomic life index
  that proves ownership (INV-3); readers need it whole; a format migration for one provider.
- **B5. Fewer writes per transition** (write `Live` directly). Rejected: two-phase creation is the
  crash-safety mechanism for a stalled `Creating`.

**Options for `_ckpt`.**

- **A1. Coalesce committed-frontier publications** to at most one per T seconds or K flushes per
  namespace, keeping birth, epoch seal and snapshot publications immediate. Pros: an order of magnitude
  fewer mutations; shorter tenures; no new object kinds. Cons: `committed_through` lags, recovery
  replays a longer log tail via LIST, floor cleanup is delayed by T; must show the lag is safe at every
  reader (INV-4 revalidation, cross-epoch GC fold); unmount and shutdown must flush the pending publish.
  **Preferred; the more conservative of the two structural options.**
- **A2. Publish `_ckpt` from an asynchronous publisher** outside the tenure ("after the last flush and
  at least every T"). Pros: the strongest effect on F11, the tenure no longer contains a CAS on a hot
  object. Cons: same lag semantics as A1 plus a background actor with fence and teardown; the
  `NeedsRecovery` path ("journal durable, frontier not installed", an error today) becomes the steady
  state and must be redefined. Alternative to A1 if A1 proves insufficient.
- **A3. Per-object token bucket on the GCS dialect.** Pros: turns 429 storms into orderly local waits,
  fewer requests. Cons: same ceiling; waiting inside the tenure makes F11 worse unless combined with A2.
- **A4. Rotating or generation-suffixed `_ckpt` key.** Rejected: every reader would have to find the
  latest; layout change for one provider.

**Until fixed, document the limit:** roughly one commit per second per table and one namespace
lifecycle transition per second per pool on GCS. `docs/en/antalya/cas/bucket-requirements.md` does not
mention it yet.

## Environment findings, recorded so nobody chases them twice {#environment}

- **[gc-run-connect-failure-propagation] a manual `SYSTEM CAS GC RUN` surfaces a connect-level
  failure as `S3_ERROR`** — DESIRABLE — During the provider connect-timeout storm the background
  scheduler simply retried next round; the synchronous command threw to its caller after one failed
  control-object `PUT` and failed a live-gate scenario. Decide whether the manual round should absorb
  transport-level failures the same way. The live gate's manual-round loop is strict by design.
- **[gcs-wan-read-latency] cacheless CAS reads over a WAN** — PERFORMANCE / DOCS — Soak SELECTs on the
  GCS stand: n=537, p50 3.6 s, p99 15.7 s, about 725 `S3GetObject` per query (one ranged GET per column
  file per part, each paying WAN latency). Not a defect; document that a WAN deployment wants a
  `cache` disk in front of the CAS disk, as the stateless lane's optional policy already does.
- **Connect-timeout storm 01:12-01:22 UTC.** Five Google front-end IPs, up to 2.9k timeouts per
  minute on one replica; host conntrack, ports and a fresh connect from the host were all healthy.
  Provider or path. The product's behaviour under it is the positive result recorded above.

## Observability and docs items {#observability}

- **[confirm-refusal-reasons]** — DESIRABLE — `Relink confirm is unproven (unknown)` names no rule.
  A ProfileEvent per refusing rule (residency, warm, lane busy, row mismatch, fence) and the same
  breakdown in the debug line would have cut the F11 investigation from an hour to a minute.
- **[cas-throttling-by-key-class]** — DESIRABLE — ProfileEvents for `SlowDown`/429 on CAS control
  writes by key class (`_ckpt`, `ref_catalog`, `gc/state`, lease objects), so F3/F12 show up as counters
  instead of log greps.
- **[bucket-requirements-gcs-limits]** — DOCS — State the one-mutation-per-second object limit, its
  consequences for commit and DDL rate, and the cache-disk recommendation for WAN reads.

## Harness follow-ups deferred from the campaign reviews {#harness}

- Default container names are duplicated across `soak/chaos.py`, `soak/pool.py`,
  `scenarios/framework/observe.py`; one module should own them.
- `observe.RUSTFS_CONTAINER` is still an import-time constant used by `scenarios/cards/s15_s18_shards_lifecycle.py`,
  `s28_s33_corner.py`, `s34_s35_d1_churn.py` and `scripts/t8_s44_stuck_removing_discrimination.py`; the
  same defect the 2026-09-02 fix removed from `soak`. `lifecycle.DEFAULT_FSCK_CONTAINER` is dead.
- The `object_kind != 'none'` filter in `test_gcs_live` has no local coverage; the stub-driven test
  passes with or without it.

## Recommended order {#order}

1. **B1 + B2 + the observability items.** Small, safety-neutral, unblocks DDL and gives the counters the
   next two steps are read through. Gates: `CAS*` gtest, a 429 mode in `gcs_mocks/server.py`, a short
   stateless-lane sample on GCS without `S3_ERROR` on `CREATE`.
2. **F11 option 1.** Start the brainstorming immediately, in parallel with step 1: predicate, TLA
   variant, two-model consult, TDD, fresh review. Gate: a two-hour phase-3 soak on the GCS stand.
3. **A1.** After F11, since it shortens tenures but guarantees nothing by itself. Gate: the soak again,
   with 429 counts on `_ckpt` before and after.
4. **By soak results:** F11 option 3 if abort churn still inflates GC; B3 only if mass DDL on GCS is a
   real need; A2 if A1 leaves tenures too long.

Open for brainstorming before code: for A1, which publications stay immediate, the coalescing policy,
the semantics of a lagging `committed_through` for GC fold, cleanup and INV-4, unmount behaviour, and the
new meaning of `NeedsRecovery`; for B1, the DDL budget, tolerance for multi-second `CREATE`,
`Unresolved` handling on the catalog, time-bounding the conflict loop; for F11, the exact flag
semantics across chunked flush and `Unresolved`, the interplay with rule 4, whether `pending` still
matters at all given `build_ops` is a closure whose refs are unknown before carve.
