---
description: TLC evidence for the B140-dangle merge fix — a gapless trim gate plus an atomic edges+cursor commit, each shown independently necessary.
sidebar_label: B140 dangle-merge results
sidebar_position: 1
slug: /superpowers/models/ca-b140-dangle-merge-results
title: CaB140DangleMerge — TLA+ gate results
doc_type: reference
---

# `CaB140DangleMerge` — TLA+ gate results {#ca-b140-dangle-merge-results}

The model checks the B140-dangle producer: a GC leader folds the journal into an in-memory
work-in-progress generation, persists it to durable storage only at a checkpoint, and trims the
journal based on a fold cursor. Two separately durable pieces — the committed snap edges and the
committed cursor — can drift apart across a leadership handoff, opening a gap in which a live
ref's tree-to-blob edge is folded by a leader that then loses the lease before persisting, is
trimmed away, and is never recovered by the next leader. The counted parent is then stripped, the
shared blob's in-degree hits zero, and it is deleted while a live ref still references it
(`INV_NO_LOSS`).

Two independent fixes compose: `TrimGated` (the journal may be trimmed only up to the *committed*
cursor, never a leader's in-memory fold) and `CursorInSnap` (the cursor is part of the atomically
committed snap, so it can never be published ahead of the edges it claims to cover). The four
configs are the full 2x2 over these two flags, using the historical `m_*.cfg` names (not the usual
`<Model>_*.cfg` prefix).

## Checker identity {#checker-identity}

- TLC: `2026.07.18.145032`, revision `30cc360`.
- SHA-256: `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Workers: 1, deterministic breadth-first search.
- Bounds (all configs): `Leaders = {L1, L2}`, `Trees = {t1, t2}`, `Blobs = {b1}`, `MaxGen = 3`,
  `MaxLog = 3`.

## Exact runner tail {#exact-runner-tail}

```text
CONFIG               EXPECT      RESULT                                   SECONDS  VERDICT
m_both_buggy         violation   violation:INV_NO_LOSS                    10       PASS
m_cursorskip         violation   violation:INV_NO_LOSS                    12       PASS
m_trimonly           violation   violation:INV_NO_LOSS                    5        PASS
m_merged             green       green                                    71       PASS

ALL EXPECTATIONS MET
```

## Per-configuration evidence {#per-configuration-evidence}

All configs assert `TypeOK`, `INV_NO_LOSS`, and `INV_NO_GC_LOSS`.

| Config | `TrimGated` | `CursorInSnap` | Outcome | Generated / distinct |
| --- | --- | --- | --- | --- |
| `m_both_buggy` | `FALSE` | `FALSE` | `INV_NO_LOSS` violated | 2,952,622 / 989,390 |
| `m_cursorskip` | `TRUE` | `FALSE` | `INV_NO_LOSS` violated | 3,412,120 / 1,202,174 |
| `m_trimonly` | `FALSE` | `TRUE` | `INV_NO_LOSS` violated | 1,175,187 / 441,391 |
| `m_merged` | `TRUE` | `TRUE` | green | 20,692,441 / 5,326,000 |

## Sabotage counterexamples {#sabotage-counterexamples}

- `m_both_buggy`: neither fix is present — the journal trims by any building leader's in-memory
  fold cursor, and the cursor commits independently of the edges. Both dangle mechanisms are live.
- `m_cursorskip`: `TrimGated = TRUE` alone (the journal trims only up to the *committed* cursor)
  is not sufficient while the cursor and edges can still commit as two separate steps
  (`GCommitEdges`/`GCommitCursor`) — a split commit can still let the durable cursor run ahead of
  the durable edges it claims to cover, reopening the gap `TrimGated` alone does not close.
- `m_trimonly`: `CursorInSnap = TRUE` alone (the cursor is atomically part of the committed snap)
  is not sufficient while the trim bound still consults a building leader's in-memory fold cursor
  — a leader can still trim past what any committed snap (cursor included) actually covers.

Each single-flag config is therefore shown independently insufficient: fixing only the trim bound
or only the commit atomicity still dangles. Only `m_merged`, combining both, is green — the two
fixes are jointly necessary, matching the model header's account of the merged design.

## Verdict {#verdict}

All four expected rows passed under the pinned TLC jar. The three single-fix (and no-fix)
sabotage configurations each reproduce `INV_NO_LOSS` violated; the merged fix is green over the
same bounds, so neither flag is redundant with the other.

## Reproduction {#reproduction}

```bash
docs/superpowers/models/run_b140danglemerge.sh
```
