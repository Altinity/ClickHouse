# TLA+ gate report — rev.7 erasure-proof soundness + disk lifecycle/FORGET protocol

Date: 2026-07-23. Models: `docs/superpowers/models/CaErasureProof.tla`,
`docs/superpowers/models/CaDiskLifecycle.tla` (+ 12 configs, runners `run_erasureproof.sh` /
`run_disklifecycle.sh`). TLC 2.19 (`tmp/tla2tools.jar`), `-workers auto`. Raw logs:
`tmp/tlc_rev7_proof_*.log`, `tmp/tlc_rev7_lifecycle_*.log`.

Modeled against: spec `docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md`
§§1-6 and the as-built sources at HEAD (`CasPool.cpp` step-0 gate / `evaluateErasureProofEmptySample` /
`forgetDisk`, `CasMountRuntime.{h,cpp}`, `CasGcScheduler.cpp`, `CasGc.cpp:2405-2468`,
`ContentAddressedMetadataStorage.cpp` forgetDisk/gcStop/gcStart).

## Verdict summary {#verdict}

| Model | One-line verdict |
|---|---|
| `CaDiskLifecycle` (FORGET + lifecycle) | **INVARIANTS HOLD** (I1, I1b, I2, I3, I4a, I4b, I6 + the `ForgetCompletes` liveness); both sabotages RED as required; all three witnesses reachable. |
| `CaErasureProof` (the crown property) | **Writer paths HOLD; grace PROVEN load-bearing; TRACE FOUND on the GC side** — two real (pre-activation) windows where a GC control write lands under the prefix at/after `VanishedErased` promotion; the `Live`-gate fix candidate is mechanically validated. |

**Task 15 gate recommendation: OPEN, with one tracked precondition.** The protocol Task 15 locks in
(the lifecycle state machine, the FORGET verb, the operation-gate semantics) is machine-checked
clean, including the two subtle races argued in prose during review (the join-window reclaim /
trip#2, and the FORGET-vs-natural-promotion race). The GC-side erasure-proof findings do NOT block
Task 15: the natural `Vanished(erased)` promotion is **dormant in production** (Task 6 deviation —
`supportsErasureProof()` is false everywhere until `setStrongPrefixListCapable(true)` is wired).
They MUST be fixed before that capability ships anywhere; the fix direction is validated below.

## Model 1 — `CaErasureProof` {#model1}

Crown invariant `TruthEmpty`: `pool_lifecycle = VanishedErased => LIST(pool prefix) = {}` — at the
promotion instant and forever after (a later land while promoted also violates it). Writers are
modeled with the honest TOCTOU (admission captures the fence generation and takes the op-scoped
guard; the `checkFenceOrThrow` recheck precedes the durable land by arbitrarily many steps; a
timeout can release the guard while the issued request is still in flight = a **zombie**). The
observer's sample is split into its two non-atomic halves in code order: the gate's authoritative
empty LIST, then the qualification reads (`outstanding_durable_requests == 0`, `gc_quiescent_fn`,
grace) and the streak/promote — everything else interleaves between the halves.

| Config | Semantics | Result | States (distinct) / depth |
|---|---|---|---|
| `_nogc_grace` | writers only, grace ON | **HOLDS** | 56,382 / 25 |
| `_sab_nograce` | writers only, grace OFF | **VIOLATED** (expected) | 17,609 / 14 (trace len 12) |
| `_gc_promptliteral` | GC per the Task-8 prose: hb only inside rounds, no round after intent | **VIOLATED — real finding F2** | 27,928 / 14 (trace len 12) |
| `_gc_asbuilt` | + out-of-round `heartbeatLoop`, no scheduler lifecycle exit | **VIOLATED — real finding F3** | 15,077 / 12 (trace len 11) |
| `_fix_gclivegate` | fix candidate: rounds AND hb pulses require lifecycle `Live` | **HOLDS** | 476,250 / 31 |
| `_witness_promote` | non-vacuity: promotion reachable | reachable (good) | 2,083 / 11 |

### The grace question ([D1]) — ANSWER: grace IS load-bearing {#grace-answer}

Counter + LIST-reset + `round_in_flight` alone do NOT close the model. The `_sab_nograce`
counterexample (12 steps): `w1` admits while `Live` (guard held, generation captured), passes its
final recheck and issues; the keeper trips; sample 1 cannot count while the guard is held — but the
op **abandons on timeout** (guard released, request still in flight); sample 1 counts; the second
LIST sees empty; **the zombie request lands after the LIST**; the qualification reads
`counter == 0` (the guard is long gone) and promotes — `VanishedErased` with `w1`'s object durable
under the prefix. This is precisely the "guard released but the backend write is still propagating"
residual window the [D1] grace exists for; with `GraceOn` (modeled per its intent: no write
admitted before the last trip remains unresolved, zombies included) the qualification refuses until
the zombie is gone and the invariant holds. Note the load-bearing subtlety: the grace covers
zombies because **all issues happen strictly before the last trip** — admission is op-gated to
`Live` and a recheck needs an unchanged generation. If any durable path ever admits outside `Live`,
the grace anchor (`fence_trip_boot_ms`, re-stamped only by a winning `Live -> TransientNotLive`
CAS) no longer bounds it — keep admission `Live`-only.

### Finding F2 (real, narrow): the completed-round window defeats `round_in_flight`-at-sample {#f2}

`_gc_promptliteral` checks exactly the Task-8 prose containment ((a) LIST covers `gc/state`/hb keys,
(b) `round_in_flight` held for the whole round closes the landed-just-after-LIST window, (c) the
`has_observation` guard refuses to recreate an absent `gc/state`) — and TLC refutes the combination
mechanically (12-step trace): full erase; two clean samples begin; after the observer's **final
LIST**, a **fresh scheduler** (created at mount, never observed any `gc/state` — `has_observation`
unlatched, so (c) does not apply; this is the normal first-acquire path of a new pool,
`CasGc.cpp:2436`) starts a round, **CREATES `gc/state`** under the prefix, and the round
**completes**; only then does the observer read `round_in_flight == FALSE` — (b) only defends while
the round is *still* in flight at the read — and promotes with `gc/state` durable under the prefix.
In real time the window is the gap between the gate's prefix-LIST response and the
`gc_quiescent_fn` read (atomic loads — but stretchable arbitrarily by descheduling), and an
erased-empty pool makes rounds short. Consequences of a false promotion: the disk answers
truth-empty while control keys exist, and the [C4] restart bootstrap then **fails loud on the
non-empty prefix** — the erase-then-recreate recovery path wedges on GC debris.

### Finding F3 (real, wider): out-of-round heartbeat pulses + no scheduler lifecycle exit {#f3}

`_gc_asbuilt` adds the two as-built facts the prose did not cover: (i) `heartbeatLoop` pulses
`gc/hb` whenever `i_am_leader`, **outside any round** (`CasGcScheduler.cpp:280-305`), and
`pulseHeartbeat` **creates the key if absent** (expected-token `nullopt`, `CasGc.cpp:2405-2419`);
(ii) nothing in `loop()`/`heartbeatLoop()` consults the pool lifecycle — the scheduler does NOT
exit on a natural terminal transition (spec §3 "GC scheduler exits at its next tick" is implemented
only for FORGET/`GC STOP`/shutdown, where `stop()` is called). TLC's shortest trace is the
post-promotion variant: promotion completes cleanly, then a round starts on the `VanishedErased`
pool and recreates `gc/state`; deeper traces land an hb pulse inside the LIST-to-qualification
window (`round_in_flight` never protects hb pulses, and `i_am_leader` stays true until a *failed*
round clears it — pulses continue through the whole observation on a pool that was GC leader before
the erase). Same consequences as F2 plus a permanent one: a `VanishedErased` disk keeps re-creating
control keys forever ("never writes again" broken), and each recreation would also have reset a
still-running proof — a liveness drag on the natural path even when no false promotion occurs.

### Fix direction (validated, not implemented) {#fix}

`_fix_gclivegate` = scheduled rounds AND heartbeat pulses refuse unless the pool lifecycle is
`Live` — the exact refusal the manual GC entry points already make via
`checkOpAdmitted(CasOpClass::Admin)`. With it, the full model (writers + GC + eraser + restore +
keeper + reclaim) HOLDS at 476k distinct states. This also cures the F3 leader-latch (no pulses
once not `Live`) and the post-promotion debris. Implementation shape: a lifecycle check at the top
of `CasGcScheduler::loop()`'s tick and in `heartbeatLoop()`'s `i_am_leader` branch (or clearing
`i_am_leader` on a non-`Live` lifecycle observation). NOT implemented here — per the task contract,
production code is untouched; the controller routes the fix.

## Model 2 — `CaDiskLifecycle` {#model2}

The lifecycle state machine + the as-built FORGET step order (`publishIntent; trip1;
gcStop(signal,join); remountStop(latch,join); trip2; drain; finishTeardown(drained ? farewell :
no-marker); keeperReset; enterVanished(Forgotten)`), concurrent with: keeper trips, the remount
thread whose **in-flight attempt does not see a freshly published intent** (checked at step
boundaries only — so its reclaim can complete inside FORGET's join window), natural terminal
promotions (nondeterministic outcomes; soundness delegated to Model 1), the GC scheduler loop, and
the `GC STOP`/`GC START` verbs under `lifecycle_mutex` (with START's Admin-gate `Live` refusal).

| Config | Checks | Result | States (distinct) / depth |
|---|---|---|---|
| `_main` | TypeOK, I1, I1b, I2, I3, I4a, I4b, I6 + `ForgetCompletes` (liveness, fairness) | **ALL HOLD** | 384 / 21 |
| `_sab_notrip2` | trip#2 removed | **I1 VIOLATED** (expected) | 386 / 21 (trace len 18) |
| `_sab_unearnedfarewell` | drain gate removed | **I2 VIOLATED** (expected) | 372 / 22 |
| `_witness_forgetdone` | FORGET completion reachable | reachable (good) | 384 / 21 |
| `_witness_racederased` | FORGET done with a raced natural `VanishedErased` | reachable (good) | 384 / 21 |
| `_witness_joinwindowreclaim` | reclaim completed inside the join window | reachable (good) | 384 / 22 |

Per-invariant meaning and result (all in `_main` unless noted):

- **I1 `ForgetTerminal`** (`forget done => fence latched AND state in Vanished*`): HOLDS under all
  interleavings including the join-window reclaim. The `_sab_notrip2` RED trace is the exact
  Task-10 race, machine-checked: attempt begins pre-intent; FORGET publishes intent + trip#1; the
  in-flight attempt's **reclaim completes** (`lost = FALSE`, pool back to `Live`); joins land;
  without trip#2 FORGET finishes `VanishedForgotten` with `mayMutate() == TRUE`. Trip#2 is
  **necessary and sufficient** in the model (the T10 reviewer's residual-window minor is closed:
  after the join, no actor can re-arm — `RArm` is refused by intent + the shutdown latch, and the
  keeper never clears `lost`).
- **I1b `ForgetWinsUnlessNatural`**: HOLDS — the final state is `VanishedForgotten` unless a
  natural terminal transition won `enterVanished` first; `_witness_racederased` proves the race is
  real and the first-terminal-wins latch handles it (spec's own design intent, not a bug: the
  prompt's strict "state = VanishedForgotten" reading is refined accordingly).
- **I2 `EarnedFarewell`** (farewell only after a provable drain): HOLDS; sabotage RED.
- **I3 `OneWay`** (`IdentityLost`/`Vanished*` never followed by `Live`/`Transient`;
  `IdentityLost -> Vanished*` allowed; `Vanished*` absorbing): HOLDS.
- **I4a/I4b** (`Transient`/`IdentityLost` and every `Vanished*` imply a latched fence — the
  benign-answer gate never coexists with write authority): HOLD.
- **I6 `GcStoppedAfterForget`**: HOLDS, including the idempotent short-circuit path (the detached
  scheduler still `stop()`s via its local's destructor) and `GC START` post-FORGET (Admin gate
  refuses on a non-`Live` pool).
- **`ForgetCompletes`** (liveness under WF on the FORGET steps, thread-exit path, attempt
  resolution, round completion): HOLDS — no interleaving (in-flight attempt, in-flight round,
  racing natural promotion = the prompt's I5) wedges FORGET; both racing `enterVanished` calls
  resolve via the idempotent `terminal_state_published` latch with no stuck half-terminal state.

## Model-fidelity caveats (what the abstraction cannot see) {#caveats}

1. **S3/LIST semantics are assumed perfect**: the model's LIST reads the true key set atomically.
   Strong-LIST consistency ([C3]) is an *assumption*, not a checked property — eventual-consistency
   anomalies (a LIST missing a durably landed object) are exactly what the capability gate excludes
   by contract, and nothing here validates RustFS or any gateway. SDK error mapping
   (`ProbeResult`), IAM permutations, and the container-vs-key probe distinction are likewise
   below the abstraction.
2. **Grace is modeled by intent, not by clocks**: "no write admitted before the last trip remains
   unresolved (zombies included)" replaces the wall-clock arithmetic of `erasureProofGraceMs`. The
   model therefore checks the *discipline*, not the numeric bound; if the real backend can hold a
   request longer than `op_wallclock_bound + attempt_timeout` (e.g. a proxy replaying a captured
   PUT arbitrarily late), the code's grace is unsound in a way this model cannot see. Sample
   spacing is one observer-clock tick, per the same reasoning.
3. **Qualification atomicity**: the counter/gc/grace reads plus streak/promote are ONE action here;
   in code they are sequential loads. This is sound only because admission is `Live`-gated (nothing
   can join the counter mid-qualification on a non-`Live` pool) — flagged in the grace answer; the
   LIST half is honestly split because that is where the real windows live (F2/F3 depend on it).
4. **Reclaim is one atomic action** (arm + `noteRemounted`): the real
   `armMountFence .. noteRemounted` window (fence re-armed, state still `TransientNotLive`) is
   collapsed, justified because the op gate refuses admission while not `Live` and every older
   generation fails its recheck — the window is unobservable to durable writers. If a durable path
   ever bypasses the op gate, this collapse hides the arm-window hazard (same caveat as the grace
   anchor).
5. **GC rounds are two writes** (`gc/state` create/renew + optional hb): fold outputs, snapshots
   and manifest writes of a real round are folded into the same abstract "control key lands under
   the prefix" — sufficient for `TruthEmpty` (any key violates), coarse for anything else. A round
   completing "successfully" after the eraser deleted `gc/state` mid-round is permitted by
   `GcRoundEnd` where the code would fail the round — a harmless superset on the hb side (leader
   latch persists either way until a failed round clears it).
6. **Scope**: `~Pool` teardown on the natural path, manual `GC RUN` rounds (gated `Live`, and
   blocked during FORGET by `gc_scheduler_mutex` — both modeled indirectly), a second concurrent
   FORGET (the verb re-runs are serialized by `lifecycle_mutex`; only one instance is modeled, its
   idempotent short-circuit is), keeper single-shot terminate details, and backup restores racing
   the *inside* of one sample window (scoped out as [A1] erase-and-recreate out-of-contract;
   restores between ticks ARE modeled) are outside the models.

## Recommendation recap {#recap}

1. **Task 15: GO.** The lifecycle/FORGET protocol it locks in is machine-checked (Model 2 fully
   green with red sabotages), and Model 1 confirms the erasure-proof's writer-side machinery plus
   the necessity of every disputed ingredient (guard counter, LIST-reset, spacing, grace, trip#2).
2. **Before wiring `setStrongPrefixListCapable(true)` anywhere** (the switch that activates natural
   `Vanished(erased)`): land the `Live`-gate on scheduled GC rounds AND heartbeat pulses
   (mechanically validated by `_fix_gclivegate`), covering F2/F3. Also honest-up the spec §2/§3
   wording: "the `has_observation` guard refuses to recreate an absent `gc/state`" holds only for a
   scheduler that has observed one; "GC scheduler exits at its next tick" is today true only for
   the verb-driven stops.
3. Keep durable-effect admission strictly `Live`-gated — two independent soundness arguments (the
   grace anchor, the reclaim-window collapse) lean on it; a future path admitting outside `Live`
   invalidates both.
