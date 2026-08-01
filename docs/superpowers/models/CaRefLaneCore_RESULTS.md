---
description: 'TLC results for the simplified CAS reference lane and its relink composition.'
sidebar_label: 'CAS ref lane TLA+ results'
sidebar_position: 20260730
slug: /superpowers/models/cas-ref-lane-core-results
title: 'CAS reference lane TLA+ results'
doc_type: 'reference'
---

# CAS reference lane TLA+ results {#cas-reference-lane-tla-results}

**Date:** 2026-08-01. **Verdict:** pass.

## Lane model {#lane-model}

`CaRefLaneCore.tla` models the six ownership states `Ready`, `Writing`, `Wedged`,
`NeedsRecovery`, `Closed`, and `Faulted`. It composes that single lane with a bounded cache model
containing two concrete runtime ids and two durable life ids. The predecessor runtime remains owned
by its captured handle while the non-authoritative logical-name slot may publish a distinct
successor after removal/rebirth or same-life self-remount.

The battery ran with:

```bash
docs/superpowers/models/run_reflane.sh
```

All 24 expectations passed:

- one honest exhaustive configuration was green;
- eleven single-rule sabotages violated their named invariant;
- twelve reachability witnesses violated their negated witness invariant.

The honest run generated 1,017,625 states, found 278,718 distinct states, reached depth 25, and
exhausted the queue. The retained run summary is
`build/test_CaRefLaneCore_checkpoint75c_final2.log`; detailed TLC output is under
`build/tlc-runs/reflane/20260801-checkpoint75c-final2`.

The sabotage controls cover arming before send, retaining uncertain attempts, blocking later
appends, complete recovery, exact attempt identity, mount fencing, and `Ready`-only certification.
The new controls independently violate `NoOldHandleRetarget`,
`ExactPredecessorInvalidationPreservesSuccessor`, `PublishedRuntimeHasAcceptedIdentity`, and
`MissingNameConfirmationAllocatesNothing`. The new witnesses reach predecessor/successor
coexistence with a predecessor-scoped old operation, same-life self-remount at accepted generation
3 after fence-loss generation 2, exact delayed invalidation that preserves the successor, and an
allocation-free missing-name confirmation. The earlier witnesses continue to reach ordinary commit,
unresolved write, recovery, inert unrelated result, `Closed`, and `Faulted`.

## Relink composition {#relink-composition}

`CaRelinkLaneComposition.tla` consumes the lane as a six-state component. It checks three seam
properties:

1. confirmation requires `Ready`;
2. promotion uses the exact confirmed identity;
3. source deletion requires receiver ownership.

The battery ran with:

```bash
docs/superpowers/models/run_relinklane.sh
```

All nine expectations passed: one honest configuration, three named sabotage violations, and five
reachability witnesses. The honest run generated 341 states, found 96 distinct states, and exhausted
the queue. The retained run summary is
`build/test_CaRelinkLaneComposition_20260730_r2.log`; detailed TLC output is under
`build/tlc-runs/relinklane/20260730T084009-609`.

## Conclusion {#conclusion}

The reference lane is now modeled from its semantic obligations rather than reconstructed from the
old implementation's nested decisions. The C++ implementation follows that model, and the relink
seam depends only on the small `Ready` certification contract.
