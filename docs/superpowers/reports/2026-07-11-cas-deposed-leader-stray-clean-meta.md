---
description: 'Real safety finding: a deposed GC leader''s pre-CAS clearSparedMeta leaves a durable stray-Clean freshness meta over a still-delete_pending blob, defeating the resurrect gate so a writer dedup-reuses the condemned exact token, which a later pending redelete deletes — a live-blob data-loss hole under concurrent leaders. Pre-existing in v3; found via the retired-in-snapshot TLA gate.'
sidebar_label: 'Finding: deposed-leader stray-Clean meta'
sidebar_position: 20
slug: /superpowers/reports/deposed-leader-stray-clean-meta
title: 'CAS GC finding — deposed-leader stray-Clean clearSparedMeta (INV_NO_LOSS)'
doc_type: 'reference'
---

# CAS GC finding — deposed-leader stray-Clean `clearSparedMeta` (INV_NO_LOSS) {#title}

**Status:** **FIXED 2026-07-11** (commits `730b59cd686` code + `96c571700382` TLA gate). **Severity:**
data loss (a referenced blob deleted) — **low probability** (requires concurrent GC leaders + a specific
spare/depose + reuse race), **high impact**. **Pre-existing in v3** — NOT introduced by the
retired-in-snapshot refactor (which keeps `.meta` writes byte-for-byte). Found 2026-07-11 while
building the retired-in-snapshot TLA gate (`CaRetiredInRun`, task-1 review finding I1: modeling the
deposed leader). **Branch:** `cas-gc-rebuild`.

## Fix (2026-07-11): GC freshness metadata is ADD-ONLY {#fix-addendum}

Design: [`specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md`](../specs/2026-07-11-cas-deposed-leader-clearsparedmeta-fix-design.md).
Two independent strong-model consults. The first proposed **Fix 1** (adoption-couple the spare clear —
move `clearSparedMeta` after the winning `gc/state` CAS) + a `condemn_round` guard. The second proved
Fix 1 **insufficient**: the destructive exact-token redelete `deleteExact(t1)` is itself issued **pre-CAS**
by a *stale, paused* leader working from an older adopted `delete_pending(t1)` snapshot, so a fresh
leader's *legitimately adopted* post-CAS clear flips the meta to Clean, a writer reuses `t1`, and the
stale leader's `deleteExact(t1)` deletes the live reuse — the final `gc/state` CAS fences **adoption, not
pre-CAS side effects**.

**Chosen fix (Fix 4):** make GC freshness metadata **add-only** — GC never transitions
`Condemned → Clean` on a spare (the spare-side `clearSparedMeta` call + helper are removed); only a
**writer** that has displaced the body with a **fresh incarnation token** publishes `Clean`
(`uploadFromSource` + `writeResurrectMetaClean` / `copyForwardFromCondemned`). This restores the
exact-token delete argument in full: any stale `deleteExact(t1)` finds the body absent or `TokenMismatch`
(the writer re-referenced the condemned hash by resurrecting to `t2`, never reusing `t1`). Accepted cost:
a spared hash stays `Condemned` until a writer resurrects it (write amplification on recurring-hash churn;
safe — normal reads never consult the meta). The pre-CAS `writeCondemnedMeta` (add-only) and
`deleteConfirmedMeta` (token already consumed) are unchanged.

**Gates.** Code (`730b59cd686`): the RED-first two-leader regression demonstrated *actual* live-blob data
loss (`hr.exists == false`) on the clear-on-spare code; after the fix, `SpareLeavesMetaCondemned` +
`StaleRedeleteAfterSpareDoesNotDeleteLiveReuse` pass, full `Cas*` 542/542. TLA (`96c571700382`,
`CaRetiredInRunFoldAbortWitness` made add-only + a split-action two-leader model): honest **GREEN** (all 4
invariants, 65.4M states); sabotages `inmem_token` / `attempt_reuse` / `no_pacing` / `gc_clear_on_spare`
**RED**, and — load-bearing — **`post_adoption_clear` RED** (an authentic 16-state two-leader
counterexample where Fix 1's post-CAS clear still loses the live blob, proving add-only is required).

## Mechanism {#mechanism}

`clearSparedMeta` (`Core/CasGc.cpp:106-113`) clears a blob's freshness meta `Condemned → Clean` when
a GC round SPARES it (in-degree recovered before graduation). It is scheduled on the spare verdict
(`CasGc.cpp:~415`, `scheduleMetaJob(... clearSparedMeta ...)`) and completed by `meta_pool->wait()`
(`CasGc.cpp:~513`) **BEFORE** the round's single `gc/state` CAS.

Therefore a GC leader that computes a spare, runs `clearSparedMeta` (durable Clean), and then **loses
the `gc/state` CAS** (deposed by a concurrent leader) leaves a **durable stray-Clean** meta over a
blob that is still `delete_pending` in the durably-adopted run.

Stray-**Clean** is the UNSAFE direction (the spec's v3 meta-race argument only covers stray-
**Condemned**, which is safe — it costs at most an unnecessary resurrect). Stray-Clean defeats the
writer's freshness gate: a writer dedup-hitting the blob point-reads Clean and **reuses the exact
condemned incarnation token** (no resurrect, no token bump). A later round then executes the pending
**exact-token redelete** — which is correctly keyed off the durably-adopted token — and, because the
reuse is the SAME token, it **deletes the live reused incarnation**. Exact-token delete only protects
against *resurrected* (different-token) incarnations; it does not protect a same-token stale reuse.

## TLA+ reproduction {#repro}

Witness model: `docs/superpowers/models/CaRetiredInRunFoldAbortWitness.tla` +
`CaRetiredInRunFoldAbortWitness.cfg`, runner `run_foldabort_witness.sh`. It is the honest
`CaRetiredInRun` gate plus a faithful `FoldAbort` action (a deposed leader: same pre-CAS exact-token
delete + advisory `.meta` writes as `FoldRound`, then does NOT adopt, bumps `nextAttempt`). Run:

```
docs/superpowers/models/run_foldabort_witness.sh CaRetiredInRunFoldAbortWitness.cfg
```

Result: **RED — `INV_NO_LOSS` violated** (reproduced twice independently: 611269/286130 and
559067/263090 states). Counterexample (single blob `b1`):

1. `WriterAdd(b1)` → phys=tok1, live edge journalled.
2. `FoldRound` r1 (cut past the edge) → condemn b1 tok1; meta→cond.
3. `WriterRemove(b1)`; `WriterStaleReuse(b1)` → one-round-stale dedup-hit re-references **tok1** (no bump).
4. `FoldRound` r2 (cut misses the stale edge) → graduate b1 → `delete_pending` tok1.
5. `WriterRemove(b1)` removes the stale edge.
6. **`FoldAbort`** picks a cut covering the stale add → sees d>0 → SPARE → advisory `clearSparedMeta`
   → **meta[b1]=Clean**, then loses adoption (`adopted` still holds `delete_pending` tok1).
7. `WriterAdd(b1)` reads the genuinely-Clean meta → **dedup-reuses tok1** (no resurrect).
8. `FoldRound` r3 (racing cut misses the re-add) → d=0 for the pending row → **exact-token redelete of
   tok1**, phys→0.
9. `FoldRound` r4 folds the live edge into coverage while phys=0 → `INV_NO_LOSS`: a folded live
   reference to an absent blob.

**Isolation:** restricting the deposed leader's advisory meta to *condemn-only* (add-only — never
clear to Clean) makes the model GREEN (935815 distinct states). So the stray-Condemned effects the
spec claims safe ARE verified safe; the SOLE violation source is the stray-Clean `clearSparedMeta`.
`FoldRound`'s identical clean-on-spare write is safe only because it is COUPLED to adoption (the
pending row is dropped in the same atom) — the hole exists purely in the decoupled deposed-leader case.

## Candidate fixes (protocol decision — not yet chosen) {#fixes}

1. **Couple `clearSparedMeta` to adoption** — only clear post-CAS (accept the point-read-gate timing
   change: the meta stays Condemned a bit longer, costing at most extra resurrects, the safe direction).
2. **Re-verify at redelete** — the exact-token redelete additionally confirms the incarnation is still
   condemned/absent (a HEAD or a meta re-check) rather than trusting only the durable pending row.
3. **Atomic re-condemn-then-clear** — the spare verdict re-condemns then clears only as part of the
   adopted atom, so a deposed leader never leaves a durable clear.

Option 1 is the smallest and most in-keeping with the fail-safe direction; option 2 adds an op to the
delete path; option 3 is the most invasive. Needs a brainstorm + its own TLA gate (re-run the witness
green after the fix). Consult a fresh model on the choice.

## Why it was nearly missed {#process}

The original `CaRetiredInRun` gate (green) did not model the deposed leader, so the hole was invisible.
The I1 review recommendation (model the deposed leader) surfaced it. A first "fix" that made the model
advisory-add-only turned it green — but that was a FALSE-GREEN that masked the real code behavior
(`clearSparedMeta` really does clear-to-Clean pre-CAS). The BLOCKED verdict + the not-weaken-invariants
discipline caught it; the code was then confirmed. Lesson: a model that is *safer than the code* hides
real bugs — model the code's actual effects, not the intended ones.
