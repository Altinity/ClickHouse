# CAS mount-lease fence recovery — design (P3.1 fix) {#design}

Date: 2026-07-06. Root cause: `docs/superpowers/worklogs/2026-07-06-p31-mount-lease-root-cause.md`
(CONFIRMED: the "foreign writer" is the GC leader's legitimate fence-out of a lease that expired
because the BEAT blocks renewals; the S13 wedge is the fence landing inside the adopt's
non-atomic GET→PUT window during `Store::open`, which has no retry). User approved fix vectors
A–E (2026-07-06).

## Protocol decisions {#decisions}

### E — "a fence costs an epoch" (the governing rule) {#fence-costs-epoch}

A `gc_fenced = true` mount body is TERMINAL for its `(server_uuid, writer_epoch)` incarnation —
this is already the fence-out's documented intent; the fix makes every path honor it:

- `MountLeaseKeeper::claim` (adopt) additionally checks `observed.gc_fenced`: same uuid + same
  epoch + FENCED is NOT adoptable — the incarnation must recover as a NEW incarnation (fresh
  `writer_epoch` via `allocateWriterEpoch`, then `claimMount` reclaim). No same-epoch resurrection,
  ever.
- `claimMount`'s same-uuid/same-epoch "refresh" branch gets the same guard (fenced → not
  refreshable; caller must re-open with a new epoch).
- The reclaim branch (same uuid, DIFFERENT epoch, fenced/expired) is already correct: reclaim
  immediately, epoch is fresh by construction.

### A — diagnose by BODY, not by token {#diagnose-by-body}

On `PreconditionFailed` in the keeper's renewal (`renewOnce`) and adopt (`claim`), RE-READ the
slot once and decide by the observed body:
- foreign `server_uuid` → fail closed (unchanged), message names the holder (already enriched).
- own uuid + `gc_fenced` → "fenced by GC after lease expiry (late renewal/adopt)" — a RECOVERABLE
  state: renewal path latches lost + `scheduleRemount` (as today, but with an honest message and a
  `mount_conflict` event whose `branch = "fenced_by_gc"`); adopt path at open retries via the C
  loop below (new epoch per E).
- own uuid + newer epoch (not fenced) → superseded by our own newer incarnation → fail closed
  (unchanged semantics, honest message).
- The generic `SingleWriterSlot` "touched by a foreign writer" text is reserved for the truly
  unexplained case (own uuid, same epoch, not fenced, token moved — a genuine single-writer
  violation): stays LOUD and fail-closed.

`SingleWriterSlot::renewOnce` therefore needs a virtual hook (e.g. `onRenewPreconditionFailed`
returning a classified refusal) or the re-read happens in `MountLeaseKeeper`-specific code — pick
the smallest seam; the base class stays generic.

### B — renewals never wait for the beat {#renew-decoupled}

`prepareRenew` must be O(1): it reads the LAST INSTALLED ack (`retire_view.round()`) and
`minActive()` without running the beat. The beat (`refreshViewForBeat` — gc/state probe +
exclusive `view_gate` + retired-view load) moves to its own cadence off the renewal thread (same
period, separate timer/thread, or an async step the renewal thread triggers but does not await).
Ack LAG is what the heartbeat floor is designed for (`lagging`, `CasGcFloorHeldByStaleAck`); lease
EXPIRY is what it cannot tolerate. After this change a renewal is one `putOverwrite`, and a
30 s TTL cannot be breached by S3-slow view loads.

Floor-safety note for the model: the ack carried by a renewal may now be STALER than the beat
would have produced, never fresher — strictly conservative for the floor (min_ack can only be
lower), so graduation only slows down. The open-ordering invariant (first anchored body carries an
ack from a post-claim gc/state read) is PRESERVED by keeping the beat synchronous in `doStart`'s
FIRST payload only if the model shows it is required — see the open question below.

### C — bounded claim/adopt retry during `Store::open` {#open-retry}

The cold-open path gets the same resilience the running path already has: wrap the
claim→keeper-start sequence in a bounded retry loop (reuse `claimMountAwaitingExpiry`'s bound:
ttl + margin). On an adopt refusal classified as `fenced_by_gc` (per A): allocate a fresh epoch
(per E) and re-run `claimMount` + adopt. On a true foreign/superseded refusal: abort as today.
The S13 wedge becomes at worst one extra epoch bump + a few seconds of retry.

### D — teardown of a never-started keeper is a no-op {#terminate-noop}

`SingleWriterSlot::doTerminate` with `seq == 0` (start never succeeded) returns quietly instead of
throwing `LOGICAL_ERROR "release before start"` — there is nothing to release; the slot was never
claimed by this keeper. (A double terminate stays loud.)

## TLA+ gate (phase 0 — BEFORE any code) {#tla-gate}

Extend `docs/superpowers/models/CaCasMountCore.tla` (current model: atomic `ClaimMount`, `Renew`,
`Tick`, no GC, 252 lines):

1. **New action `GcFence`**: any actor may stamp `gc_fenced` on an EXPIRED, unterminated, unfenced
   mount (token changes; uuid/epoch preserved) — models `computeHeartbeatFloor`'s fence-out.
2. **Split the adopt into GET then CAS** (two steps with an interleaving point), modeling the real
   non-atomic `claim()`; `Renew` similarly carries the token it read last.
3. **Late renewal**: allow `Renew` to fire after expiry (the beat-blocked renewal) — with the OLD
   protocol this + `GcFence` reproduces the "foreign writer" refusal; with the fix it classifies
   as fenced-by-gc and recovers via remount (new epoch).
4. **New invariants**:
   - `FENCE-COSTS-EPOCH`: no write ever lands under a `(uuid, epoch)` that was fenced.
   - Existing `ForeignUuidNeverAutoTakesOver`, `WriterEpochMonotoneUnique`,
     `SupersededWriterMakesNoMutation`, `NoTwoServerUuidsOwnSameServerRoot` all still hold.
5. **New liveness witness `W_CrashedServerEventuallyRemounts`**: a server whose lease was fenced
   (crash or late renewal) eventually holds a live mount again (under fairness), i.e. NO PERMANENT
   WEDGE.
6. **Sabotage configs**: (a) the OLD adopt (no `gc_fenced` check, no retry) must violate the
   liveness witness (exhibits the wedge); (b) same-epoch resurrection after fence must violate
   `FENCE-COSTS-EPOCH`.
7. **Open question the model must answer**: may `doStart`'s FIRST anchored ack also come from the
   installed view (fully async beat), or must the first body's ack be post-claim-fresh? Model the
   GC floor's spare decision against a stale first ack; if safety holds, B applies to `doStart`
   too (simpler code); if not, `doStart` keeps one synchronous beat BUT the C retry loop makes the
   expiry-during-open case recoverable anyway.

Gate: fixed-protocol model GREEN on all invariants + witnesses; both sabotage configs produce the
expected counterexamples. Only then implementation.

## Implementation acceptance {#acceptance}

- Unit (gtest, `Cas*` sweep green): fence-lands-in-adopt-window → open retries with a new epoch
  and mounts (no exception escapes `Store::open`); renewal PreconditionFailed over a fenced body →
  honest classification + remount schedule; `doTerminate` at `seq==0` → no-op; same-epoch
  adopt/refresh over fenced body → refused.
- Message honesty: "touched by a foreign writer" appears ONLY for the genuine
  single-writer-violation case; fence recoveries say "fenced by GC after lease expiry".
- Events: `mount_conflict` with `branch=fenced_by_gc` on the recoverable path.
- Live: S13 full-scale GREEN 3× consecutively (the P1 acceptance gate); a soak err.log carries no
  misleading foreign-writer text for fence recoveries.
- The chronic-collision rate itself should DROP (vector B removes the beat-blocked-renewal cause);
  the remaining fences (genuine long stalls/crashes) recover cleanly.

## Out of scope {#out-of-scope}

- The beat's own cost/frequency tuning (Phase 4 territory).
- P2 ("structurally impossible" in-degree), P5 (GC liveness under contention) — next in the
  phase-3 queue; they share the fold semantics, not the mount protocol.
- First-open audit-event blindness (BACKLOG fast-follow, already compensated via err.log).
