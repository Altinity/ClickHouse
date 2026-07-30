---
description: 'TLC results for the simplified CAS reference lane and its relink composition.'
sidebar_label: 'CAS ref lane TLA+ results'
sidebar_position: 20260730
slug: /superpowers/models/cas-ref-lane-core-results
title: 'CAS reference lane TLA+ results'
doc_type: 'reference'
---

# CAS reference lane TLA+ results {#cas-reference-lane-tla-results}

**Date:** 2026-07-30. **Verdict:** pass.

## Lane model {#lane-model}

`CaRefLaneCore.tla` models the six ownership states `Ready`, `Writing`, `Wedged`,
`NeedsRecovery`, `Closed`, and `Faulted`. It deliberately omits the nested implementation enums
that made the superseded model unmanageable.

The battery ran with:

```bash
docs/superpowers/models/run_reflane.sh
```

All 15 expectations passed:

- one honest exhaustive configuration was green;
- seven single-rule sabotages violated their named invariant;
- seven reachability witnesses violated their negated witness invariant.

The honest run generated 16,768 states, found 7,952 distinct states, and exhausted the queue.
The retained run summary is `build/test_CaRefLaneCore_20260730_r7.log`; detailed TLC output is under
`build/tlc-runs/reflane/20260730T084000-3`.

The sabotage controls cover arming before send, retaining uncertain attempts, blocking later
appends, complete recovery, exact attempt identity, mount fencing, and `Ready`-only certification.
The witnesses reach ordinary commit, unresolved write, retry-created adoption, recovery, inert
unrelated result, `Closed`, and `Faulted`.

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
