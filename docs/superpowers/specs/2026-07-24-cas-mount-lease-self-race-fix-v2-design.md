# CAS mount-lease renewal ambiguity — three-layer fix (v2) — design

**Status:** DESIGN rev.2 (2026-07-24). rev.1 was reviewed by the second codex gpt-5.6-sol
adversarial pass (verdict **UNSAFE**, 7 CONFIRMED findings — verdict:
`tmp/codex_review_mountlease_v2_verdict.md`); rev.2 incorporates all seven. Layer 2 was endorsed
unchanged by that review. Pending: targeted third review of the rev.2 deltas (gate 0), then TLA+
phase 0, then implementation.
**Branch:** `cas-gc-rebuild` (PR #2073).
**Supersedes:** `2026-07-24-cas-mount-lease-self-race-fix-proposal.md` (v1) — refuted by the first
codex review (`tmp/codex_review_mountlease_selfrace_result.txt`).
**Origin:** SIGABRT under ASan in CI (Altinity PR #2073, run 30019911967, CAS-s3 stateless):
`LOGICAL_ERROR "CAS mount-lease: key ... was touched by a foreign writer"` from
`SingleWriterSlot::onRenewMismatch` (`CasServerRoot.cpp:1011`) via the `MountLeaseKeeper`
classifier fallthrough (`CasServerRoot.cpp:879`), ~10 s after a client-side `Timeout` on the same
renewal key. Third member of the STID 3982-3b48 family (parts 1a `8742d746d4e`, 1b `cafb64652d0`).

## Problem {#problem}

The mount-lease background renewal (`SingleWriterSlot::renewOnce`, `CasServerRoot.cpp:976-1008`)
issues ONE raw conditional PUT per beat. A client-side timeout leaves the outcome **permanently
unresolved**: the PUT may have landed server-side (bumping the slot's token/seq) with the ack
lost. The next beat then sends *different bytes* under the *stale* expected token, gets a
confirmed `PreconditionFailed`, and lands in `MountLeaseKeeper::onRenewMismatch` — whose re-read
shows a body with our own `server_uuid`, our own `writer_epoch`, not `gc_fenced`. No classifier
branch covers that shape, so it falls through to the base class's unconditional `LOGICAL_ERROR`,
which **aborts debug/ASan builds at exception construction**.

Facts established by the two reviews that shape this design:

1. **"Same uuid + same epoch + unfenced" does NOT prove "our own prior write"** (v1 refuted):
   `allocateWriterEpoch` re-mints epoch 1 over a root whose epoch object vanished while a mount is
   live (`serverRootSubtreeEmpty` checks only data subtrees, `CasServerRoot.cpp:72-84`), and
   `claimMount` adopts a same-pair live body immediately. The condition is **state uncertainty**.
2. **The `superseded` branch is also a normal fencing outcome** that still aborts; the TLA+ model
   treats it as `localLost` (`CaCasMountCore.tla:342`).
3. **A resolve step must not trust request latency, byte equality alone, the `get` bytes/token
   pair, or `get`-absence** (rev.1 refuted on all four — see the finding map below).

Three layers: **resolve the ambiguity at the source, safely bounded** (layer 1), **never abort on
the fail-closed classifications** (layer 2), **close the epoch-reuse hole with authoritative
evidence, without breaking decommission** (layer 3).

## Round-2 findings → rev.2 resolutions (traceability) {#finding-map}

| # | Round-2 finding (all CONFIRMED) | rev.2 resolution |
|---|---|---|
| 1 | Critical: delayed resolve can re-arm an old writer after a successor reclaimed (deadline refreshed from response time; `attempt_timeout_ms` is advisory, real S3 timeout 30 s) | **Prepare-time deadline anchoring** everywhere (§layer-1, D1): the fence deadline is computed from the monotonic instant *before* the PUT, threaded through both `refreshConfirmedDeadline` and the `CasMountRuntime` callback; a resolve-commit whose anchored deadline has already elapsed **refuses to re-arm** and takes the fence path instead. Latency can then never extend a lease beyond what the durable body itself authorizes. |
| 2 | Byte equality does not identify THIS attempt (`seq+1` reused across ambiguous beats; `now_ms` non-monotonic, can freeze) | **Per-beat nonce** in the lease body (§layer-1, D2): `renewOnce` mints a random 64-bit nonce per attempt; body equality is then attempt-unique by construction. Pre-release format change (no compat scaffolding per project policy). |
| 3 | Native `get` returns an incoherent `(bytes_newer, token_older)` pair (HEAD-then-GET, documented at `CasObjectStorageBackend.cpp:613`); content-ETag ABA breaks "token equal ⇒ never applied" | **Coherence sandwich** for the resolve read (§layer-1, D3): HEAD-GET-HEAD; the pair is trusted only when both HEADs return the same token, else the resolve is `Indeterminate` → rethrow (conservative). "token == last_token" is downgraded from "provably never applied" to "no commit evidence → rethrow" — it is never used to justify anything unsafe (the retry it permits is the same If-Match retry the protocol already performs). |
| 4 | Layer 3 gated on `get`-absence, which is non-authoritative (transport faults flatten to "not found") | Layer 3 uses **`probeSentinelRaw`/`ProbeOutcome`** (`CasBackend.h:132+`): mint is allowed only on authoritative `KeyAbsent`; `ContainerAbsent`/`AccessDenied`/`Indeterminate` fail closed (§layer-3). |
| 5 | The TLA+ model cannot express the needed scenarios (actor ≡ uuid; no epoch-presence bit, pending request, stale response, held token; no fairness) | Gate 1 scope expanded accordingly (§verification): process identity separated from uuid, epoch-presence modeled, request/response split with stale delivery, fairness assumptions stated. |
| 6 | Layer 3 breaks `openForDecommission`'s supported recovery (derives victim uuid from a surviving mount, then allocates an epoch) | `allocateWriterEpoch` gains an explicit **policy parameter** (§layer-3): `NormalMount` enforces the guard; `DecommissionRecovery` (used only by `openForDecommission`) bypasses it — decommission is operator-driven, impersonates the victim deliberately, and retires the slot afterwards. Pinned by a test. |
| 7 | Gate 2 as written was untestable (direct `renewOnce` throw never latches the fence) and missed the critical race | Gate 2 rewritten (§verification): fence assertions run through the real `backgroundLoop` (or an explicit seam), plus a **barrier-controlled stale-resolve race test**, a mixed HEAD/GET-token test, a frozen-clock/identical-body test, and the decommission test. |

Round 2 also **confirmed sound**: layer 2's exhaustiveness (only `su`/`we` are required by
`decodeMountLease`, everything else defaults — `CasServerRootFormats.cpp:138`), the `ABORTED`
downgrade flow, the `foreign_writer` justification (decommission impersonates the victim uuid, it
never manufactures a foreign one), removing the trailing base call, and the absence of a
double-keeper race (renewal and `on_lost` are sequential on one thread; remount stops/joins the
old keeper first — `CasPool.cpp:997`).

## Layer 1 — bounded resolve-on-ambiguity in `SingleWriterSlot::renewOnce` {#layer-1}

### D1. Prepare-time deadline anchoring (independent hardening; prerequisite)

Today `refreshConfirmedDeadline` (`CasServerRoot.cpp:661`) and the `CasMountRuntime` renew-ok
callback (`CasMountRuntime.cpp:81,240`) both compute `now + TTL` at **response** time, while the
durable `expires_at_ms` was stamped at **prepare** time — the local fence deadline can exceed the
durable authorization by the full request latency. This skew exists today (a slow successful PUT);
finding 1 shows it becomes exploitable once a resolve path can complete arbitrarily late.

Change: `renewOnce` captures a **monotonic** attempt-start instant before the PUT and hands it to
the success hooks; both deadline computations become `attempt_start + TTL`. The local deadline is
then always ≤ the durable one regardless of how long the PUT, the resolve, or a delayed response
took. This lands first and independently (it hardens the existing success path too).

### D2. Per-beat nonce

`MountLease` gains a `nonce` field (random 64-bit, minted inside each `renewOnce` call, encoded in
the body; decode defaults it to 0 like every other optional field). Body byte-equality then
identifies exactly one attempt — immune to frozen clocks, reused `seq + 1`, and identical
`min_active`. Pre-release format change; no compat path (project policy: no compat scaffolding
before first release). The nonce is diagnostic/comparison-only — no protocol decision reads it
except the resolve's byte comparison (which compares whole bodies anyway).

### D3. The resolve

On **any exception** from the renewal PUT, `renewOnce` performs one **coherence-sandwich read**:
`head₁ → get → head₂` on the slot key (implemented as a small helper next to the resolve, or as a
`Backend` "coherent get" variant if review prefers; the sandwich needs no new backend
capability). The result is trusted only if `head₁.token == head₂.token` (no write interleaved the
read window; the bytes belong to that token). Then:

| Coherent observation | Action |
|---|---|
| `bytes == body` (this attempt's exact bytes, nonce-unique) **and** the D1-anchored deadline has NOT elapsed | commit: `recordWrite(seq + 1, token)`; return success (`onRenewSucceeded` refreshes the fence from the anchored instant) |
| `bytes == body` but the anchored deadline HAS elapsed | **refuse to re-arm** (finding 1): fall through to the confirmed-mismatch path — fence + self-remount. The durable slot does hold our write, but we can no longer prove liveness ownership within the lease bound; the successor-observation window may already be running |
| `token == last_token` | no commit evidence; rethrow the ORIGINAL exception (transient path, as today). NOT treated as proof of non-application (finding 3): the only thing this branch enables is the next beat's ordinary If-Match retry, which fails closed on any interleaved write |
| any other coherent body | genuine interposed write: set the confirmed-mismatch flag, call `onRenewMismatch(key)` (classifier decides on its own re-read) |
| absent | confirmed-mismatch flag + `onRenewMismatch` → its vanished branch (part 1a). `get`-absence is non-authoritative (finding 4) but the consequence here is conservative fencing, never a mint — acceptable and documented |
| sandwich incoherent (`head₁.token != head₂.token`) or any read error | `Indeterminate`: rethrow the ORIGINAL exception. If the write actually landed, the next beat's confirmed mismatch is handled non-fatally by layer 2 — defense in depth |

Unchanged: the `res.outcome != Done` path (a COMPLETED PUT reporting `PreconditionFailed` is
already unambiguous — no resolve); the `dead`/`seq == 0` guards stay outside the resolve wrapper;
`state_mutex` hold extends by the sandwich on the failure path only (same single-driver invariant,
`CasServerRoot.cpp:982-985`).

### Why the commit is now safe (the finding-1 trace, closed)

In the round-2 counterexample the old keeper re-armed because the deadline was computed at
response time. Under D1+D3 the commit deadline is anchored before the PUT: by the time a successor
may legally begin reclaiming (durable expiry + mandatory stale-token observation of
`TTL + 5% + renew_period/2` ≈ 36.5 s — `CasServerRoot.cpp:390`, `CasPool.cpp:514`), the
predecessor's anchored deadline (`attempt_start + TTL`, where `attempt_start` precedes the durable
stamp) has **necessarily already elapsed**, so a late resolve refuses to re-arm — structurally,
not by latency assumption. No constant-tuning is involved: the inequality is
`anchored deadline ≤ durable expiry < successor takeover`, both halves by construction.

## Layer 2 — non-aborting classification in `MountLeaseKeeper::onRenewMismatch` {#layer-2}

*(Endorsed by round 2 as specified in rev.1; restated for completeness.)*

| Observed body | Branch | Exception (code) | Aborts? | Change |
|---|---|---|---|---|
| same uuid, `gc_fenced` (any epoch) | `fenced_by_gc` | `MountFencedException` | no | unchanged |
| same uuid, same epoch, unfenced | **NEW `same_epoch_state_uncertain`** | `ABORTED` | no | **added** |
| same uuid, different epoch, unfenced | `superseded` | `LOGICAL_ERROR` → **`ABORTED`** | was yes | **downgraded** |
| different uuid | `foreign_writer` | `LOGICAL_ERROR` | yes | unchanged (deliberate: the owner anchor makes it unreachable by protocol; decommission impersonates the victim uuid, never a foreign one) |
| absent | `vanished` (part 1a) | `FILE_DOESNT_EXIST` | no | unchanged |

The new branch's name and message state what is KNOWN (the slot advanced past our held token under
our own pair — cause uncertain), with forensics: observed vs local `seq`, observed
`pid`/`hostname`/`started_at_ms`/`nonce`. Recovery: `ABORTED` → `backgroundLoop` confirmed-mismatch
path → `onRenewFailed` → `on_lost` → fence → self-remount with a fresh durable epoch. The trailing
`SingleWriterSlot::onRenewMismatch` call is removed from this override (the five cases are
exhaustive; round 2 confirmed nothing relies on reaching the base). `CasMountLeaseLost` increments
in both changed branches; its exhaustive-cause description (`ProfileEvents.cpp:886`) gains the new
cause. No new ProfileEvent (the mount event `outcome` distinguishes the branches).

Test repartition (`gtest_cas_heartbeat.cpp:120` `ForeignTouchMakesRenewThrow`): the existing
same-uuid/same-epoch/`seq=99` construction → `EXPECT_THROW(ABORTED)`, no death; a NEW
different-uuid variant keeps `EXPECT_DEATH`; a NEW same-uuid/different-epoch variant →
`EXPECT_THROW(ABORTED)`. Fence-latch assertions run via the real `backgroundLoop` (finding 7 —
a direct `renewOnce` throw does not call `onRenewFailed`), or via an explicit test seam if loop
timing proves flaky.

## Layer 3 — epoch re-mint guard with authoritative evidence, decommission-aware {#layer-3}

`allocateWriterEpoch` (`CasServerRoot.cpp:151-190`) gains a policy parameter:

- **`NormalMount`** (default; `Pool::mountWritable` startup and self-remount): on the
  absent-epoch-object branch, probe the mount key with **`probeSentinelRaw`** (finding 4):
  - `KeyAbsent` (authoritative) → proceed to mint (fresh-root bootstrap; concurrent bootstrappers
    still serialize on the epoch CAS loop);
  - `Present` → throw `CORRUPTED_DATA` — "epoch object missing but a mount lease exists over this
    server-root: durable epoch state was lost while a mount is live or recently live; refusing to
    re-mint epoch 1. If no server is live on this root, decommission it or manually remove the
    stale mount object." (wording consistent with the existing recovery advice at
    `CasServerRoot.cpp:370`);
  - `ContainerAbsent` / `AccessDenied` / `Indeterminate` → fail closed with the probe outcome named
    (absence was never proven).
- **`DecommissionRecovery`** (only `openForDecommission`, `CasPool.cpp:700→735`): guard bypassed.
  Decommission exists to reclaim a dead root, is operator-invoked, derives the victim uuid FROM
  the surviving mount, and retires the slot as its job — the guard's target (accidental same-pair
  revival by a normally-restarting server) does not apply. Pinned by a dedicated test (finding 6).

Residual hole, honestly stated: if epoch AND mount are both wiped under a live mount, the guard
passes and a second same-pair process can exist — but the first process then fails safe on its
next renewal (vanished/uncertain branch → fence), degrading to a remount, never to two silent
same-pair writers. Race window (mount object appears between the probe and the epoch CAS): the
newcomer that created that mount holds a *different* epoch allocation path (it allocated first —
order in `mountWritable` is epoch-then-mount), so the CAS loop's conflict retry re-observes; a
truly concurrent fresh bootstrap pair is serialized by the epoch CAS as today.

Steady-state cost: zero (the probe runs only when the epoch object is absent).

## Interactions and non-goals {#non-goals}

- `MountFencedException` at `keeperStart` (`CasPool.cpp:625`), the GC fence machinery, and
  `claimMount`'s same-pair adopt-on-open are untouched (with layer 3, a same-pair body at claim
  time again means "our own prior incarnation" — a restart reclaiming its own slot).
- The farewell PUT (`terminate`) stays out of scope: a lost farewell leaves the lease to expire by
  TTL (part 1b covers absent-at-release); the server is shutting down either way.
- `min_active` exposure on the rethrow path is unchanged from today's ambiguous window (values are
  re-published every beat; GC reads them at TTL scale).
- No lease-budget constant changes; D1 removes the constants from the safety argument entirely.
- `system.content_addressed_log`: the new branch reuses `MountConflict` with a new `outcome`
  string — no schema change.

## Verification plan (gates, in order) {#verification}

**Gate 0 — targeted third codex review** of the rev.2 deltas (D1/D2/D3, layer-3 probe+policy, the
finding map). Implementation starts only after it passes.

**Gate 1 — TLA+ phase 0** (scope per finding 5, superseding rev.1's sketch). Extend or fork
`CaCasMountCore.tla`:
- separate **process identity from uuid** (two processes may share (uuid, epoch));
- model the **epoch object's presence** as state (absent/present) with a wipe action;
- split PUT into request/response with **ambiguous landed writes and arbitrarily delayed
  responses** (a resolve read may deliver a stale snapshot);
- model the keeper-held token distinct from the durable slot token; model the **anchored
  deadline** as taken at request time;
- add the resolve action (sandwich-coherent read; commit gated on the anchored deadline), the
  uncertain-branch fence (`localLost`, not a failure state), and the layer-3 guard (mint only on
  authoritative absence);
- state **fairness/bounded-response assumptions explicitly** so the liveness gate ("an ambiguous
  renewal eventually reaches renewed-or-fenced") is meaningful.
- **Invariants:** single-writer (no two live unfenced writers per slot) across all new actions —
  including the two-process same-pair scenario and the delayed-resolve trace from finding 1.
- **Negative controls (each must break its invariant):** (a) resolve commits without the anchored
  deadline gate → finding-1 trace violates single-writer; (b) blind adopt (no byte compare) →
  single-writer violated; (c) layer-3 guard removed + epoch wipe → same-pair two-writer state
  reachable; (d) uncertain branch continues writing instead of fencing → single-writer violated.

**Gate 2 — gtests (TDD; failing-first where the current code exhibits the bug).**
- New fault backend `ApplyThenThrowPutOverwriteFaultBackend` (applies, then throws — the existing
  `TransientPutOverwriteFaultBackend` at `gtest_cas_heartbeat.cpp:419` throws before applying and
  cannot model the bug).
- **The finding-1 race, explicitly** (finding 7): a barrier-controlled resolve whose sandwich read
  snapshots the old body, then a successor reclaims (fresh epoch), then the stale resolve result
  is delivered — assert the predecessor's fence stays closed (anchored deadline elapsed → no
  re-arm), no death, and the successor's writes are unaffected.
- Layer 1 happy path: apply-then-throw → resolve commits, `seq`/token advance, next beat renews
  cleanly; resolve GET also faulted → original exception propagates, next beat hits the uncertain
  branch → `ABORTED`, fence latches (via real `backgroundLoop`), remount recovers with a fresh
  epoch, no death.
- Mixed HEAD/GET token test: sandwich detects the interleaved write (`head₁ ≠ head₂`) →
  `Indeterminate` → rethrow (no commit on an incoherent pair).
- Frozen-clock / identical-body test: two ambiguous beats with a frozen `now_ms_fn` — the nonce
  still distinguishes them (no cross-beat false commit).
- Layer 2: the three repartitioned/new classification tests + the full-loop fence-latch test.
- Layer 3: epoch absent + mount `Present` → `CORRUPTED_DATA`; epoch absent + authoritative
  `KeyAbsent` → mints 1; probe `Indeterminate` → fail closed; `DecommissionRecovery` policy +
  surviving mount + missing epoch → decommission proceeds (finding 6); epoch present → no probe
  issued (counting backend pins the steady-state cost).
- Full existing suites: `CasHeartbeat.*`, `CasMountLease.*`, `gtest_cas_mount.cpp`,
  `gtest_cas_operation_gate.cpp`, decommission suites (`gtest_cas_decommission.cpp`).

**Gate 3 — live validation.** CAS-s3 stateless lane on the fixed build; optionally ca-soak with
injected S3 timeouts on the mount-lease key.

## Implementation order {#order}

Phase A = layer 2 (endorsed, smallest, kills the abort class immediately; even unresolved
ambiguities then fail safe). Phase B = D1 (deadline anchoring — independent hardening of the
existing success path). Phase C = D2+D3 (nonce + bounded resolve). Phase D = layer 3
(probe-gated guard + decommission policy). Each phase lands with its gtests; TLA+ phase 0
precedes B-D (A is classifier-only and TLA-neutral: it maps an existing abort to the modeled
`localLost` transition); the rev.2 targeted review precedes everything.
