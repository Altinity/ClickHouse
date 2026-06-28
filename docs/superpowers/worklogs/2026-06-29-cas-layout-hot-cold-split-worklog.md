# Work log — CA layout hot/cold split (server_root_id identity, mount safety, relocation, sweep, ordinal)

Spec: `docs/superpowers/specs/2026-06-28-cas-layout-hot-cold-split-design.md` (rev5, approved).
Branch: `cas-layout-hot-cold-split` (off `cas-gc-part-manifest-impl` @ `a0c21fe`).
Mode: **fully unattended**, subagent-driven TDD, **codex CLI (`codex exec --sandbox read-only`, gpt-5.5) as reviewer** (fall back to self/subagent review if codex is unusable). Don't stop, don't ask.

## Ultimate goal (user directive 2026-06-29)
1. Deliver the FULL spec via the ritual: per-phase plan → review → subagent-driven TDD impl → final review. Phases: 0 (mount safety — plan done) → 1 (relocation) → 2 (cursor sweep) → 3 (writer_epoch rename + manifest_ordinal).
2. Backlog grooming: read `docs/superpowers/deferred_backlog/`, mark done/resolved/obsolete by last-changed, move to archive; partial → keep only the remainder, archive the rest. No noise.
3. Run `utils/ca-soak/scenarios/`, collect observations; obvious problems → fix, non-obvious → backlog.
4. Start the 4h soak.

## Environment
- codex CLI `codex-cli 0.142.2`, `codex exec --sandbox read-only "<prompt>"` confirmed headless (rc=0).
- deferred_backlog: `cas-gc-redesign-backlog.md` (41K, 2026-06-27), `cas-mergetree-integration.md` (207K, 2026-06-26), `cas-mergetree-integration-archive.md` (352K, 2026-06-26).
- Phase-0 plan: `docs/superpowers/plans/2026-06-29-cas-layout-phase0-mount-safety.md` (8 tasks; Task 1 = TLA+ CaCasMountCore gate).

## Timeline

### 2026-06-29 — kickoff
- Created branch `cas-layout-hot-cold-split`. Verified codex headless. Worklog started.
- Starting **Phase 0** subagent-driven: Task 1 (TLA+ gate) → Tasks 2-8 (TDD, codex review between).
