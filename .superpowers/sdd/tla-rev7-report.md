# TLA+ gate report — rev.8 (FORGET-only v1) disk lifecycle/FORGET protocol

Date: 2026-07-23 (reworked the same day for the v1 scope change: the natural `Vanished(erased)`
proof stack is excised from the code; the erasure-proof model is retained as HISTORICAL evidence —
see the last section). Model: `docs/superpowers/models/CaDiskLifecycle.tla` (+ 7 configs, runner
`run_disklifecycle.sh`). TLC 2.19 (`tmp/tla2tools.jar`), `-workers auto`. Raw logs:
`tmp/tlc_rev7_lifecycle_*.log` (and `tmp/tlc_rev7_proof_*.log` for the historical model).

Modeled against: spec `docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md`
§§1/3/5/6 (as amended through commit `6c4c05c9802`) and the as-built sources at HEAD
(`CasPool.cpp` `forgetDisk`/`tryRemountOnce`, `CasMountRuntime.{h,cpp}`, `CasGcScheduler.cpp`,
`ContentAddressedMetadataStorage.cpp` forgetDisk/gcStop/gcStart), plus two queued hardenings
modeled as landed: **[C1]** `loop()`/`heartbeatLoop()` check `isVanished()` at each wake and
self-exit (no new round once `Vanished`); **[M1]** the remount attempt's step-0 bail checks
`vanished_intent` as well as `isVanished()`.

## Verdict {#verdict}

**`CaDiskLifecycle`: ALL INVARIANTS + BOTH LIVENESS PROPERTIES HOLD.** All three sabotages RED as
required (the model has teeth), all three witnesses reachable (the checks are not vacuous).

**Task 15 gate recommendation: OPEN.** The v1 protocol Task 15 locks in — the lifecycle state
machine (`Live`/`Transient`/`IdentityLost`/`VanishedReplaced`/`VanishedForgotten`), the FORGET
verb, the GC stop/start serialization — is machine-checked clean under every interleaving with the
keeper, the self-remount thread, a racing natural `Replaced` promotion, and the GC scheduler,
**conditional on [C1] and [M1] landing as queued** (both are modeled as present; `_sab_nogcselfexit`
shows exactly what breaks without [C1]).

## Model and scope {#model}

v1 state machine: the observer never promotes to `VanishedErased` (the state does not exist);
`IdentityLost` is entered directly on authoritative sentinel absence (no prefix-emptiness leg);
`Vanished` = `Replaced` (identity gate: `Present` + foreign `pool_id`, reachable from `Transient`
AND `IdentityLost`) | `Forgotten` (the verb). FORGET is modeled as the exact as-built step order:
mutex-acquire; idempotent `isVanished` short-circuit (the detached scheduler still `stop()`s via
its local's destructor); publish intent; trip#1; GC stop (signal + join waiting out an in-flight
round); remount stop (shutdown latch + join); trip#2; drain (nondeterministic outcome);
`finishTeardown` (farewell ONLY if drained); `keeperReset`; `enterVanished(Forgotten)`
(first-terminal-wins latch); mutex-release. The remount attempt is honest about the race: the loop
condition and the [M1] step-0 bail stop any NEW attempt once the intent is published, but an
attempt already past its bail does not re-check the intent mid-flight — its reclaim can complete
inside FORGET's join window (that race is a witness, and trip#2 is its cure).

## Results {#results}

| Config | Checks | Result | States (distinct) / depth |
|---|---|---|---|
| `_main` | TypeOK, I1, I1b, I2, I3, I4a, I4b, I6 + `ForgetCompletes` + `GcExitsAfterVanished` (liveness, fairness) | **ALL HOLD** | 268 / 21 |
| `_sab_notrip2` | trip#2 removed | **I1 VIOLATED** (expected) | 272 / 21 (trace len 18) |
| `_sab_unearnedfarewell` | drain gate removed | **I2 VIOLATED** (expected) | 260 / 22 |
| `_sab_nogcselfexit` | pre-[C1] code: no GC self-exit on natural `Vanished` | **`GcExitsAfterVanished` VIOLATED** (expected) | 268 / 21 (lasso) |
| `_witness_forgetdone` | FORGET completion reachable | reachable (good) | 268 / 21 |
| `_witness_racedreplaced` | FORGET done with a raced natural `VanishedReplaced` | reachable (good) | 268 / 21 |
| `_witness_joinwindowreclaim` | reclaim completed inside the join window | reachable (good) | 268 / 22 |

Per-invariant meaning and result (all in `_main` unless noted):

- **I1 `ForgetTerminal`** (`forget done => fence latched AND state in Vanished`): HOLDS under all
  interleavings including the join-window reclaim. The `_sab_notrip2` RED trace is the exact
  Task-10 race, machine-checked: attempt begins pre-intent; FORGET publishes intent + trip#1; the
  in-flight attempt's **reclaim completes** (`lost = FALSE`, pool back to `Live`); joins land;
  without trip#2 FORGET finishes `VanishedForgotten` with `mayMutate() == TRUE`. Trip#2 is
  **necessary and sufficient** in the model: after the join, no actor can re-arm — `scheduleRemount`
  is refused by the intent latch and the shutdown latch, and the keeper never clears `lost`.
- **I1b `ForgetWinsUnlessNatural`**: HOLDS — the final state is `VanishedForgotten` unless a
  natural `Replaced` transition won `enterVanished` first; `_witness_racedreplaced` proves the race
  is real and the first-terminal-wins latch handles it (by design, per the `forgetDisk` comment).
  With [M1], the racing promotion can only come from an attempt already in flight at
  `publishVanishedIntent` — no new attempt begins after it.
- **I2 `EarnedFarewell`** (farewell only after a provable drain): HOLDS; sabotage RED.
- **I3 `OneWay`** (`IdentityLost`/`Vanished` never followed by `Live`/`Transient`;
  `IdentityLost -> VanishedReplaced/Forgotten` allowed; `Vanished` absorbing): HOLDS.
- **I4a/I4b** (`Transient`/`IdentityLost` and every `Vanished` state imply a latched fence — the
  benign-answer gate never coexists with write authority): HOLD.
- **I6 `GcStoppedAfterForget`**: HOLDS, including the idempotent short-circuit path (the detached
  scheduler still `stop()`s via its local's destructor) and `GC START` post-FORGET (the Admin gate
  refuses on a non-`Live` pool).
- **`ForgetCompletes`** (liveness under weak fairness on the FORGET steps, thread-exit path,
  attempt resolution, round completion): HOLDS — no interleaving (in-flight attempt, in-flight
  round, racing natural promotion = the I5 question) wedges FORGET; both racing `enterVanished`
  calls resolve via the idempotent `terminal_state_published` latch with no stuck half-terminal
  state.
- **`GcExitsAfterVanished`** (the [C1] fix): HOLDS — once the pool is `Vanished`, the scheduler
  eventually stops ticking (self-exit on the natural path, stop+destroy on the FORGET path).
  `_sab_nogcselfexit` (the pre-fix code) produces the exact bug lasso: natural `VanishedReplaced`
  with `gcsched = Running` stuttering forever. This makes [C1] a modeled **precondition** of the
  green verdict — it must land as queued.

## Model-fidelity caveats {#caveats}

1. **Blocking joins are enabled-when conditions**: `stop()` joining an in-flight round and
   `stopRemountThread` joining the thread are modeled as actions enabled when the join target is
   gone, with weak fairness supplying progress. Real-time bounds ("one step + one backend
   timeout") are outside TLA scope — the model proves the joins cannot deadlock or livelock, not
   how long they take.
2. **Reclaim is one atomic action** (arm + `noteRemounted`): the real
   `armMountFence .. noteRemounted` window (fence re-armed, state still `TransientNotLive`) is
   collapsed — justified because the op gate refuses durable admission while not `Live` and every
   older fence generation fails its recheck, making the window unobservable to durable writers. If
   a durable path ever bypasses the op gate, this collapse hides a hazard.
3. **The keeper's single-shot terminal op** (`doTerminate` throwing on a second call) is modeled
   only through `keeperReset` making later teardown find no keeper; `~Pool` itself is out of scope.
4. **Manual `GC RUN` rounds** are not a separate actor: they are `Live`-gated and blocked during
   FORGET by `gc_scheduler_mutex` (held by `forgetDisk` from entry), so the scheduler-loop rounds
   dominate them for every property here. A second concurrent FORGET is serialized by
   `lifecycle_mutex`; only one instance is modeled (its idempotent short-circuit is).
5. **The natural `Replaced` promotion is nondeterministic** — the model does not check WHEN the
   identity gate may conclude `Replaced` (that is the probe/verdict logic of
   `probePoolLifecycleGate`, covered by `gtest_cas_lifecycle_condition.cpp`), only that a
   promotion at ANY moment interleaves safely with FORGET.
6. **[C1]/[M1] are modeled as landed.** If either lands differently (e.g. the scheduler self-exit
   keyed on something other than `isVanished()` at wake, or the step-0 bail omitted), the model
   must be re-checked against the actual patch.

## Historical: the excised erasure-proof model (`CaErasureProof.tla`) {#historical-erasure}

Before the v1 scope decision, the natural `Vanished(erased)` promotion (spec §2 [C2][C3][D1]) was
modeled and TLC-checked with the observer's sample split into its two non-atomic halves (the
gate's LIST, then the qualification reads) exactly as the code ordered them. Verdict, preserved
for a possible v2 revival: the writer-side machinery was SOUND — op-gate `Live`-only admission +
the op-scoped `DurableRequestGuard` counter + the LIST/streak reset discipline held at 56k states,
and the [D1] grace was proven **load-bearing** (without it, a zombie request — guard released on
timeout while the PUT is still in flight — lands after the second empty sample and falsifies
"verified: pool prefix empty"). Two REAL GC-side windows were found: a fresh (never-observed)
scheduler's round CREATES `gc/state` between the observer's final LIST and its `round_in_flight`
read and completes before it (the `has_observation` guard protects nothing until latched), and
out-of-round `heartbeatLoop` pulses land `gc/hb` in the same window; the `Live`-gate fix direction
held at 476k states. These traces are part of the evidence behind excising the natural-erasure
stack from v1 (and behind [C1], which the surviving model now requires). The model, its 6 configs,
runner, and logs remain in the tree as the record; any v2 must re-run and extend them.
