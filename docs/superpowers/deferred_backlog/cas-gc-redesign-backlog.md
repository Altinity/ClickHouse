# CA GC Redesign — Backlog (findings / ideas / sidetracks)

Captured during the autonomous execution run (see `../cas-gc-unattended-execution-log.md`). Items here
are deferred-not-dropped: protocol nits, refactors, test gaps, ideas, and sidetrack findings that should
not block the current phase. Each item: ID, severity, where, what, why-deferred.

---

## Open

- **B1 — Phase-0 model journal record naming (cosmetic).** Severity: low. Where:
  `docs/superpowers/plans/2026-06-26-cas-gc-phase0-tla-model.md` (and `…-phase2-token-diff-discovery.md`
  references it). The model names the root-journal record `OwnerTransition` (a `[ver, ref, old, new]`
  tuple); per spec rev. 15 the protocol/C++ name is `RootOwnerEvent`. The model record is *semantically*
  the rev.15 event (owner-move dispatch via equal old/new manifest is modeled), so this is a naming nit,
  not a semantic gap. Deferred: renaming would cascade into the phase2 model reference; defer to a
  terminology-only pass once the model is green, to avoid churn during the R0 gate.

## Resolved

(none yet)
