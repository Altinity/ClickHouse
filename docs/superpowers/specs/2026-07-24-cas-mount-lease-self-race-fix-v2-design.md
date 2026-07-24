# CAS mount-lease renewal ambiguity — fence-not-rescue fix (rev.4) — design

**Status:** IMPLEMENTED (2026-07-24) — Phases A-C + TLA+ gate + addenda all landed on `cas-gc-rebuild`:
TLA+ gate `8451222bb14` + `f39f2070bbd`; Phase A classifier `2e5b2df7397` + e2e test `07c8770eb0b`;
Phase B keeper anchoring `e6b1d90acc0`, startup-arm `e0ee7af7564` + remount-arm addendum
`25e3e34413c` + regression test `683579789c7`; Phase C guard `6094c1473ea` + follow-up
`d00cc114af8`. `Cas*:CA*` gtest gate green post-landing. Open: the `amd_msan`/`amd_tsan` 6h-hang
question (BACKLOG §8, Task F) is unrelated and still unanswered; Gate 3 (live CAS-s3 stateless-lane
validation) rides the next CI push of `cas-gc-rebuild` rather than a local re-run of the full lane.
rev.3 = the user-approved fence-not-rescue simplification
of rev.2; rev.4 folds in the four findings of the third codex round that survive the
simplification (the round reviewed rev.2; its verdict: `tmp/codex_review_mountlease_v2rev2_verdict.md`;
its six findings against the dropped rescue machinery are MOOT). The surviving four: the
decommission bypass hardening (round-3 №1, Critical), startup-arm anchoring after the
materialization grace (№2, Critical), precise per-domain clock wording (№4), and the
implementation notes (№8/№9 and the closing notes: probe available in both backend modes;
fence-latch testable via `startBackground` + loss callback; all three `mountWritable` allocation
sites take the policy; self-remount stays `NormalMount`).
**Reframing (the rev.3 decision):** rev.2 tried to *rescue* an ambiguous-but-landed renewal in
place (resolve → adopt → continue), which required three proofs — attempt identity (a body nonce),
read coherence (a HEAD-GET-HEAD sandwich), and liveness bounds (prepare-time anchored commit
gates) — plus a large TLA+ rework. rev.3 drops the rescue and restates the problem with plain
lease semantics: **a lease holder that cannot confirm its renewal is not live** — fence, then
self-remount (the existing, battle-tested path). The "did my write land?" question is not
answered; it is made irrelevant. This is also what the round-2 review itself offered as the
acceptable conservative option ("the throw/remount approach is acceptable, but it must be
described as state uncertainty"). rev.2 (with the full resolve design) is preserved in git history
at `dc68a7e7c66` should metrics ever show ambiguous-timeout remounts are frequent enough to be
worth rescuing.
**Branch:** `cas-gc-rebuild` (PR #2073).
**Review history:** v1 refuted by codex round 1 (`tmp/codex_review_mountlease_selfrace_result.txt`);
rev.1-of-v2 returned UNSAFE by codex round 2 (`tmp/codex_review_mountlease_v2_verdict.md` — 7
CONFIRMED findings); round 2 **endorsed the classifier layer unchanged** (now Phase A). Round-2
findings 1-3 and the race-test half of 7 are MOOT under rev.3 (they attacked the rescue); findings
4 and 6 are addressed in Phase C exactly as prescribed (authoritative probe; decommission policy);
finding 5 shrinks to a small model delta; the testability half of 7 is honored in the test plan.
**Origin:** SIGABRT under ASan in CI (Altinity PR #2073, run 30019911967, CAS-s3 stateless):
`LOGICAL_ERROR "CAS mount-lease: key ... was touched by a foreign writer"` from
`SingleWriterSlot::onRenewMismatch` (`CasServerRoot.cpp:1011`) via the `MountLeaseKeeper`
classifier fallthrough (`CasServerRoot.cpp:879`), ~10 s after a client-side `Timeout` on the same
renewal key. Third member of the STID 3982-3b48 family (parts 1a `8742d746d4e`, 1b `cafb64652d0`).

## Problem {#problem}

A mount-lease renewal PUT (`SingleWriterSlot::renewOnce`, `CasServerRoot.cpp:976-1008`) that times
out client-side may have landed server-side (token/seq bumped, ack lost). The next beat then gets
a confirmed `PreconditionFailed` and re-reads a body carrying our own `server_uuid`, our own
`writer_epoch`, not `gc_fenced` — a shape no classifier branch covers, so it falls through to the
base class's unconditional `LOGICAL_ERROR`, which aborts debug/ASan builds at exception
construction.

Two review-established facts constrain any fix:
- "Same uuid + same epoch + unfenced" does **not** prove "our own prior write": the
  `allocateWriterEpoch` absent-epoch path can hand a second same-uuid process the same pair over a
  root whose epoch object vanished (`serverRootSubtreeEmpty` checks only data subtrees,
  `CasServerRoot.cpp:72-84`). The condition is state **uncertainty**, never proof — so the only
  safe reactions are fence (always safe) or a proof-carrying rescue (rev.2's road; rejected as
  disproportionate).
- The `superseded` branch (same uuid, different epoch) is likewise a normal fencing outcome — the
  TLA+ model already treats it as `localLost` (`CaCasMountCore.tla:342`) — yet it too constructs
  `LOGICAL_ERROR` today.

## Phase A — non-aborting, exhaustive classification (the crash fix) {#phase-a}

*(Endorsed verbatim by codex round 2.)* `MountLeaseKeeper::onRenewMismatch`
(`CasServerRoot.cpp:822-880`) becomes exhaustive over body-present cases and loses both abort
paths reachable by normal operation:

| Observed body | Branch | Exception (code) | Aborts? | Change |
|---|---|---|---|---|
| same uuid, `gc_fenced` (any epoch) | `fenced_by_gc` | `MountFencedException` | no | unchanged |
| same uuid, same epoch, unfenced | **NEW `same_epoch_state_uncertain`** | `ABORTED` | no | **added** |
| same uuid, different epoch, unfenced | `superseded` | `LOGICAL_ERROR` → **`ABORTED`** | was yes | **downgraded** |
| different uuid | `foreign_writer` | `LOGICAL_ERROR` | yes | unchanged (deliberate: the owner anchor makes it unreachable by protocol; decommission impersonates the victim uuid, never a foreign one) |
| absent | `vanished` (part 1a) | `FILE_DOESNT_EXIST` | no | unchanged |

The new branch's name and message state what is KNOWN — the slot advanced past our held token
under our own (uuid, epoch); cause uncertain (an ambiguous landed renewal of ours is the common
cause; a same-pair twin after epoch-state loss is the pathological one — Phase C narrows the
latter). Forensics: the exception `message` carries the observed body's full identity
(`describeMountHolder`: `server_uuid`/`hostname`/`pid`/`writer_epoch`/`seq`/`expires_at_ms`) plus
OUR local `seq` ("... vs our seq=N"); the `MountConflict` event carries only the observed body's
identity (`holder_uuid`/`holder_hostname`/`holder_pid`/`holder_epoch`/`holder_seq`/
`holder_expires_at_ms` — `expires_at_ms`, not `started_at_ms`). Recovery is uniform and already proven: `ABORTED` →
`backgroundLoop` confirmed-mismatch path → `onRenewFailed` → `on_lost` → write fence latches →
self-remount re-claims with a fresh durable epoch (`CasPool.cpp:950-1050`; observed working
end-to-end in the very CI run that caught the crash).

Details (all confirmed by round 2): only `su`/`we` are decode-required, other fields default
(`CasServerRootFormats.cpp:138`) — the case split is total; the trailing
`SingleWriterSlot::onRenewMismatch` call becomes unreachable and is removed from this override
(the base stays for other slots); `ABORTED` flows through `backgroundLoop`'s catch-all; no
double-keeper race (renewal and `on_lost` are sequential on one thread; remount stops/joins the
old keeper first, `CasPool.cpp:997`). `CasMountLeaseLost` increments in both changed branches; its
exhaustive-cause description (`ProfileEvents.cpp:886`) gains the new cause; no new ProfileEvent
(the event `outcome` distinguishes branches).

**Cost accepted by this reframing:** an ambiguous-but-landed renewal now costs one self-remount
cycle (fence + fresh-epoch re-claim; the re-claim of an unexpired own slot waits the stale-token
observation interval, ~36.5 s of fenced writes with defaults) instead of being rescued in place.
Frequency: requires a timed-out-yet-applied conditional PUT on one specific key — observed once
per ~2 h under a deliberately flapping backend in CI; ~never on a healthy one, and during a real
backend flap writes are degraded anyway. Writes under the fence fail closed and retryable (the
established `CasWriteRetryLater` path); reads are unaffected.

## Phase B — prepare-time deadline anchoring (small independent hardening) {#phase-b}

`refreshConfirmedDeadline` (`CasServerRoot.cpp:661`) and the `CasMountRuntime` renew-ok callback
(`CasMountRuntime.cpp:81,240`) compute `now + TTL` at **response** time; the durable
`expires_at_ms` was stamped at **prepare** time, so the local fence deadline exceeds the durable
authorization by the request latency (pre-existing; today bounded only by the S3 request timeout,
30 s by default — not by any protocol constant). Change: `renewOnce` captures attempt-start
instants **before calling `prepareRenew`** (so the anchors precede-or-equal the payload's own
wall-clock stamp — the wall anchor IS the payload stamp: `prepareRenew` reads it from the same
`last_attempt_wall_ms` the renewal hooks anchor from) and passes them to the success hooks; both
deadlines become `attempt_start + TTL`.

Clock-domain precision (round-3 №4): the two deadline sites live in different domains — the
keeper's `confirmed_deadline_ms` is wall-clock (`now_ms_fn`), the runtime's `mayMutate` fence is
`CLOCK_BOOTTIME` (`CasMountRuntime.cpp:61,81`) — so ONE timestamp cannot serve both. `renewOnce`
samples **one pre-I/O stamp per domain** (wall + boottime, both before `prepareRenew`) and each
hook anchors in its own domain. The safety statement is therefore not a cross-domain absolute
inequality but the standard lease argument the protocol already makes: within one domain the
anchored deadline precedes the durable stamp by construction, and across nodes the comparison
rides on the clock-rate allowance the successor's stale-token observation already grants
explicitly (the 5% slack at `CasServerRoot.cpp:407`). Phase B removes the *request-latency* term
from the skew — the unbounded one; the rate term was always present and stays modeled.

**Startup-arm coverage (round-3 №2, Critical).** The renewal hooks are not the only fence-arm
site: `mountWritable` claims the slot, may then wait an operator-configured
`materialization_grace_ms` (unbounded; `CasPool.cpp:659`, `CasPool.h:145`), and only afterwards
arms `bootMsNow() + TTL` and starts renewal (`CasPool.cpp:670`). With a grace longer than the
stale-token observation threshold a successor can legally reclaim DURING the wait, and the
predecessor then arms an already-superseded claim without revalidation. Fix, same anchoring
discipline: the startup arm anchors at the **claim-attempt start** (pre-I/O, both domains); if by
arm time the anchored deadline has already elapsed (the grace consumed the TTL), the arm is
refused and the claim is redone — one fresh conditional lease write, which fails closed on a
successor's token — before arming from the new attempt's anchor. A grace shorter than
`TTL − safety margin` (every sane config; grace defaults are seconds, TTL 30 s) never triggers the
redo — zero normal-path cost.

~15 lines + two tests; independent of every other phase.

## Phase C — epoch re-mint guard, authoritative and decommission-aware {#phase-c}

Narrows the same-pair-twin hole that makes the Phase A branch "uncertain" rather than merely
"self-raced". `allocateWriterEpoch` (`CasServerRoot.cpp:151-190`) gains a policy parameter:

- **`NormalMount`** (default; `Pool::mountWritable` startup, self-remount, and the fence-recovery
  re-allocation loop): on the absent-epoch branch, probe the mount key with **`probeSentinelRaw`**
  (`CasBackend.h:132+` — `get`-absence is explicitly non-authoritative and must not gate a
  lifecycle decision):
  - authoritative `KeyAbsent` → proceed to mint (fresh-root bootstrap; concurrent bootstrappers
    still serialize on the epoch CAS loop — a mount appearing between probe and CAS implies its
    creator allocated the epoch first, so our expected-absent CAS conflicts and the loop
    re-observes);
  - `Present` → `CORRUPTED_DATA`: "epoch object missing but a mount lease exists over this
    server-root: durable epoch state was lost while a mount is live or recently live; refusing to
    re-mint epoch 1. If no server is live on this root, decommission it or manually remove the
    stale mount object." (consistent with the recovery advice at `CasServerRoot.cpp:370`);
  - `ContainerAbsent` / `AccessDenied` / `Indeterminate` → fail closed naming the probe outcome.
- **`DecommissionRecovery`** (only `openForDecommission`, `CasPool.cpp:700→735`, which derives the
  victim uuid FROM a surviving mount and retires the slot as its purpose). **NOT a blind bypass**
  (round-3 №1, Critical: a blind bypass would re-mint epoch 1 while a LIVE epoch-1 victim mount
  survives; `claimMount` then same-pair-adopts it immediately — `CasServerRoot.cpp:286` — creating
  exactly the forbidden twin and defeating the existing live-member refusal test,
  `gtest_cas_decommission.cpp:374`). Instead, on the absent-epoch branch this policy:
  - requires the surviving mount to be **terminal** (expired or released) — a LIVE mount refuses,
    preserving decommission's existing live-member refusal semantics;
  - mints an epoch **distinct by construction** from the surviving mount's:
    `max(1, surviving_mount.writer_epoch + 1)` — the same-pair state is unrepresentable on this
    path regardless of any other evidence.

Implementation note (round 3): ALL THREE `allocateWriterEpoch` call sites inside `mountWritable`
receive the policy (`CasPool.cpp:495, 592, 632` — startup claim and the fence-recovery
re-allocations); the self-remount path stays `NormalMount` (`CasPool.cpp:957`). `probeSentinelRaw`
is confirmed implemented for both Native and Emulated backends
(`CasObjectStorageBackend.cpp:735`), and `InstrumentedBackend` preserves the outcome
classification (`CasInstrumentedBackend.h:95`).

Residual hole, honestly stated: epoch AND mount both wiped under a live mount still permits a
same-pair twin — the survivor then fails safe on its next beat (vanished/uncertain → fence), a
bounded ≤ one-beat dual-fence window in a doubly-vandalized environment, degrading to a remount,
never to two silently-live writers persisting. Steady-state cost: zero (probe only on the
absent-epoch path).

## Non-goals {#non-goals}

- **No rescue of ambiguous renewals** (the rev.3 decision). No body nonce, no coherent-read
  machinery, no resolve — `MountLease` format, `Backend::get`, and `renewOnce`'s happy path are
  untouched.
- `claimMount`'s same-pair adopt-on-open, the farewell PUT, GC fencing, `min_active` exposure,
  lease budget constants, `MountFencedException` at `keeperStart` — all untouched.
- `system.content_addressed_log`: `MountConflict` reused with a new `outcome` string; no schema
  change.

## Verification {#verification}

**Gate 1 — TLA+ (small delta).** In `CaCasMountCore.tla`: map the new uncertain branch onto the
existing `localLost` transition (the model already routes `superseded` there — Phase A aligns the
implementation WITH the model rather than extending it); add the Phase C guard (epoch re-mint
enabled only on authoritative absence) with an epoch-presence bit, the **decommission policy
branch** (terminal-mount requirement + distinct-epoch mint), and two negative controls: (a) guard
removed + epoch wipe ⇒ same-pair two-writer state reachable; (b) decommission bypass minting
epoch 1 over a live epoch-1 mount ⇒ same-pair reachable (the round-3 №1 trace). The rev.2-scale
rework (request/response split, stale deliveries, held tokens, fairness) is not needed — nothing
new is adopted based on reads.

**Gate 2 — gtests (TDD, failing-first where the current code aborts).**
- New `ApplyThenThrowPutOverwriteFaultBackend` (applies, then throws — the existing transient mock
  at `gtest_cas_heartbeat.cpp:419` throws before applying). One end-to-end test reproducing the CI
  crash: ambiguous landed renewal → next beat confirmed mismatch → `same_epoch_state_uncertain` →
  fence latches → self-remount with a fresh epoch → **no death** (via the real `backgroundLoop`;
  a direct `renewOnce` throw never latches the fence — round-2 finding 7).
- Classification repartition of `ForeignTouchMakesRenewThrow` (`gtest_cas_heartbeat.cpp:120`):
  same-uuid/same-epoch (its current construction) → `EXPECT_THROW(ABORTED)`, no death; NEW
  different-uuid variant → keeps `EXPECT_DEATH`; NEW same-uuid/different-epoch → `ABORTED`.
- Phase B: deadline equals attempt-start + TTL even when the PUT ack is delayed (fault backend
  with a controllable delay); startup-arm redo: a `materialization_grace_ms` exceeding the TTL
  forces a fresh claim before arming (and a successor's token meanwhile → fail closed, no arm).
  Fence-latch observability is confirmed practical via `startBackground` + the loss callback
  (round-3 №8, citing the existing pattern at `gtest_cas_heartbeat.cpp:442`).
- Phase C: epoch absent + mount `Present` → `CORRUPTED_DATA`; + authoritative `KeyAbsent` → mints
  (bootstrap unbroken); + `Indeterminate` → fail closed; epoch present → no probe issued (counting
  backend). Decommission: missing epoch + **live** epoch-1 mount → **refuses** (round-3 №1);
  missing epoch + terminal mount → proceeds and mints `surviving.writer_epoch + 1` (asserted
  distinct); the existing live-member refusal test (`gtest_cas_decommission.cpp:374`) still
  passes.
- Full suites: `CasHeartbeat.*`, `CasMountLease.*`, `gtest_cas_mount.cpp`,
  `gtest_cas_operation_gate.cpp`, `gtest_cas_decommission.cpp`.

**Gate 3 — live.** CAS-s3 stateless lane on the fixed build (the lane that caught the crash).

## Implementation order {#order}

Phase A (the fix; classifier-only, TLA-aligned) → B (anchoring) → C (guard). Each with its tests.
