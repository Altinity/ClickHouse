# T8 early-phase report — Stage-B gate skeleton, residual row, gate totals {#t8-early-report}

Worktree: `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`. Commits:
`44db0d3739` (item-4 test + T2-F1/F2 probes + comment-policy fixes), `0f7b755bb16` (RESULTS
skeleton + residual row + gate totals).

## Scope covered

Steps E1–E4, the residual-cleanup gate row (all 7 items), and Step 1 (full CA gate, both builds).
The battery/soaks (integration lanes, four required soaks, Step-3c/3d, verdict) are explicitly
**not** in scope for this early phase — they wait for the controller's orchestration once T1–T7's
predecessors (all COMPLETE per the ledger) are confirmed and the two-worktree resource rules allow
it.

## Step E1 (tidy)

Already DONE end-to-end by a prior session (ledger: draft `b0f87e8aaf1` + closure `c0d2cad0dfd`,
119 diagnostics resolved). Not re-run; the user separately cancelled the planned FINAL tidy
re-run (CI/CD's tidy job is now the gate). Recorded here only for completeness — no action taken.

## Step E2 — executable-prose sweep

I did **not** reuse an earlier draft pass (`draft/t8` branch) that had grepped the whole current
file content (351 hits, exhaustively unclassified). Instead I grepped the actual Stage-B diff
against the plan's own named baseline (`ce312f547c3`, the old-plan Task-0 commit), on ADDED
comment lines only — this is what the plan's Step E2 text literally asks for ("Grep the Stage-B
diff … for … in comments"), and it is far more tractable: **12 hits** total (`whenever`: 2,
`always`: 10, `must also`: 0, `in the same change`: 0), all individually read and classified in
the RESULTS doc's `{#e2-executable-prose-sweep}` table. Every hit is either local (explained by
code in the same function/file), self-testing (asserted by a test in the same file), or
type-enforced (`NamespaceLifeId`'s construction contract makes the `CasFsck.cpp:514` cross-file
claim a compile-time guarantee, not a convention). No hit matched CLAUDE.md's target pattern (an
unenforced cross-cutting rule stated only in prose); no BACKLOG entry was added.

**Deviation from the plan's literal Task-0-baseline framing, disclosed:** I used `ce312f547c3`
(the commit the ledger's `{#baseline}` section and the plan's own `{#authority}` paragraph both
name as the audit baseline) rather than re-deriving a different commit — this matches what "old
plan Task 0 baseline" means operationally in this ledger.

## Step E3 — report skeleton + suite inventory + specimen directory

Created `docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md` (dated for real, not `2026-08-XX`).
Contains the battery table (gate rows filled with real numbers; integration/soak rows still
placeholders for the battery stage), the six-result-criteria table (placeholders), the Step-3c
cost-inventory table with drafted SQL (placeholders — needs a live soak), the E2 sweep write-up,
the suite inventory, the E4 soak-command pinning, and the full residual-row walk.

Suite inventory: `utils/cas-gate/generate_cas_suites.sh build` → 278 suites (static enumeration,
4 excluded, 0 unclaimed, `build/t8_gatecheck.log`). The REAL gate runs report slightly different
counts — 279 (release) and 297 (ASan) — because `--gtest_filter='CAS*:Cas*'` also matches
`TEST_P` parameterized instantiations that the script's static suite-name regex does not
separately enumerate; this is explained in the RESULTS doc, not a discrepancy to chase.

Specimen directory: `utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/` created with a
`smoke/` subdirectory, both holding `.gitkeep` placeholders. **Note:** `utils/ca-soak/logs_archive/`
is gitignored — the directories exist on disk for the battery/soak stage to write into, but are
not (and should not be) tracked by git; I did not force-add them.

## Step E4 — soak command pinning

Enumerated all registered scenarios via `python3 -m scenarios.run --list` (the plan-mandated
command, not a file-read survey) — **S01–S45**, `build/t8_scenario_list.txt`. Both cards T8's soak
runs (b)/(c) require — S44 (rebirth with concurrent namespace-file mutation readers/writers) and
S45 (decommission with hidden `Removing` catalog entries) — **already exist and were validated
live** earlier in the campaign (S44: 2026-08-03, 6 cycles, seed 1; S45: 2026-08-03, seed 4, after
a live run found and fixed five real gaps: the mount-lease-wait bound, both-replica table
create/drop, and the `_CLICKHOUSE_DISKS`/`ca_ro` disk invocation shape). **I did not touch the
`draft/t8` branch's older, pre-fix versions of these cards** — a direct diff (`cas-gc-rebuild` vs
`draft/t8`) showed the draft branch's S44/S45 predate the five live-run fixes; the current tree's
versions are strictly better and already integrated.

Pinned the four soak commands (paired smoke-then-full, staged per the 2026-08-03 user directive)
into the RESULTS doc's `{#e4-soak-commands}` section — seeds `20260804` (smoke) /`20260805` (full),
default compose variant, worktree left as `<WORKTREE>` (the controller's decision).

## Residual gate row — all 7 items walked

Full table in the RESULTS doc's `{#residual-gate-row}`. Summary by disposition:

- **Fixed (this task, committed):** item 4 (the `casPut`-throw regression test), item 5's two
  named comment citations, plus one NEW stale citation I found while auditing the same file
  (`CasNamespaceLifeId.h`'s "Stage B Task 6 adds `fromLiveHandle`" — never built; T1a's
  classification kept all ten reads as-is). Also fixed as part of the same slice: T2's F1/F2
  residual debts (backoff-cap-from-below and reset-restarts-schedule probes), both proven
  red-first via a genuine temporary production mutation (captured, then reverted).
- **Verified, no action needed:** items 1a–1f (unlocatable in the tree, counts as fixed per the
  plan's own rule — one caveat disclosed for 1b, a live "spells" grep found unrelated text),
  MINOR-B (discharged; symbol is `recoverRefTableDetailedFromAuthority`), NITs C–F
  (historical-unrecoverable per the midpoint audit), T2-F4 (already covered by
  `RefWriterChunkedFlush.SnapshotPublisherLatchedAcrossChunks`, no duplicate test added).
- **Accepted, recorded with reason, no action:** item 6 (Q-2 ABA-edge sequencing — the ordering
  argument is already structural, no comment change needed), item 7 (10b sharding-arm debt,
  carried to the post-B list), T4 TEST-1 (design sketched in the RESULTS doc, not applied — the
  real-round fold call sites would need a genuine behavior-preserving split, judged too large for
  this early-phase pass; a concrete next step is written down).

**Verdict counts: 4 fixed, 6 verified/no-action, 3 accepted-with-reason** (10 named line items
covering all 7 plan-numbered slots — several plan items bundle multiple sub-findings).

## Step 1 — full CA gate, both builds

Ran under the standing ONE-HEAVY-STAGE-AT-A-TIME rule (release build → release gate → ASan build
→ ASan gate, strictly sequential, `flock` held throughout; never overlapped with any other stage).
An earlier attempt at the release gate died silently mid-run (background process vanished with no
crash message, ~10 min in) — restarted with `setsid` for full detachment; the second attempt ran
clean to completion, so this looks like a harness/session-detachment issue, not a test defect. No
build edits were needed: the pre-existing release binary (built fresh by the dispatch before I
started) and the ASan binary (rebuilt once, before my test edits, then again after) were both
already current.

**Results:**

| Build | Baseline (T1-lane closure) | This run | Suites | Exit |
|---|---|---|---|---|
| release | 1977/1977, 277 suites | **2007/2007** | 279 | 0 |
| ASan | 1982/1982, 295 suites | **2012/2012** | 297 | 0 |

Delta (+30 release / +30 ASan tests, +2/+2 suites over the T1-lane baseline) is explained line by
line in the RESULTS doc's battery table: T2 (ordering/poison/backoff suite), T3 (fence-arm
rename+add, fsck grammar), T4 (duty-queue/orphan-nomination), T5 (probe-A deletion, net −3), T6/T6b
(destruction-enablement + 9 budget-setting tests), the shortkeys/obscure-names/naming-sweep
mechanical renames (~0 net), and this task's own +1 test. Cross-checked against the shortkeys
agent's measurement 35 minutes earlier (2006/2006 release, 2011/2011 ASan, same 279/297 suite
counts) — this run's +1/+1 is exactly this task's new `CasPutExceptionPropagatesAfterMandatoryResolution`
test, landed in an existing suite (no new suite).

## Deviations disclosed

- Used the `CAS*:Cas*` combined gate filter (matching the shortkeys agent's immediately-preceding
  precedent), not a pure `Cas*` filter — the plan's `{#global-constraints}` describes a
  suite-naming normalization to a single `Cas` prefix that **has not actually landed**: the tree
  still mixes `CAS*` (majority) and `Cas*` (files created after some later renames), and
  `utils/cas-gate/generate_cas_suites.sh` currently enforces the `CAS*` invariant, not `Cas*`.
  Flagging this because it is a real gap between what the plan's global-constraints section
  describes and what is in the tree — whoever eventually runs that normalization sweep needs to
  also update the invariant script.
- Did not run the integration lanes, the four required soaks, the Step-3c cost inventory, or the
  six result criteria — all explicitly deferred to the controller's battery-stage orchestration
  per the dispatch's own scoping ("the battery/soaks come later under my orchestration").
- Did not start any praktika or soak run myself, per the standing serialization rule relayed
  mid-task.

## What remains for the battery phase

1. Integration lanes (the CA selector set named in T6 Step 3 plus whatever the suite inventory
   lists beyond it).
2. The four required soaks (churn, S44 rebirth, S45 decommission, 90-minute general), each
   smoke-then-full per the pinned commands in the RESULTS doc.
3. The Step-3c cost inventory and the six result criteria, both measured off the soak's
   `system.content_addressed_garbage_collection_log` rows.
4. The specimen preservation (Step 3f) and the insert-path guard (Step 3e).
5. The four ex-known-red stateless tests, to be run green under their current names
   (`05008_cas_gc_snapshot_prune`, `04290`/`04295`, `05010`).
6. The final results file completion (battery table's integration/soak rows, the verdict line).
