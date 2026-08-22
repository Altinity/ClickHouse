---
description: 'TLC evidence for durable condemn-marker gating and logical-content NoDangle safety'
sidebar_label: 'CAS condemn-marker TLC results'
sidebar_position: 5
slug: /superpowers/models/CaGcCondemnMarkerGate-results
title: 'CAS condemn-marker TLC results'
doc_type: 'reference'
---

# `CaGcCondemnMarkerGate` results {#cagccondemnmarkergate-results}

## Verdict {#verdict}

The focused gate passed twice from identical final model inputs. The missing-marker sabotage violated the exact
invariant `NoDangle`, while the durable-marker gate exhausted its state graph without an error. The
runner printed `ALL EXPECTATIONS MET` only after both exact expectations matched.

`NoDangle` now expresses logical content identity rather than incarnation-token identity. This
model has one fixed content-addressed key and every present body carries that key's content, so a
committed reference requires `body.present`; it does not require the replacement body's current
token to equal the token previously observed by the writer. Tokens continue to authorize exact
deletion.

## Checker and finite scope {#checker-and-finite-scope}

- TLC: `2026.07.18.145032`, revision `30cc360`.
- Pinned `tla2tools.jar` SHA-256:
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Workers: `1`.
- One fixed content-addressed body slot and one writer edge.
- `MaxGen = 3` and `MaxRounds = 6` in both configurations.
- Breadth-first search, deadlock checking, `TypeOK`, and `NoDangle` are enabled.
- Raw TLC logs, metadata, and generated traces are retained below repository `tmp`; generated
  source-adjacent trace modules were moved there before staging.

The model covers a swallowed condemn-marker write, same-token writer adoption in the cut/redelete
window, round graduation, confirmed durable marker evidence, marker retry, and exact-token deletion.
It is a focused gate for that interleaving, not the broader split publication protocol owned by
`CaBlobPublishCore`.

## Safety invariant {#safety-invariant}

```tla
NoDangle == committed => body.present
```

Presence is content identity in this one-key abstraction. `body.tok` is deliberately absent from
the invariant: an equivalent body replacement may have a new provider token without invalidating a
logical content-addressed reference.

## First complete run {#first-complete-run}

Command output: `build/task2_condemnmarker_tla.log`. TLC row-log run ID:
`CaGcCondemnMarkerGate-11-1787386011121917359`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Queued | Depth | Seconds |
|---|---|---|---:|---:|---:|---:|---:|
| `CaGcCondemnMarkerGate_bug.cfg` | violation | `NoDangle` violated | 381 | 227 | 48 | 11 | 0 |
| `CaGcCondemnMarkerGate_fix.cfg` | green | `green` | 1,540 | 808 | 0 | 17 | 0 |

The runner exited `0` and its final line was exactly `ALL EXPECTATIONS MET`.

## Second complete run {#second-complete-run}

Command output: `build/task2_condemnmarker_rerun.log`. TLC row-log run ID:
`CaGcCondemnMarkerGate-11-1787411329587200063`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Queued | Depth | Seconds |
|---|---|---|---:|---:|---:|---:|---:|
| `CaGcCondemnMarkerGate_bug.cfg` | violation | `NoDangle` violated | 381 | 227 | 48 | 11 | 1 |
| `CaGcCondemnMarkerGate_fix.cfg` | green | `green` | 1,540 | 808 | 0 | 17 | 0 |

The rerun also exited `0` and ended with exact `ALL EXPECTATIONS MET`. The deterministic state
counts and depths match the first complete run.

## Counterexample audit {#counterexample-audit}

The missing-marker trace was inspected rather than accepted from a generic TLC error:

- Before the invariant edit, `run_condemnmarker.sh bug` produced the exact `NoDangle` violation at
  depth `11`, with `381` generated, `227` distinct, and `48` queued states.
- After changing `NoDangle`, the same selected sabotage produced the same exact violation and state
  counts. This sensitivity check proves the logical invariant was not weakened into green.
- In the trace, `WCommit` makes `committed = TRUE` while
  `body = [present |-> TRUE, tok |-> 1]`. `GSettleRedelete` then consumes the retired entry and sets
  `body = [present |-> FALSE, tok |-> 0]` while `committed` remains true. The violation is caused by
  absence of the key's content, not by token mismatch.

The fixed configuration gates graduation on confirmed durable `Condemned` evidence or a synchronous
metadata re-read. Without confirmation, the retired entry is carried and the marker write retried.
Its entire graph completes with zero queued states.

## Commands {#commands}

```bash
docs/superpowers/models/run_condemnmarker.sh bug \
    > build/task2_condemnmarker_red_control.log 2>&1
docs/superpowers/models/run_condemnmarker.sh bug \
    > build/task2_condemnmarker_red_after_nodangle.log 2>&1
docs/superpowers/models/run_condemnmarker.sh \
    > build/task2_condemnmarker_tla.log 2>&1
docs/superpowers/models/run_condemnmarker.sh \
    > build/task2_condemnmarker_rerun.log 2>&1
```

The two selected negative controls each exited `0` because their declared exact sabotage
expectation matched. Both complete batteries were accepted only through the runner's exact row
parser and final marker.

## Scope note {#scope-note}

This is a bounded safety proof for durable condemn-marker gating. It does not model provider
wire-format behavior, multipart publication, or the split `HEAD`/publication transition system;
those are separate implementation/live-test obligations and `CaBlobPublishCore` model scope.
