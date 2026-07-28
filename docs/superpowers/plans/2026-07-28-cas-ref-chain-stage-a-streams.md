# CAS ref contiguous streams — Stage A (streams) Implementation Plan

> **DRAFT MARKER: being elaborated — do not execute until this line is removed.**

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the LIST-incompleteness release blocker for existing namespaces: per-namespace
contiguous ref ids, the in-band `EpochSeal`, `_ckpt`, arithmetic fold + recovery, the
destructive-round frontier proof and REBUILD-surviving holds — with LIST demoted to a zero-trust
hint on every consumer this stage touches.

**Architecture:** Spec `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`
(v9 CONVERGED), INV-1/INV-2/INV-4 + §4 recovery + §5 fold/holds/frontier + §6 sweep premise +
§7 REBUILD/fsck. Stage B (catalog + incarnations, INV-3) is a separate plan
(`2026-07-28-cas-ref-chain-stage-b-catalog.md`) with its own soak gate; this stage is fully
functional without it. TLA gate: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` says
`TLA PHASE: PASS` (93/93) — models are normative for every mechanism named below.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`),
gtest (CA gate filter), integration lanes (`with_rustfs`), `utils/ca-soak` (phase 3 `--duration`),
TLA+ models under `docs/superpowers/models/` (already green; extended only by register items).

## Global Constraints {#global-constraints}

Copied from the spec, the project instructions and the standing user directives — every task's
requirements implicitly include ALL of these:

1. Branch `cas-gc-rebuild`; new commits only (no rebase/amend); **NEVER `git push`**.
2. Allman braces; never `sleep` to fix a race; wrap literal names in backticks in docs/comments;
   say "exception" not "crash" for logical errors; ASan not ASAN.
3. **Fail-close, no fallback paths**: when an operation fails, propagate; never substitute a
   default. A defensive branch whose premise Stage A kills is REMOVED and replaced by a loud
   check (see Task 12), never left "just in case".
4. **Recreate-only migration, zero compat scaffolding** (pre-release): Stage A bumps the pool
   format writer generation AND backward floor once (Task 3); old-format open fails closed naming
   pool recreation; no dual-format readers, no upgrade converters.
5. CAS identity primitives are untouchable: never GET a condemned object to revive it (revival =
   fresh re-upload); no skip-read hash-equality shortcuts (re-hashing is the identity primitive);
   the HEAD-before-PUT dedup protocol steps are not "optimization" targets.
6. No CA-specific fields or hooks in generic Replicated/Keeper code or formats; changes to
   shared/upstream surfaces require consultation BEFORE editing.
7. Wire formats changed here (`CasRefLogFormat`, `CasFoldSealFormat`, `CasRefSnapshotFormat`)
   follow the strict-grammar rule: fields required in exactly the states that permit them,
   forbidden otherwise, rejected loudly on violation; every codec change lands with
   boundary AND boundary-plus-one tests (spec r9-4).
8. The append hot path gains **+0 requests**; recovery/seal/`_ckpt` costs are per spec §8 and a
   task exceeding them is out of spec (raise, do not land).
9. Any register item (R2/R3/…) landed later extends its TLA model per spec §9 — Stage A tasks do
   NOT edit the five phase models; model edits in Stage A are forbidden (the gate stays sealed).
10. Every new externally-visible failure mode gets a `LOG_WARNING`-or-stronger message naming the
    object key and the decision taken (the CI-observability lesson: unlogged decisions block
    triage).
11. Tests: prefer NEW test files over extending existing ones; stateless tests via
    `./tests/queries/0_stateless/add-test`; no `no-*` tags unless strictly necessary; gtests run
    under the CA gate filter (exact filter recorded in Task 0).

## Staging contract {#staging-contract}

**Stage A delivers standalone:** contiguous ids + seals + `_ckpt` at TODAY's un-qualified key
shape (`<ns>/_log/...`, `<ns>/_snap/...`, `<ns>/_ckpt`); namespace DISCOVERY still uses today's
mechanisms (hint enumeration + `gc/state` cursors) — but every per-namespace decision (fold
advance, deletion safety, recovery completeness) becomes arithmetic/point-read/CAS.

**Named Stage-A residuals (closed by Stage B, honest until then):**
- A namespace absent from BOTH the hint and `gc/state` has no frontier entry (the universe is not
  yet authoritative — INV-3's job). New-namespace creation is DDL-rate; the observed blocker class
  (existing-namespace hidden records) is closed by Stage A.
- Verbatim-file rebirth aliasing (register R1) — pre-existing, untouched here.
- Manifest-less orphan blobs after REBUILD goes condemn-nothing (register R4) — a NAMED leak, not
  a regression: today's REBUILD condemnation of acked data is the thing being removed.

**Interface handed to Stage B:** the `EpochSeal`/`_ckpt` codecs and the slot-occupy primitive are
namespace-shape-agnostic (they take full keys); Stage B re-keys the layer under `<ns>/<inc>/` and
swaps discovery to the catalog without touching Stage A's arithmetic.

---

## Task overview {#task-overview}

| # | Task | Spec | Depends on |
|---|---|---|---|
| 0 | Gate + baseline preflight | §9 | — |
| 1 | `EpochSeal` record kind + grammar | INV-2 | 0 |
| 2 | Slot-occupy backend primitive | INV-2 | 0 |
| 3 | Contiguous allocator; delete `next_ref_sequence`; format bump | INV-1 | 0 |
| 4 | Writer wedge: every-attempt rule + admission fence | INV-1/INV-2 | 1,2,3 |
| 5 | `_ckpt` object + shared semantic-max merge + fence recheck helper | INV-4 | 0 |
| 6 | Recovery CAS-walk + install-lock generation recheck; retire sentinel seal | §4 | 1,2,4,5 |
| 7 | Fold arithmetic intake; seals cross epochs; B1/B2 accounting | §5 | 1,3 |
| 8 | Holds: classification-4 grammar, REBUILD carry, refuse-on-missing-seal | §5/§7 | 7 |
| 9 | Frontier proof + `suppress_destructive` before every destructive site | §5 | 7,8 |
| 10 | Sweep §6 deletion premise (two rules; retain on uncertainty) | §6 | 7,9 |
| 11 | REBUILD condemn-nothing; fsck arithmetic streams | §7 | 7,8 |
| 12 | Retirement sweep (probe A demotion; grace/gap-heuristic/reconciliation verdicts) | §5/R7 + ledger | 6,7,9 |
| 13 | LIST-liar fault injection + end-to-end blocker regression test | §1/§9 | 4,6,7,9 |
| 14 | Stage A gates: gtest + integration + soak; stage summary + PASS line | §9 | all |

Tasks 1, 2, 3, 5 are independent after 0 — but per SDD discipline they are still executed
SEQUENTIALLY (never two implementers in one worktree).

---

*(Tasks 0-14 elaborated below — each with Files / Interfaces / bite-sized steps and code. The
elaboration is being appended; the DRAFT marker at the top comes off when the self-review passes.)*
