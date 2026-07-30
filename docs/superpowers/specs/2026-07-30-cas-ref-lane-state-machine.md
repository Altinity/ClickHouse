---
description: 'A small ownership-state contract for the content-addressed reference append lane.'
sidebar_label: 'CAS ref lane state machine'
sidebar_position: 20260730
slug: /superpowers/specs/cas-ref-lane-state-machine
title: 'CAS reference lane state machine'
doc_type: 'reference'
---

# CAS reference lane state machine {#cas-reference-lane-state-machine}

**Date:** 2026-07-30. **Status:** implemented and model-checked. **Branch:** `cas-gc-rebuild`.

## Purpose {#purpose}

The old reference lane was described as the product of request results, occupant classifications,
resolution results, diagnostic reasons, a durable floor, a wedge, and an apply marker. Those values
are useful inside individual computations, but they are not independent states. Treating them as a
state machine made both the implementation and its formal model needlessly hard to reason about.

The replacement has one state variable and one ownership rule:

> The lane state says who owns the next safe action. An exact append attempt exists only while the
> writer or resolver owns it.

The executable specification is `docs/superpowers/models/CaRefLaneCore.tla`.

## States {#states}

| State | Owner and meaning | Permitted way out |
|---|---|---|
| `Ready` | No durable transaction is missing from the cached table. The lane may append and may certify its view. | Start one exact append attempt. |
| `Writing` | The writer owns an exact attempt installed before any request may be sent. | Commit and install; prove non-durability; or transfer the attempt to `Wedged`. |
| `Wedged` | The resolver owns the same exact attempt because its durability is uncertain. | Install that exact durable attempt; remain `Wedged`; enter `NeedsRecovery`; or terminate as `Closed`/`Faulted`. |
| `NeedsRecovery` | Durability is known, but installing the corresponding table state did not complete. No append or certification is allowed. | A full recovery walk installs the durable state and returns to `Ready`. |
| `Closed` | A successor seal proves that this writer epoch cannot append at the attempted slot. | A real remount replaces the runtime. |
| `Faulted` | Foreign or malformed bytes occupy the writer-owned slot. | A real remount replaces the runtime after the anomaly is surfaced. |

`Closed` and `Faulted` are terminal for one runtime. A test-only generation change is not a remount
and must not reopen either state.

## Invariants {#invariants}

1. `Ready` is certifiable: for the current runtime, the cached transaction and binding equal the
   durable transaction and binding.
2. `Writing` and `Wedged` retain exactly one attempt. Its transaction id is the cached successor.
3. `Ready`, `NeedsRecovery`, `Closed`, and `Faulted` retain no append attempt.
4. No new append begins outside `Ready`.
5. A resolver result may install state only when it names the exact retained attempt and the runtime
   still has authority.
6. If the exact attempt is proven durable after authority moved, it enters `NeedsRecovery`; it is not
   discarded as a stale result.
7. Only a completed recovery install returns `NeedsRecovery` to `Ready`.
8. Confirmation and snapshot publication are allowed only in `Ready`.

## Transition ownership {#transition-ownership}

The attempt is created completely before I/O and installed under the lane mutex while moving
`Ready -> Writing`. A definite pre-send or non-durable refusal clears it and returns to `Ready`.
An unresolved post-send outcome preserves the exact attempt and moves `Writing -> Wedged`.

Resolution performs its decoding and candidate construction before I/O. After I/O it rechecks both
the attempt identity and mount authority under the lane mutex:

- exact durable + current authority: install and move `Wedged -> Ready`;
- exact durable + moved authority, or a post-durable install exception: clear the attempt and move
  to `NeedsRecovery`;
- still unknown: preserve the attempt and remain `Wedged`;
- successor seal: clear the attempt and move to `Closed`;
- foreign or malformed occupant: clear the attempt and move to `Faulted`;
- a result for another attempt: no lane transition.

This last distinction is the defect exposed by the stopped model attempt: a durability-producing
retry cannot be treated as inert merely because its fence moved.

## Implementation mapping {#implementation-mapping}

The contract is represented by `RefLaneState` and `RefAppendAttempt` in `Pool/CasRefLedger.h`. Both
are guarded by `RefTableRuntime::state_mutex`. `CasRefLedger::commitRefChunk`,
`CasRefLedger::resolveWedgeOnce`, and `CasRefLedger::ensureRefTableRecovered` are respectively the
writer, resolver, and recovery owners.

The previous `RefApplyState`, durable-floor bookkeeping, and separate wedge/apply-marker encodings
are removed. Request and occupant enums remain local decision results; they no longer pretend to be
persistent lane states.

## Relink contract {#relink-contract}

Relink consumes only this public rule: a source view may be certified in `Ready`. The receiver
promotes only the exact certified identity, and source deletion follows receiver ownership. The
composition is checked independently by `docs/superpowers/models/CaRelinkLaneComposition.tla`.
