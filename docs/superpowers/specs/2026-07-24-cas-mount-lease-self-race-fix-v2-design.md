# CAS mount-lease renewal ambiguity — three-layer fix (v2) — design

**Status:** DESIGN (2026-07-24), approved interactively (scope = all 3 layers; layer-1 mechanism =
inline resolve in `SingleWriterSlot`); pending the second adversarial codex review (gate), then TLA+
phase 0, then implementation.
**Branch:** `cas-gc-rebuild` (PR #2073).
**Supersedes:** `2026-07-24-cas-mount-lease-self-race-fix-proposal.md` (v1) — rejected by the first
codex gpt-5.6-sol adversarial review (verdict SAFE WITH CHANGES, 7 CONFIRMED findings; transcript:
`tmp/codex_review_mountlease_selfrace_result.txt`). v2 incorporates all seven.
**Origin:** SIGABRT under ASan in CI (Altinity PR #2073, run 30019911967, CAS-s3 stateless):
`LOGICAL_ERROR "CAS mount-lease: key ... was touched by a foreign writer"` from
`SingleWriterSlot::onRenewMismatch` (`CasServerRoot.cpp:1011`) via the `MountLeaseKeeper`
classifier fallthrough (`CasServerRoot.cpp:879`), ~10 s after a client-side `Timeout` on the same
renewal key. Third member of the STID 3982-3b48 family (parts 1a `8742d746d4e`, 1b `cafb64652d0`).

## Problem, restated after the v1 review {#problem}

The mount-lease background renewal (`SingleWriterSlot::renewOnce`, `CasServerRoot.cpp:976-1008`)
issues ONE raw conditional PUT (`backend->putOverwrite(key, body, last_token)`) per beat. A
client-side timeout leaves the outcome **permanently unresolved**: the PUT may have landed
server-side (bumping the slot's token/seq) with the ack lost. The next beat then sends *different
bytes* (fresh `now_ms` from `prepareRenew`) under the *stale* expected token, gets a confirmed
`PreconditionFailed`, and lands in `MountLeaseKeeper::onRenewMismatch` — whose re-read shows a body
with our own `server_uuid`, our own `writer_epoch`, not `gc_fenced`. No classifier branch covers
that shape, so it falls through to the base class's unconditional `LOGICAL_ERROR`, which **aborts
debug/ASan builds at exception construction** (before any catch).

The v1 review established three facts that shape this design:

1. **"Same uuid + same epoch + unfenced" does NOT prove "our own prior write"** (v1's central
   claim, refuted). `allocateWriterEpoch` re-mints epoch 1 over a root whose *epoch object*
   vanished while a mount is still live (`serverRootSubtreeEmpty`, `CasServerRoot.cpp:72-84`,
   checks only refs/manifests/root-data — not the mount/epoch control objects), and `claimMount`
   adopts a same-uuid/same-epoch live body immediately with no token-stability observation. A
   second process can therefore legitimately hold our exact pair. The condition is **state
   uncertainty**, never proof.
2. **The `superseded` branch (same uuid, different epoch) is also a normal fencing outcome** that
   still constructs `LOGICAL_ERROR` — the TLA+ model (`CaCasMountCore.tla:342`) already treats it
   as `localLost`, not as a wedge or abort. It must be de-abortified in the same change.
3. **The codebase already owns the principled cure**: `putOverwriteControlled`'s documented
   resolve-by-GET contract (`CasRequestControl.h:324`) — "current token still equals expected →
   provably never applied; current bytes equal ours → committed; neither → genuine conflict".
   Renewal should resolve its own ambiguity at the source rather than let the *next* beat
   misdiagnose it.

Hence three layers: **resolve the ambiguity at the source** (layer 1), **never abort on the
fail-closed classifications** (layer 2), **close the epoch-reuse hole** so layer 2's reasoning is
sound (layer 3).

## Layer 1 — resolve-on-ambiguity in `SingleWriterSlot::renewOnce` {#layer-1}

### Change

`renewOnce` keeps its exact shape (payload off-lock, guards, `encodeBody(seq + 1, payload)`, one
conditional PUT). The single change: the PUT is wrapped so that **any exception it throws**
triggers ONE resolving `backend->get(key)`, three-way:

| GET observes | Meaning | Action |
|---|---|---|
| `bytes == body` (the exact string we just attempted) | our PUT landed; the ack was lost | `recordWrite(seq + 1, got.token)`; return normally (renewal SUCCEEDED — `onRenewSucceeded` fires from `backgroundLoop` as on any success) |
| `token == last_token` | the attempt provably never applied (the slot is byte-for-byte the incarnation we last wrote) | rethrow the ORIGINAL put exception — exactly today's transient path; no state advanced |
| any other (body present, different bytes AND different token) | a genuine competing write landed between our beats | set `last_renew_failure_was_confirmed_mismatch = true`; call `onRenewMismatch(key)` (which re-reads and classifies, as today) |
| object absent | slot vanished under us | set the confirmed flag; call `onRenewMismatch(key)` — its existing vanished branch (part 1a) handles it non-fatally |
| the GET itself throws | still ambiguous | rethrow the ORIGINAL put exception (transient). If the write did land, the NEXT beat's confirmed mismatch is handled non-fatally by layer 2 — defense in depth |

The `res.outcome != PutOutcome::Done` path (the PUT **completed** and reported
`PreconditionFailed`) is untouched — that outcome is already unambiguous; no resolve.

### Why this is correct — corner cases considered {#layer-1-corners}

- **No exception classification needed.** Resolving on ALL put exceptions (not just
  "maybe-landed" ones) is safe: for a definitely-not-sent failure (connection refused, local
  encode error) the GET simply finds `token == last_token` and rethrows the original — behavior
  identical to today plus one GET on an already-failing path (renewals are one per
  `mount_renew_period_ms` = 10 s; failures are rare). This avoids importing
  `CasRequestControl`'s failure classifier.
- **Byte-equality uniquely identifies THIS attempt.** The body encodes `seq + 1` and the payload's
  `now_ms`/`expires_at_ms` (`MountLeaseKeeper::prepareRenew` stamps wall-clock). A previous beat's
  ambiguous body carries the same `seq + 1` (seq only advances via `recordWrite`) but a `now_ms`
  at least one full renew period (seconds) older — bodies differ. Within one beat there is exactly
  one attempt. So `bytes == body` can only be this attempt's landed write. (A frozen wall clock
  across beats would break this; `now_ms_fn` is the same monotonically-sampled wall clock the
  whole lease protocol already depends on — no new assumption.)
- **Token-equality ("never applied") inherits `putOverwriteControlled`'s trust model.** On a
  content-hash-token backend an ABA (foreign write + restore of our exact old bytes) could fake
  `token == last_token`; this is a pre-existing property of the conditional-write token model that
  the documented controlled-overwrite contract already accepts (`CasRequestControl.h:324`). No new
  exposure is introduced.
- **`state_mutex` hold extends by one GET, failure path only.** The documented invariant
  (`renewOnce` has a single driver; `doTerminate` joins the thread before taking the mutex —
  comment at `CasServerRoot.cpp:982-985`) is unchanged. The `dead`/`seq == 0` guard throws stay
  OUTSIDE the resolve wrapper (a programming-bug guard must not trigger a resolve).
- **Confirmed-deadline skew stays bounded.** `refreshConfirmedDeadline` (`CasServerRoot.cpp:661`)
  computes `now + ttl` at ACK time while the durable `expires_at_ms` was stamped at
  `prepareRenew` time, so the local fence deadline can exceed durable expiry by the attempt
  latency. This skew is pre-existing (a slow successful PUT has it today); the resolve path adds
  at most one GET timeout. Bound: skew ≤ `attempt_timeout_ms` (5 s) + one GET (≤ 5 s) = 10 s,
  versus the reclaimer's mandatory stale-token observation wait (~36.5 s in production logs,
  ≥ TTL-scale by construction) before any takeover — no overlap window opens. Documented here so a
  future budget change re-checks this inequality (`attempt_timeout + get_timeout <
  reclaim_observation_wait` must hold).
- **Merged-watermark payload (`min_active`)**: a landed-but-unacked renewal also published a
  `min_active` snapshot. Resolve-commit acknowledges it; the rethrow path leaves it published one
  beat early — the same exposure the ambiguous window has today (values are re-published every
  beat; GC reads them freshness-insensitively at TTL scale). No new hazard.
- **The farewell PUT (`terminate`) is out of scope.** Its ambiguity resolves naturally: a lost
  farewell leaves the lease to expire by TTL (part 1b already made the absent-at-release case a
  no-op); the server is shutting down either way.

## Layer 2 — non-aborting classification in `MountLeaseKeeper::onRenewMismatch` {#layer-2}

### Change

The classifier (`CasServerRoot.cpp:822-880`) becomes **exhaustive over body-present cases** and
loses both abort paths that are reachable by normal protocol operation:

| Observed body | Branch | Exception (code) | Aborts? | Change |
|---|---|---|---|---|
| same uuid, `gc_fenced` (any epoch) | `fenced_by_gc` | `MountFencedException` | no | unchanged |
| same uuid, same epoch, unfenced | **NEW `same_epoch_state_uncertain`** | `ABORTED` | no | **added** |
| same uuid, different epoch, unfenced | `superseded` | `LOGICAL_ERROR` → **`ABORTED`** | was yes | **downgraded** |
| different uuid | `foreign_writer` | `LOGICAL_ERROR` | yes | unchanged (deliberate) |
| absent | `vanished` (part 1a) | `FILE_DOESNT_EXIST` | no | unchanged |

The trailing `SingleWriterSlot::onRenewMismatch(mismatched_key)` call is now **unreachable** for
`MountLeaseKeeper` (the five cases above are exhaustive) and is removed from this override; the
base class's generic loud throw remains for any other slot subclass.

**New branch semantics.** Name and message say what is KNOWN, not what is guessed (v1 review
finding 3/7): the slot advanced past our held token under our own (uuid, epoch) — cause uncertain.
The most likely cause after layer 1 lands is "layer-1 resolve itself failed" (GET also failed);
before layer 1 lands it is the ambiguous-timeout self-race. Both recover identically: throw
`ABORTED` (non-aborting, no retry semantics attached — the renewal loop is stopping either way) →
`backgroundLoop`'s confirmed-mismatch path → `onRenewFailed` → `on_lost` → write fence latches →
self-remount re-claims with a FRESH durable epoch (the proven path: `CasPool.cpp:950-1050`).
Forensics ride in the message and the mount event: observed vs local `seq`, observed
`pid`/`hostname`/`started_at_ms` vs ours (`MountLease` carries them —
`CasServerRootFormats.h:47`) — an operator can tell "seq exactly ours+1, same pid" (almost
certainly the self-race) from anything else, without the code pretending to know.

**Why `foreign_writer` keeps `LOGICAL_ERROR`.** A foreign *uuid* on our mount slot cannot arise
from any modeled protocol interleaving: the owner anchor (`claimOwnerOrThrow`) fails a foreign
claim closed at open, before any mount write. Reaching it implies owner-anchor bypass or
corruption — a genuine invariant violation, which is exactly what `LOGICAL_ERROR` is for. (The
same-uuid cases, by contrast, are all reachable by timeouts/suspensions of *ourselves*.)

**Accounting.** `ProfileEvents::CasMountLeaseLost` increments in both changed branches (it already
does in `superseded`; the new branch adds it) — the incarnation IS deliberately abandoned. Its
description's exhaustive cause list (`ProfileEvents.cpp:886`) gains the new cause. The mount event
`outcome` field distinguishes the branches (`same_epoch_state_uncertain` vs `superseded`), so a
dedicated new ProfileEvent is not needed (v1 review finding 7/7).

### Test repartition (mandated by v1 review finding 3/7 and 5/7) {#layer-2-tests}

`CasHeartbeat.ForeignTouchMakesRenewThrow` (`gtest_cas_heartbeat.cpp:120`) today builds a
same-uuid/same-epoch/`seq=99` body and `EXPECT_DEATH`s. That scenario is precisely the new
uncertain branch — the test is REPARTITIONED, not deleted:

- same-uuid/same-epoch/unfenced body (the existing construction) → `EXPECT_THROW` with code
  `ABORTED`, **no death**, and the fence must latch (assert via the keeper's fence probe);
- a NEW genuinely-foreign variant (different uuid) → keeps `EXPECT_DEATH` (the `foreign_writer`
  branch is intentionally still loud);
- a NEW same-uuid/different-epoch variant → `EXPECT_THROW(ABORTED)`, no death (`superseded`
  downgrade pinned).

## Layer 3 — `allocateWriterEpoch` refuses to re-mint over a live mount {#layer-3}

### Change

In `allocateWriterEpoch` (`CasServerRoot.cpp:151-190`), the absent-epoch-object branch currently
mints `next_writer_epoch = 1` whenever `serverRootSubtreeEmpty` passes. Add one GET of the mount
key (`l.mountKey(srid)`) to that branch: if a mount object EXISTS (any content), throw
`CORRUPTED_DATA` — "epoch object missing but a mount lease exists over this server-root: durable
epoch state was lost while a mount is live or recently live; refusing to re-mint epoch 1. Recover
by removing the stale mount object only after verifying no server is live on this root."

This closes the exact hole from v1 review finding 1/7: without it, a second same-uuid process can
be handed epoch 1 while the first incarnation holds (1, live-mount), making the same-(uuid, epoch)
pair genuinely two processes. With the guard, epoch re-mint requires the mount object to be gone
too — and then the surviving first process fails safe anyway: its next renewal finds the slot
vanished or foreign-token and takes the non-fatal vanished/uncertain path, fencing itself. The
residual double-wipe scenario (epoch AND mount both deleted under a live mount) therefore degrades
to fencing, never to two silent same-pair writers.

Cost: one GET, only on the absent-epoch path (bootstrap of a fresh root, or the pathological
wipe) — never on the steady-state path (epoch object present). Fresh-root bootstrap still works:
both objects absent → guard passes → CAS-mint as today (concurrent first-mounters still serialize
on the epoch CAS loop).

## Interactions and non-goals {#non-goals}

- `MountFencedException` handling at `keeperStart` (`CasPool.cpp:625`) is untouched; layers 1-2
  live entirely in the background-renewal path.
- The GC fence/ack-floor machinery is untouched (the `fenced_by_gc` branch is unchanged).
- No change to `claimMount`'s same-pair immediate adopt (v1 review noted it; with layer 3 the
  same-pair body at claim time once again implies "our own prior incarnation", which is what the
  adopt is for — a restart reclaiming its own slot).
- No change to lease budget constants; the layer-1 skew bound documents the inequality any future
  budget change must preserve.
- `system.content_addressed_log` events: the new branch emits the existing `MountConflict` event
  type with a new `outcome` string — no schema change.

## Verification plan (gates, in order) {#verification}

**Gate 0 — second adversarial codex review of THIS spec** (two-independent-consults convention for
hard concurrency changes). Implementation starts only after it passes.

**Gate 1 — TLA+ phase 0.** Extend `docs/superpowers/models/CaCasMountCore.tla` (its `Renew` action
currently has no keeper-held-token state and models same-epoch/unfenced token movement as a hard
failure — `CaCasMountCore.tla:356,467`):
- split "durable slot token" from "keeper-held token"; add an `AmbiguousRenew` action (the PUT
  lands, the keeper's held token does not advance);
- add the `Resolve` action (GET-compare: adopt-if-bytes-match / rethrow-if-token-match /
  conflict);
- add the uncertain-branch transition: confirmed mismatch under same (uuid, epoch, unfenced) →
  `localLost` + fresh-epoch remount (NOT a hard failure state);
- model the layer-3 guard: epoch re-mint is enabled only when both control objects are absent.
- **Invariants:** single-writer (no two live unfenced writers on one slot) holds across all new
  actions; liveness: an ambiguous renewal eventually reaches renewed-or-fenced (no wedge).
- **Negative controls (each must break its invariant):** (a) blind-adopt sabotage (resolve adopts
  the observed body without byte comparison) → single-writer violated in the two-process
  same-pair scenario; (b) removing the layer-3 guard → same-pair two-writer state reachable;
  (c) removing the uncertain-branch fence (continue writing) → single-writer violated.

**Gate 2 — gtests (TDD: each written failing-first against the current code where applicable).**
- New fault backend `ApplyThenThrowPutOverwriteFaultBackend`: **applies** the putOverwrite to the
  in-memory state, then throws a transient exception — the landed-but-unacked case the existing
  `TransientPutOverwriteFaultBackend` (`gtest_cas_heartbeat.cpp:419`, throws before applying)
  cannot model (v1 review finding 5/7).
- Layer 1: (a) apply-then-throw fault → `renewOnce` resolves, returns success, `seq`/token
  advanced, next beat renews cleanly (no mismatch ever surfaces); (b) apply-then-throw with the
  resolve GET ALSO faulted → original exception propagates (transient), next beat hits confirmed
  mismatch → uncertain branch → `ABORTED`, fence latched, no death; (c) throw-without-apply →
  behavior identical to today's transient path (loop survives while the deadline allows —
  reuse/extend `CasHeartbeat` loop tests).
- Layer 2: the three repartitioned/new classification tests (§layer-2-tests) plus a full-path
  test through a real `backgroundLoop`: fault-driven confirmed mismatch → `onRenewFailed` →
  `on_lost` fires (observe via the keeper's callback), no process death.
- Layer 3: epoch object deleted + mount object present → `allocateWriterEpoch` throws
  `CORRUPTED_DATA`; both absent → mints 1 (fresh-root bootstrap unbroken); epoch present → no GET
  of the mount key (assert via a counting backend, pinning the steady-state cost).
- Full existing suites: `CasHeartbeat.*`, `CasMountLease.*`, `gtest_cas_mount.cpp`,
  `gtest_cas_operation_gate.cpp` (the `Cas*`/`CA*` gate filter per project convention).

**Gate 3 — live validation.** The CAS-s3 stateless lane (where the crash was caught) on the fixed
build; optionally a ca-soak run with injected S3 timeouts on the mount-lease key.

## Implementation order {#order}

Phase A = layer 2 (pure classifier change; smallest, kills the abort class immediately — even
layer-1 residuals then fail safe). Phase B = layer 1 (removes the common cause). Phase C = layer 3
(closes the epoch hole; makes layer 2's uncertainty analysis airtight). Each phase lands with its
gtests; TLA+ phase 0 precedes all of them; the spec's second codex review precedes TLA+ work.
