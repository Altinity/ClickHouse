---
description: 'Opt-round §5 (absence-means-Clean blob meta) as specified re-introduces the 2026-07-11 FIXED deposed-leader stray-Clean data-loss hole: its GcSpareHeal transition (spare clears the tombstone) is the removed clearSparedMeta under a new representation. Blocked; TLA gate was false-green; a token-preserving adoptEvidence recovery path refutes the safety premise.'
sidebar_label: 'Finding: §5 spare-clear reopens data loss'
sidebar_position: 21
slug: /superpowers/reports/s5-spare-clear-reopens-dataloss
title: 'CAS opt-round §5 finding — spare-clear tombstone reopens deposed-leader data loss'
doc_type: 'reference'
---

# CAS opt-round §5 finding — spare-clear tombstone reopens deposed-leader data loss {#title}

**Status:** **§5 BLOCKED 2026-07-14** (not implemented this round; no unsafe code landed — opt-t7 WIP
halted uncommitted). **Severity:** data loss (a referenced blob deleted) — low probability, high impact,
same class as [`2026-07-11-cas-deposed-leader-stray-clean-meta`](2026-07-11-cas-deposed-leader-stray-clean-meta.md).
**Branch:** `cas-gc-rebuild`.

## Summary {#summary}

Optimization-round §5 of `specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md`
("absence-means-Clean blob meta as a pure tombstone") specifies five transitions, the fifth being
**Transition 5 — Spare/Heal**: a GC recheck that meets a condemned tombstone with in-degree ≥ 1
**clears the tombstone** (`metaState → absent`). Absent meta means "Clean" in §5, so a spare that clears
the tombstone advertises the blob as Clean while its body still carries the **condemn-time token**.

This is a re-introduction, under the absence-means-Clean representation, of the exact
`clearSparedMeta` "spare = clear (Condemned → Clean)" transition that was **removed on 2026-07-11 as the
Fix-4 data-loss fix** ("GC freshness metadata is ADD-ONLY — only a writer that displaces the body with a
**fresh** incarnation token publishes Clean"). §5 puts the removed hole back.

## Why it loses data {#mechanism}

The safe design relies on: a stale/deposed GC leader's pre-CAS `deleteExact(h, t1)` (issued from a prior
pass's durable `delete_pending` row, keyed off token `t1`) is harmless because any recovery of the
condemned blob's in-degree happens via a **token-displacing** resurrect (`putBlob` → `uploadFromSource` →
fresh token `t2`), so `deleteExact(t1)` finds `TokenMismatch`/absent and is a no-op. **Exact-token delete
only protects against different-token resurrection; it does not protect a same-token reuse.**

§5's Spare/Heal breaks that in two ways:

1. **The spare keeps the original token `t1`** (a spare is an in-degree recovery, not a resurrect — no
   body displacement). After the spare clears the tombstone, the body is Clean-advertised **at `t1`**, and
   a stale deposed leader's captured `deleteExact(h, t1)` deletes the **live** blob.
2. **§5-specific second victim:** with the tombstone cleared to absent, a later `putBlob` dedup-hit
   point-reads absent (= Clean) and **adopts `t1`** instead of resurrecting to `t2` — re-exposing the
   same live token to `deleteExact(t1)`. The add-only rule specifically forced that writer to resurrect.

The premise that in-degree recovers **only** via a token-displacing `putBlob` is false: `Build::adoptEvidence`
(the §4 manifest-trust relink used by `createHardLink`) re-references a condemned blob at its **exact token
`t1`** with no body touch and no `.meta` read (`CasBuild.cpp:664-679`; trusted-with-no-probe at promote,
`CasBuild.cpp:227-234,951-983`). A new committed edge to a condemned hash, no displacement — a
token-preserving recovery. That is the reachable spare that keeps `t1`.

## The gate was false-green {#false-green}

`docs/superpowers/models/CaMetaAbsenceClean.tla` passed the reduced (all-sabotages-off) cfg, but its
`GcSpareHeal` action contains a single unfaithful conjunct — **line 188, `queuedDeletes' = {}`** — that
**atomically cancels every scheduled delete** in the same atom that clears the tombstone (comment: "Drops
any scheduled delete for it"). The real code cannot do this: a spare by one leader cannot cancel a delete
already **captured pre-CAS by a deposed leader** ("the final `gc/state` CAS fences adoption, not pre-CAS
side effects"). The gate closes the hole by construction instead of proving the code closes it — the exact
false-green failure mode the 2026-07-11 report warns about ("a model safer than the code hides real bugs").

**Mechanical confirmation (2026-07-14):** a probe with `GcSpareHeal` made faithful (line 188 replaced by
keeping `queuedDeletes` UNCHANGED) goes **RED on `INV_ABSENCE_NO_QUEUED_DELETE`** in 37 distinct states.
Minimal counterexample:

1. `Seed` → `body = (present, tok=1)`.
2. `GcCondemn` → `metaState = condemned`.
3. `GcScheduleDelete` → `queuedDeletes = {[tok=1, pending]}` (a leader captures the exact-token delete).
4. `Precommit` → writer re-references (in-degree recovers).
5. `GcSpareHeal` → `metaState = absent` while `queuedDeletes` still holds `[tok=1, pending]` over
   `body.tok=1` ⇒ **`INV_ABSENCE_NO_QUEUED_DELETE` violated** (and, one step further, `INV_NO_LOSS`).

The gate also modeled recovery **only** as a token-displacing resurrect; a token-preserving
`adoptEvidence`-style relink edge (new committed edge to a condemned hash, no displacement, no meta read)
is absent from the model.

## Corroboration {#corroboration}

Two independent adversarial audits (the two-model discipline for hard concurrency) converged:

- **Audit A (gate fidelity): FALSE-GREEN** — line-188 conjunct hides the deposed-leader hazard.
- **Audit B (premise refutation): REFUTED** — `adoptEvidence` is the token-preserving recovery path; §5
  adds the second victim; and the existing add-only guard tests
  `SpareLeavesMetaCondemned` + `StaleRedeleteAfterSpareDoesNotDeleteLiveReuse`
  (`src/Disks/tests/gtest_cas_gc_ack_floor.cpp:188-330`) still assert `MetaState::Condemned` after a
  spare — §5's spare-clear was never reconciled with the tests that encode the add-only safety argument.

## Disposition {#disposition}

- **§5 is BLOCKED for the opt-round.** opt-t7's WIP (`CasBuild.cpp`, `CasGc.cpp`, 5 test files) was
  halted **uncommitted**; nothing unsafe landed.
- **If §5 is revived,** it must adopt the 2026-07-11 add-only principle in the absence-means-Clean
  representation: **the spare must NOT clear the tombstone** (drop Transition 5 / `GcSpareHeal`
  entirely). A spared hash keeps its tombstone until a **writer** resurrects it with a fresh token
  (which deletes the tombstone as part of publishing the fresh incarnation) — identical to Fix 4's
  accepted "spared hash stays Condemned until a writer resurrects it" cost. §5 still nets the
  create-time meta-PUT elimination (create writes no meta), which is the actual lever.
- A revived §5 needs a **rebuilt, faithful gate**: a split-action two-leader model (deposed leader with
  a private captured exact-token delete surviving the spare, à la `CaRetiredInRunFoldAbortWitness.tla`)
  **and** a token-preserving `adoptEvidence`-style recovery action, verified GREEN with add-only and RED
  with spare-clear. It also needs the read-path design for distinguishing absent-Clean (live, never
  condemned) from absent-deleted (body also gone) — a `putBlob` dedup-hit must HEAD the body when meta is
  absent — which is the part most needs a fresh brainstorm.

## Separate, pre-existing finding (filed independently) {#adoptevidence}

Audit B surfaced a distinct, **§5-independent** exposure worth its own investigation: `adoptEvidence`
records a **tokenless, bodyless, edgeless** dep during the transaction body (`getView(source)` →
`adoptEvidence`), while the **durable** protecting edge (`stageManifest` + `precommitAdd`) is only
appended at **commit**. `EDGE-BEFORE-OBSERVE` orders `precommitAdd` before **`putBlob`**, not before an
already-recorded `adoptEvidence` dep. If the source ref is dropped mid-relink and GC condemns+graduates+
deletes the blob before the destination's commit writes the edge, the promoted (trusted-no-probe) leaf
dangles. The code's own comments concede the "genuinely-absent adopted blob" case and rely on fsck
detection (`CasBuild.cpp:949-950,964-966`). This must be investigated on its own (reachability of a
source drop mid-relink; interaction with the D4 relink-trust model
[`feedback_cas_relink_trust_model`]), with its own two-model consult — NOT folded into §5.

**Key files:** `Core/CasGc.cpp:379-486,854-905`; `Core/CasBuild.cpp:227-234,294-356,664-679,951-983`;
`Core/CasBlobInDegree.cpp:396-472`; `ContentAddressedTransaction.cpp:264-272,925-941`;
`src/Disks/tests/gtest_cas_gc_ack_floor.cpp:188-330`; model
`docs/superpowers/models/CaMetaAbsenceClean.tla:184-188`; prior report
`docs/superpowers/reports/2026-07-11-cas-deposed-leader-stray-clean-meta.md`.
