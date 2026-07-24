# CAS mount-lease renewal ambiguity — fence-not-rescue fix (rev.3) — design

**Status:** DESIGN rev.3 (2026-07-24), user-approved simplification of rev.2.
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
latter). Forensics in the message and the `MountConflict` event: observed vs local `seq`, observed
`pid`/`hostname`/`started_at_ms`. Recovery is uniform and already proven: `ABORTED` →
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
30 s by default — not by any protocol constant). Change: `renewOnce` captures the attempt-start
instant **before calling `prepareRenew`** (so the anchor precedes the payload's own wall-clock
stamp, keeping `anchor ≤ durable stamp` strict) and passes it to the success hooks; both deadlines
become `attempt_start + TTL` (each site keeps its own clock domain). The local deadline is then ≤ the
durable one **by construction**, closing the skew for good rather than relying on
`request timeout < stale-token observation wait` staying true across config changes. ~5 lines +
one test; independent of every other phase.

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
  victim uuid FROM a surviving mount and retires the slot as its purpose): guard bypassed —
  operator-driven, and the guard's target (accidental same-pair revival by a normally restarting
  server) does not apply. Pinned by a test.

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
enabled only on authoritative absence) with an epoch-presence bit and one negative control
(guard removed + epoch wipe ⇒ same-pair two-writer state reachable). The rev.2-scale rework
(request/response split, stale deliveries, held tokens, fairness) is not needed — nothing new is
adopted based on reads.

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
  with a controllable delay).
- Phase C: epoch absent + mount `Present` → `CORRUPTED_DATA`; + authoritative `KeyAbsent` → mints
  (bootstrap unbroken); + `Indeterminate` → fail closed; `DecommissionRecovery` + surviving mount
  + missing epoch → proceeds; epoch present → no probe issued (counting backend).
- Full suites: `CasHeartbeat.*`, `CasMountLease.*`, `gtest_cas_mount.cpp`,
  `gtest_cas_operation_gate.cpp`, `gtest_cas_decommission.cpp`.

**Gate 3 — live.** CAS-s3 stateless lane on the fixed build (the lane that caught the crash).

## Implementation order {#order}

Phase A (the fix; classifier-only, TLA-aligned) → B (anchoring) → C (guard). Each with its tests.
