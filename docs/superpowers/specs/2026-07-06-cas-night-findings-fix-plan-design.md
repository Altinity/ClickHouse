# CAS night-campaign findings — fix plan (design) {#design}

Date: 2026-07-06. Approved approach: **A — "instrument first, then surgery"** (four dependent
phases). Source analysis: the 2026-07-05/06 full-scale scenario campaign
(`docs/superpowers/worklogs/2026-07-03-scenarios-full-scale-campaign.md`,
`utils/ca-soak/scenarios/BACKLOG.md`) plus a four-way post-campaign re-audit of the raw data
(all run `report.md`s, all scenario runner logs, both nodes' full-night server logs, and the
introspection code surface).

## Re-audit summary — what the fix plan answers {#re-audit}

The campaign's headline conclusions stand (data correctness sound at every scale; memory bounded;
one availability bug; an O(N) S3-budget family). The re-audit ADDED the following, and this plan
exists to fix all of it:

**Product findings (P1–P6):**
- **P1 (HIGH, availability)** — the mount-lease "foreign writer" collision is a CHRONIC all-night
  race (~26 occurrences on both nodes, 18:03–05:01 UTC, mostly self-healed by retry), not an
  S13-only event; the S13 wedge was the 5th `touched while adopting our own mount slot` on ch1
  escalating to a permanent fail-closed (exit 49). A SECOND distinct bug rides along:
  `CAS mount-lease: release before start on key ...` (`LOGICAL_ERROR`) thrown during Store
  teardown — release called on a lease whose start never registered.
- **P2** — `delete_pending blob recovered in-degree — structurally impossible under the ack floor
  (spared anyway, fail-closed)` fired for **116 blobs in a 4-minute window** (03:29 UTC, ch1 only).
  An invariant the code believes impossible fired massively; fail-closed spared the blobs, but the
  root cause is unknown.
- **P3** — `stageManifest: manifest ordinal collision` fires ~12×/night and is normally retried by
  the background executor; ONE instance (05:04:29, 28 s after ch1 got mount-fenced) escaped to a
  foreground query as `LOGICAL_ERROR` — the collision-retry path appears to assume a healthy
  mount-lease.
- **P4** — S3 `InvalidPart` on multipart completion (48 ETags rejected) at the tail of the S13
  kill window — suspicion: a stale multipart session is being completed/resumed after a writer
  kill instead of aborted and restarted.
- **P5 (liveness)** — GC round 31 on ch1 stalled ~4 hours under leader contention (8 attempts,
  `manifests_deleted=0` in 6 of them); 2:1 leadership skew (ch1 round 310 vs ch2 173);
  `clamp anomaly ... SUPPRESSED (carried)` 188× on ch1 only (up to 64/pass); chronic
  `heartbeat acked round N but published M (lag > 2)`. Fail-safe, but reclaim can fall behind
  indefinitely under sustained concurrent load.
- **P6** — S09 mutation carry-forward FLAKY suspicion: the failing run showed 2876 MiB pool growth
  with a 34.5% S3 write-error rate; the passing rerun 1.4 MiB with 12.5%. The campaign attributed
  the FAIL to a merge-in-measurement-window confound; the alternative — an S3 write failure knocks
  `ALTER UPDATE` off reference carry-forward onto a full re-upload — was not ruled out.

**Harness findings (H1–H6) — these made several night verdicts vacuous:**
- **H1** — the harness `gc_log` query references dropped columns `forgotten_on_delete` /
  `forgotten_absent` (removed by the ack-floor redesign) → `UNKNOWN_IDENTIFIER` 213×/night →
  `gc_log` capture returned EMPTY for every scenario → every "GC no Failed rounds → pass" and all
  GC-duration/throughput verdicts of the campaign were vacuous.
- **H2** — `pool_shape` prefix classifier predates the per-server-tree relocation: S08 showed
  858081 objects / 138 GB in "other" and `_manifests=0` for a 100k-part pool.
- **H3** — container-cgroup `memory.peak` includes page cache (5–21× above the CH-tracked peak) —
  memory verdicts based on it are unreliable evidence.
- **H4** — the end-checkpoint `forced_gc_to_fixpoint` races the background GC tick ("GC round
  raced background tick (ABORTED)") — the direct cause of 7/15 runs ending INCONCLUSIVE.
- **H5** — sustained 10–20% S3 read-error rates (RustFS timeouts under load) are invisible in
  verdict tables; only op counts are asserted, never error rates.
- **H6** — S04/S05 have multi-minute silent setup phases (no progress logging); S13's
  "recent GC leader (unknown)" label never resolves the actual leader.

**Introspection findings (I1–I4):**
- **I1** — CAS has ZERO gauges: no `CurrentMetrics`, no `AsynchronousMetrics` entries at all.
  Everything is a historical log table or a monotone counter.
- **I2** — mount/lease state (who holds which slot, renewal health, fenced state) is invisible
  outside `err.log`; the S13 wedge was diagnosable only from raw error-log lines.
- **I3** — the per-mount `HeartbeatFloor` breakdown (per-srid `observed_gc_round`, live /
  terminated / fenced classification, lagging list) is computed EVERY GC round and discarded;
  only `min_ack` and `fence_outs` survive into the GC log.
- **I4** — `fsck` is O(pool) in memory and time, has no scoping, and on timeout returns NOTHING
  (the campaign's 100k-part run lost 4 verdicts to this).

## Phase 1 — restore harness honesty {#phase-1}

Scope: `utils/ca-soak/scenarios/framework/*` + touched cards. No product code.

1. **`gc_log_rows` column fix (H1):** remove `forgotten_on_delete`/`forgotten_absent` from the
   SELECT; reconcile the column list against `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`.
   Acceptance: a smoke run's report carries populated `gc_log.per_node` rows and the night's
   `UNKNOWN_IDENTIFIER` error is absent from `err.log`.
2. **`pool_shape` classifier (H2):** classify by the current layout — `blobs/`,
   `cas/manifests/<srid>/`, `cas/refs/<srid>/`, `roots/<srid>/`, `gc/` — and report a
   near-empty "other". Acceptance: a run with parts shows `_manifests > 0` and "other" is a small
   residue, not the dominant bucket.
3. **Memory verdicts (H3):** source peak memory from the CH tracker
   (`peak_mem_resident_by_node` / `system.query_log`); keep the cgroup number as info-only.
4. **Checkpoint vs background GC (H4):** make `forced_gc_to_fixpoint` treat the
   raced-background-tick `ABORTED` as retry-until-clean (bounded retries), and gate settle on
   RECLAIMABLE prefixes only (existing `HARNESS-DRAIN-VERDICT-CONVERGENCE` item). The proper
   product-side fix (B197 `SYSTEM STOP` for CAS GC) stays a release-gate item, out of scope here.
5. **Small items (H5, H6):** an info-level S3 error-rate row in every report; progress logging in
   S04/S05 setup loops (match S14's per-50-tables pattern).

## Phase 2 — introspection package (also the phase-3 debugging instrument) {#phase-2}

Scope: product code. Each item independently shippable.

1. **`system.content_addressed_mounts` (I2, I3):** one row per srid, decoded from the mount
   bodies the GC already reads (`computeHeartbeatFloor` decode logic): `disk_name, srid,
   server_uuid, writer_epoch, expires_at_ms, min_active, observed_gc_round, gc_fenced,
   state (live/terminated/fenced)`. Point-in-time state lives here — the GC log is NOT widened
   with per-mount detail (YAGNI).
2. **Mount-slot writer audit events (P1 instrument):** `content_addressed_log` events for every
   mount-slot write — mint / renew / adopt / release / fence — carrying the writer identity
   (`server_uuid`, incarnation/epoch, `gc_id` where relevant) and the observed/new token. This
   directly answers "who is the foreign writer" in the P1 investigation.
3. **First CAS gauges (I1):** `CurrentMetrics::CasGcIsLeader` (0/1 per disk) and
   `CurrentMetrics::CasGcPendingReclaim` (condemned − graduated backlog), updated at round end in
   the scheduler.
4. **fsck scalability (I4):** on deadline, return the partial accumulated summary flagged
   `partial=true` instead of throwing with nothing; add `--namespace <ns>` scan scoping.

Acceptance: mounts table shows both soak nodes live; gauges visible in `system.metrics`; a
kill-restart cycle produces adopt/renew audit rows; `fsck --namespace` bounds the scan; fsck on a
too-large pool returns partial results.

## Phase 3 — product bugs, root-cause first {#phase-3}

Method: `systematic-debugging` — no fix before a root cause; design-sensitive fixes (3.1, 3.2/3.5)
get a spec + TLA+ gate BEFORE code. Order:

1. **3.1 Mount-lease race + wedge (P1):** reproduce with an S13-style rapid-kill loop using the
   phase-2 audit events; identify the foreign writer (suspects: GC heartbeat scan writing peer
   slots; a killed incarnation's delayed renewal landing post-restart). Fix direction: self-adopt
   decides by server-uuid OWNERSHIP (re-read + compare uuid, retry the CAS) rather than by token
   equality, so "my own stale/renewed lease" is always adoptable while a true foreign owner still
   fails closed; fix the `release before start` teardown throw as its own bug. **TLA+:** extend
   `CaCasMountCore` with rapid restart + delayed-renewal interleavings; the fixed protocol must
   pass; the old one should exhibit the wedge. Acceptance: S13 full green 3× consecutively; the
   all-night chronic collisions disappear from a soak err.log (or degrade to debug-level benign
   retries).
2. **3.2 "Structurally impossible" in-degree recovery (P2):** reconstruct the 03:29 window from
   `content_addressed_log` + GC journal (which scenario/workload, which blobs); determine whether
   the ack-floor invariant is actually violable (protocol hole) or the check's assumption is wrong
   under clamp-suppressed/carried state. Deliverable: a written root cause; then either a protocol
   fix (with TLA+ check) or a corrected invariant/message; a regression test either way.
3. **3.5 GC liveness under contention (P5):** root-cause the 4-hour round-31 stall (leader
   ping-pong restarting the fold from scratch each takeover?). Fix direction: round work must
   survive leader change (resume from sealed cursors / adopt the predecessor's attempt artifacts)
   or leader stickiness with backoff; verify carried (clamp-suppressed) graduations actually drain
   once clamps clear; explain the ch1-only clamp asymmetry and chronic heartbeat lag>2. Bundled
   with 3.2 (same fold/ack-floor semantics). TLA+ liveness check: every owner-removal event is
   eventually folded under leader churn.
4. **3.3 Ordinal collision under fence (P3):** make the `stageManifest` collision path re-check
   mount state and surface a RETRYABLE error (not `LOGICAL_ERROR`) when the lease is fenced/lost;
   unit test: fenced lease + staged-manifest collision → retryable, no LOGICAL_ERROR.
5. **3.4 InvalidPart multipart after kill (P4):** audit the blob upload path for multipart-session
   reuse across incarnations; ensure a dead writer's session is ABORTED and the upload restarts
   fresh; assert via the S13 card (no `InvalidPart` in server logs during chaos runs).
6. **3.6 S09 carry-forward re-triage (P6):** mine the failing run's `ca_events` /
   `content_addressed_log` (body_put vs dedup counts inside the mutation window) to decide:
   merge-confound (close, keep the card fix) vs real fallback-to-reupload on S3 write error (then
   fix: mutation retry must retry the CAS reference commit, not fall back to a body re-upload).

## Phase 4 — the O(N) apex: GC fold skip-unchanged {#phase-4}

One item only — the shared root of "93 s @ 10k tables / 398 s @ 100k parts per round" and
"~2500 S3 ops per idle round":

- Short-circuit a GC round to ~O(servers) reads when the journal/ack-floor shows ZERO new
  transitions since the last sealed generation ("nothing to do" round).
- Skip-unchanged-namespace pruning in `discoverUniverse`/fold so round cost is O(delta), not
  O(universe).
- **Mandatory TLA+ gate:** pruning must provably never skip folding an owner-removal event (the
  exact failure mode of the 2026-06-27 concurrent-leader leak).
- Acceptance: idle-pool round S3 ops drop from ~2500 to near-zero (S03 card gains an ops/round
  budget assertion); a small-delta round on a 10k-table pool is far below 93 s.

Explicitly OUT of scope (stay as separate ROADMAP items): scratch=full-part-size spill, replicated
OPTIMIZE double-spill, wide-part O(columns) batching, partitioned-INSERT O(partitions) commit
batching, startup O(refs), the O(N²) backend LIST fix (release gate #16), fsck streaming rewrite.

## Validation & mechanics {#validation}

- After phases land: re-run S03, S05, S08, S13 (full scale where host-feasible, ci scale
  otherwise) on the FIXED harness — the first time these verdicts are all real (populated gc_log,
  correct pool_shape, tracked memory). S13 is the P1 acceptance gate (3× green).
- Each phase gets its own implementation plan (`writing-plans`) and its own branch; phases 1–2 are
  ordinary TDD work; 3.1, 3.2/3.5 and phase 4 are design-sensitive: spec + TLA+ gate before code.
- BACKLOG entries for P1–P6 and H1–H6 are updated with root causes/resolutions as each lands.
