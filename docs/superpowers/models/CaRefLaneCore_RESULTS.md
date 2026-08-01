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
successor after removal/rebirth or same-life self-remount. `StartWrite` captures `lane_runtime` in
`attempt_runtime`; `BeginResolve` carries it into `resolver_runtime`; every resolver observation
records `observation_runtime`; and the real write, install, observation, resolution, and recovery
transitions update or validate per-runtime projections of `cache_id` and `durable_id`.

The battery ran with:

```bash
docs/superpowers/models/run_reflane.sh
```

All 26 expectations passed:

- one honest exhaustive configuration was green;
- twelve single-rule sabotages violated their named invariant;
- thirteen reachability witnesses violated their negated witness invariant.

The honest run generated 952,403 states, found 296,280 distinct states, reached depth 25, and
exhausted the queue. The retained run summary is
`build/test_CaRefLaneCore_r2_final2.log`; detailed TLC output is under
`build/tlc-runs/reflane/r2-final2`.

The sabotage controls cover arming before send, retaining uncertain attempts, blocking later
appends, complete recovery, exact attempt identity, mount fencing, and `Ready`-only certification.
The new controls independently violate `NoOldHandleRetarget`,
`ExactPredecessorInvalidationPreservesSuccessor`, `PublishedRuntimeHasAcceptedIdentity`, and
`MissingNameConfirmationAllocatesNothing`. Every focused sabotage config checks all non-target
safety invariants before its expected RED. The new witnesses reach predecessor/successor
coexistence with a real predecessor-scoped `WriteLands`, same-life self-remount at accepted generation
3 after fence-loss generation 2, exact delayed invalidation that preserves the successor, and an
allocation-free missing-name confirmation. The old-handle sabotage drives that same real transition
to `r2`, while the honest trace updates only `r1`. Missing-confirm sabotage only reattaches already
valid live `r1` to an armed empty slot, so published identity remains valid. The earlier witnesses continue to reach ordinary commit,
unresolved write, recovery, inert unrelated result, `Closed`, and `Faulted`.

Resolver provenance is checked independently of application targeting. In the focused sabotage,
`resolver_runtime = r1` survives successor publication, but `ObserveSuccessorSealScoped` records
`observation_runtime = r2` from the current slot and violates only `NoOldHandleRetarget` after 2,689
generated / 1,003 distinct states at depth 8. The paired honest witness follows the same unresolved,
removal, rebirth, lookup, and successor-seal sequence, records the observation against `r1`, applies
it to `r1`, reaches `Closed`, and violates `W_RebirthResolverScope` after 9,071 generated / 3,941
distinct states at depth 9.

With only `NoOldHandleRetarget` omitted, the observation-retarget sabotage exhaustively preserves
every non-target invariant: 223,716 generated / 62,061 distinct states, empty queue, depth 21.

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
