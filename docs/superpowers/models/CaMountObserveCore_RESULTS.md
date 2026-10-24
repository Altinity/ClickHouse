---
description: 'TLC evidence for observer timing of the CAS mount lease: a GC round or a re-mounting server fences a mount only after the holder authority ended. Covers the CAS-342 sabotage, the measured fence margin and the witnesses.'
sidebar_label: 'CAS mount observer timing TLC results'
sidebar_position: 6
slug: /superpowers/models/CaMountObserveCore-results
title: 'CAS mount observer timing TLC results'
doc_type: 'reference'
---

# `CaMountObserveCore` results {#camountobservecore-results}

## Verdict {#verdict}

The battery passed: `run_mountobserve.sh` printed `EXECUTED ROWS: 20/20`, `SWEPT POINTS: 12/12`
and `ALL EXPECTATIONS MET`.

- **The fixed rule is safe.** A sighting dated after its read, and a stability test that uses a
  sample taken before the confirming read, never fence a holder inside its authority. This holds
  for a holder that keeps its lease for the whole `TTL`, with clock drift up to the allowance.
- **CAS-342 is reproduced.** Dating a sighting with the round-start sample fences a holder inside
  its authority, on the first decision and on the re-decision after a refused fence-out.
- **The margin is the cadence term.** The fence lands `Cadence / 1.05` after authority ends at
  full drift: 9.5 s for GC and 4.8 s for a re-mount with the default settings.

## What the model contains {#model}

One holder, one observer, one mount slot.

- **Holder.** `RenewStart` samples the start on `hclock` before the request. `RenewLand` installs
  a fresh token at any later time, because the holder retries until the store answers.
  `RenewAck` sets `lastStart` to that start. `Write` is admitted while
  `hclock < lastStart + AuthoritySpan`. `RenewLearnFenced` is the definitive answer.
- **Observer.** `ObsSample` reads `oclock` into `roundSample`. `ObsRead` reads the slot any number
  of ticks later. `ObsDecide` dates a new token with `oclock` and tests stability with
  `roundSample >= watchSince + Threshold`. `ObsFenceWrite` is conditional on the token the decision
  saw. A refused fence is followed by `ObsReRead` and a second `ObsDecide`.
- **Threshold.** `TTL + Allowance + Cadence`, as `mountObservationThresholdMs`. `Cadence` is
  `Period` for GC and `Period / 2` for `claimMountAwaitingExpiry`.

The observer is more adversarial than the code: a read may reuse a sample of any age, and the fence
write may land at any later time.

## Timing assumption {#timing-assumption}

`hclock` and `oclock` are separate variables. No action and no invariant compares them.

Their only coupling is a rate bound. `TickBoth` advances both. `TickObserverOnly` and
`TickHolderOnly` advance one, and each needs `GainEvery` joint ticks since the last solo tick. One
solo tick can be banked, so over `X` holder ticks the observer gains at most
`floor(X / GainEvery) + 1`. With `GainEvery = TTL / Allowance` this is the code's assumption (the
`TTL / 20` term) plus one tick of jitter.

`CaMountObserveCore_env_drift_exceeds_allowance.cfg` breaks the bound and is red: the rule depends
on it.

## Invariants {#invariants}

| Invariant | Says |
|---|---|
| `NoFenceInsideAuthority` | When the fence lands, `hclock >= lastStart + AuthoritySpan`, and no late `RenewAck` changes that |
| `FenceMarginAtLeast` | The fence lands at least `MinMargin` holder ticks after authority ended |
| `NoWriteAfterFence` | `Write` is never admitted after the fence landed |
| `HealthyNeverFenced` | A holder whose renewals land on time is never fenced |
| `HealthyKeepsAuthority` | That holder is never without authority |
| `TypeOK` | Variable domains |

## Sabotages and traces {#sabotages}

All at `TTL = 4`, `Period = 2`, `Allowance = 1`, `Cadence = 2` (`Threshold = 7`), `GainEvery = 4`,
`MaxClock = 14`. Each violates `NoFenceInsideAuthority`.

| Config | Defect | Trace |
|---|---|---|
| `_sab_sighting_before_read` | CAS-342: a first sighting is dated with `roundSample` | `ObsSample` at 0, the holder renews at 7, `ObsRead` sees that token, `ObsDecide` dates it 0, `ObsSample` at 7, `ObsFenceWrite` at 7 with authority until 11 |
| `_sab_redecision_stale_sample` | A sighting after a refused fence is dated with `roundSample` | Token 1 watched from 0, `ObsSample` at 7, `ObsRead` at 14, the holder renews at 14, `ObsFenceWrite` is refused, `ObsReRead` sees token 2, `ObsDecide` dates it 7, `ObsSample` at 14, `ObsFenceWrite` at 14 with authority until 18 |
| `_sab_wrapping_stability` | The stability test wraps when the sample precedes the sighting | `ObsSample` at 0, `ObsRead` and `ObsDecide` at 1 date the token 1, a second `ObsRead` under the sample 0 is "stable", `ObsFenceWrite` at 1 with authority until 4 |
| `_env_drift_exceeds_allowance` | No defect: `GainEvery = 1`, outside the assumption | Token watched from 0, four `TickObserverOnly` among three `TickBoth`, `ObsSample` at observer tick 7, `ObsFenceWrite` at holder tick 3 with authority until 4 |

The third sabotage has no route through `computeHeartbeatFloor`: `readModifyWrite` returns as
soon as the decision declines, so a slot is not decided twice under one sample after a sighting.
It guards the shared helper: rule 11 allows the two samples in either order, and an unsigned
subtraction of a later sighting from an earlier sample is a huge number.

### How long a walk the defect needs {#walk-bound}

`MaxWalk` bounds the ticks between a sample and the reads decided with it. With both stale stamps
on, which is `computeHeartbeatFloor` taking one `mono_now_ms` argument for every stamp:

| Clocks | Green up to | Red from | Configs |
|---|---|---|---|
| One rate (`GainEvery = 0`) | `MaxWalk = 3` | `MaxWalk = 4` | `_stale_stamps_nodrift_short_walk`, `_stale_stamps_nodrift_long_walk` |
| One tick of drift per lease (`GainEvery = 4`) | `MaxWalk = 2` | `MaxWalk = 3` | `_stale_stamps_short_walk`, `_stale_stamps_long_walk` |

3 is `Threshold - TTL`. The defect needs a walk longer than that, less what the observer's clock
gains in one lease. In seconds: 11.5 s, and 10 s at 5 percent drift.

## Fence margin {#fence-margin}

The margin is `hclock` at the fence minus the end of authority. Each value below is exact: green
with `MinMargin` at that value, `FenceMarginAtLeast` violated one above.

| Config pair | Observer | `AuthoritySpan` | Margin |
|---|---|---:|---:|
| `_gc`, `_margin_gc_tight` | GC, `Threshold = 7` | 4 (`TTL`) | 1 |
| `_remount`, `_margin_remount_tight` | re-mount, `Threshold = 6` | 4 (`TTL`) | 0 |
| `_earlyfence`, `_margin_earlyfence_tight` | GC | 3 | 2 |

The margin survives a holder that uses its whole `TTL`. A holder that gives up earlier gets a
margin larger by exactly the ticks it gave up.

Every measured point fits one formula. Let `X` be the least holder-tick count with
`X + G(X) >= Threshold`, where `G(X) = 0` for `GainEvery = 0` and `floor(X / GainEvery) + 1`
otherwise. Then the margin is `X - AuthoritySpan`. The formula was written before the sweep ran,
and all 12 points matched.

| Point | `TTL` | `Period` | `Allowance` | `Cadence` | `AuthoritySpan` | `GainEvery` | Margin |
|---|---:|---:|---:|---:|---:|---:|---:|
| `nodrift_gc` | 4 | 2 | 1 | 2 | 4 | 0 | 3 |
| `nodrift_remount` | 4 | 2 | 1 | 1 | 4 | 0 | 2 |
| `noallowance` | 4 | 2 | 0 | 2 | 4 | 0 | 2 |
| `drift1_gc` | 4 | 2 | 1 | 2 | 4 | 4 | 1 |
| `drift1_remount` | 4 | 2 | 1 | 1 | 4 | 4 | 0 |
| `drift2_gc` | 4 | 2 | 1 | 2 | 4 | 2 | 0 |
| `allowance2` | 4 | 2 | 2 | 2 | 4 | 4 | 2 |
| `span2` | 4 | 2 | 1 | 2 | 2 | 4 | 3 |
| `ttl6_gc` | 6 | 2 | 1 | 2 | 6 | 6 | 1 |
| `ttl6_cadence3` | 6 | 3 | 1 | 3 | 6 | 6 | 2 |
| `ttl6_drift2` | 6 | 3 | 1 | 3 | 6 | 3 | 1 |
| `ttl10_gc` | 10 | 3 | 1 | 3 | 10 | 10 | 2 |

Read as a function:

- No drift: the margin is `Allowance + Cadence`.
- Drift equal to the allowance: the margin is `Cadence` minus the one banked tick.
- Drift above the allowance eats the cadence term (`drift2_gc`: margin 0), then authority.

The continuous form is `(TTL + TTL/20 + Cadence) / (1 + drift) - AuthoritySpan`. With the default
settings (`TTL` 30 s, `Period` 10 s):

| Observer | Threshold | Margin, no drift | Margin, 5 percent drift |
|---|---:|---:|---:|
| GC | 41.5 s | 11.5 s | 9.5 s |
| Re-mount | 36.5 s | 6.5 s | 4.8 s |

These four numbers come from the formula. TLC did not run at that scale.

## Witnesses {#witnesses}

Negated reachability, with every safety invariant enabled.

| Config | Shows |
|---|---|
| `_witness_observer_fences` | The observer fences a holder that stopped: token watched from 0, fence at 7 |
| `_witness_failing_holder_keeps_lease` | `RenewStart` at 3 never lands, `Write` at 3 (the last tick, authority ends at 4), fence at holder tick 6 |
| `_witness_refused_fence_redecided` | The holder's renewal starts at 7 and lands between `ObsRead` and `ObsFenceWrite`, the fence is refused, `ObsDecide` dates the new token 7 (after `ObsReRead`), and a fence lands at 14; that renewal gives authority until 11 at most |
| `_witness_healthy_outlives_threshold` | The observer waits 7 ticks and finds that a healthy holder moved the token |

`_healthy_gc` and `_healthy_remount` have equal state counts: the stability test never passes for a
healthy holder, so the cadence term is never consulted.

## Recorded run {#recorded-run}

```bash
SWEEP=1 TLC_JAVA_OPTS=-Xmx16g TLC_GREEN_WORKERS=6 docs/superpowers/models/run_mountobserve.sh
```

- TLC `2026.07.18.145032`, revision `30cc360`; `tla2tools.jar` SHA-256
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Violation and witness rows: 1 worker. Green rows: 6 workers; the depth of a green row can vary
  between runs, its state counts cannot.
- Every green row ends with `0 states left on queue`.

| Configuration | Expected | Observed | Generated | Distinct | Depth |
|---|---|---|---:|---:|---:|
| `_sab_sighting_before_read` | violation | `NoFenceInsideAuthority` | 2,653,946 | 1,119,526 | 18 |
| `_sab_redecision_stale_sample` | violation | `NoFenceInsideAuthority` | 167,525,121 | 52,571,711 | 31 |
| `_sab_wrapping_stability` | violation | `NoFenceInsideAuthority` | 3,737 | 2,164 | 8 |
| `_stale_stamps_long_walk` | violation | `NoFenceInsideAuthority` | 2,411,514 | 1,062,208 | 18 |
| `_stale_stamps_nodrift_long_walk` | violation | `NoFenceInsideAuthority` | 148,329 | 66,773 | 18 |
| `_env_drift_exceeds_allowance` | violation | `NoFenceInsideAuthority` | 747,415 | 285,596 | 15 |
| `_margin_gc_tight` | violation | `FenceMarginAtLeast` | 806,599 | 365,166 | 15 |
| `_margin_remount_tight` | violation | `FenceMarginAtLeast` | 431,647 | 200,046 | 14 |
| `_margin_earlyfence_tight` | violation | `FenceMarginAtLeast` | 746,933 | 345,275 | 15 |
| `_gc` | green | green | 219,879,569 | 65,946,998 | 48 |
| `_remount` | green | green | 234,922,803 | 70,541,891 | 51 |
| `_earlyfence` | green | green | 194,883,584 | 60,240,243 | 48 |
| `_stale_stamps_short_walk` | green | green | 110,925,407 | 35,294,799 | 52 |
| `_stale_stamps_nodrift_short_walk` | green | green | 4,700,725 | 1,585,241 | 50 |
| `_healthy_gc` | green | green | 5,943,858 | 1,691,748 | 43 |
| `_healthy_remount` | green | green | 5,943,858 | 1,691,748 | 43 |
| `_witness_observer_fences` | witness | `W_ObserverFences` | 794,509 | 360,178 | 15 |
| `_witness_failing_holder_keeps_lease` | witness | `W_FailingHolderKeepsLeaseToItsEnd` | 2,619,438 | 1,136,420 | 17 |
| `_witness_refused_fence_redecided` | witness | `W_RefusedFenceIsRedecided` | 156,497,550 | 49,767,644 | 30 |
| `_witness_healthy_outlives_threshold` | witness | `W_HealthyHolderOutlivesTheThreshold` | 79,782 | 27,695 | 17 |

Sweep rows, horizon `Threshold + 3`. The green half of each point is exhaustive:

| Point | Green: generated | Green: distinct | Depth | Red: generated | Red: distinct | Depth |
|---|---:|---:|---:|---:|---:|---:|
| `nodrift_gc` | 1,078,765 | 346,243 | 33 | 59,797 | 27,287 | 15 |
| `nodrift_remount` | 594,172 | 190,511 | 32 | 34,804 | 16,083 | 14 |
| `noallowance` | 594,172 | 190,511 | 32 | 34,804 | 16,083 | 14 |
| `drift1_gc` | 19,595,890 | 5,944,540 | 37 | 778,869 | 348,962 | 15 |
| `drift1_remount` | 9,621,705 | 2,944,387 | 36 | 410,526 | 187,345 | 14 |
| `drift2_gc` | 17,201,713 | 4,907,747 | 38 | 775,478 | 319,168 | 15 |
| `allowance2` | 37,693,533 | 11,314,792 | 38 | 1,433,263 | 630,217 | 16 |
| `span2` | 12,933,411 | 4,323,887 | 37 | 638,686 | 297,924 | 15 |
| `ttl6_gc` | 74,616,789 | 21,707,527 | 39 | 2,679,351 | 1,135,538 | 17 |
| `ttl6_cadence3` | 48,880,334 | 14,613,928 | 37 | 3,135,975 | 1,317,900 | 18 |
| `ttl6_drift2` | 43,169,331 | 12,180,045 | 38 | 3,164,768 | 1,242,188 | 18 |
| `ttl10_gc` | 351,042,621 | 96,837,486 | 41 | 22,703,480 | 8,562,247 | 22 |

## Findings against the design {#findings}

Design: `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md`.

- **Rule 11 holds.** Nothing in the design has to change for safety.
- **The walk bound of step one is confirmed.** The defect needs a walk longer than
  `Threshold - TTL`, less the drift gain ([walk bound](#walk-bound)).
- **Rule 11 should name the order of the two samples.** The test `sample - sighting >= threshold`
  must be false when the sample precedes the sighting. The design says so only in test 1.
- **The early self-fence does not hide CAS-342 on every path.** It ends authority early only for a
  holder whose renewals fail. `CasMountRuntime::admit` refuses on `lost`, on a changed generation
  or on the clock reaching the deadline, and a renewal thread that is stalled sets none of them
  before `start + TTL`. So the code before this design has that exposure too. This is read from
  the code, not reproduced. `_sab_sighting_before_read` runs with `AuthoritySpan = TTL` and
  describes that path as well.

## Limits {#limits}

- **Scale.** Exhaustive at `TTL = 4` with a horizon of 14 ticks, and for the margin at `TTL` 4, 6
  and 10 with a horizon of `Threshold + 3`. `Allowance = 1` tick is 25, 17 and 10 percent of the
  lease there, not 5.
- **A config with exactly 5 percent did not complete.** `TTL = 20`, `Period = 6`, `Allowance = 1`,
  `GainEvery = 20`, `MaxClock = 28` was stopped at 184,038,696 distinct states, depth 35, with
  47 million states on the queue and growing. It is not in the runner.
- **The rate bound is an assumption.** A holder whose clock stops while the observer's runs (a
  paused virtual machine whose `BOOTTIME` does not advance) is outside it.
- **`Write` is admission.** A request admitted inside authority that lands after the fence is not
  modelled here. The successor's seal and generation check in `CaCasMountCore` cover it.
- **The start of a renewal precedes its first request.** The model takes this from rule 3;
  `CaMountRenewRetryCore` owns it.
- **One observer.** A second GC leader has its own sightings and its own conditional write. Their
  interplay is not modelled.
- **No remount.** The model stops at the fence. Reclaim and the next epoch are `CaCasMountCore`.
