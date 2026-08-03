# T7 lane B report — Task 10c runner closure + 10f disclosure

Scope: `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md` Task T7, Lane B
(`run_gclease.sh`, `run_gcshardincarnation.sh`, `run_gcrounddefer.sh`, `run_b140danglemerge.sh`
pinned + recorded) plus the 10f `UniverseAuthoritative` disclosure.

## Jar preflight

`sha256sum "$(readlink -f tmp/tla2tools.jar)"` →
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3` (resolves to
`tmp/tla2tools-official.jar`). Matches the pin; done before any TLC invocation.

## Step 1 — pin the three runners

Added `source ./tlc_temporal_gate.sh` + `check_tlc_pin "$JAR" || exit 3` to `run_gclease.sh`,
`run_gcshardincarnation.sh`, `run_b140danglemerge.sh`, copying the exact shape from
`run_gcrounddefer.sh`.

## Step 2 — runner results

All four 10c runners ran to completion, one worker each (`-workers 1`), TLC
`2026.07.18.145032` (rev `30cc360`), jar SHA-256
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.

| Runner | Config | Expect | Result | Seconds | Verdict |
|---|---|---|---|---:|---|
| `run_gclease.sh` | `sab_noheartbeat` | violation | `NoFalseSteal` violated | 0 | PASS |
| `run_gclease.sh` | `heartbeat` | green | green | 0 | PASS |
| `run_gclease.sh` | `safety_noheartbeat` | green | green | 0 | PASS |
| `run_gcshardincarnation.sh` | `sab_deletebeforefold` | violation | `INV_NO_ORPHAN_EDGE` violated | 1 | PASS |
| `run_gcshardincarnation.sh` | `sab_incarnationreuse` | violation | `INV_NO_DANGLING` violated | 2 | PASS |
| `run_gcshardincarnation.sh` | `sab_newbornnofloor` | violation | `INV_NO_DANGLING` violated | 2 | PASS |
| `run_gcshardincarnation.sh` | `sab_pathkeyedcursor` | violation | `INV_NO_DANGLING` violated | 2 | PASS |
| `run_gcshardincarnation.sh` | `design` | green | green | 85 | PASS |
| `run_gcrounddefer.sh` | `sab_graduate_on_stale` | violation | `NoOverDelete` violated | 0 | PASS |
| `run_gcrounddefer.sh` | `sab_unbounded_defer` | temporal | `EventuallyFolded` violated | 1 | PASS (after fix, see below) |
| `run_gcrounddefer.sh` | `stage1` | green | green | 0 | PASS |
| `run_gcrounddefer.sh` | `witness_deferthenfold` | violation | `W_DeferThenFold` violated | 1 | PASS |
| `run_b140danglemerge.sh` | `m_both_buggy` | violation | `INV_NO_LOSS` violated | 10 | PASS |
| `run_b140danglemerge.sh` | `m_cursorskip` | violation | `INV_NO_LOSS` violated | 12 | PASS |
| `run_b140danglemerge.sh` | `m_trimonly` | violation | `INV_NO_LOSS` violated | 5 | PASS |
| `run_b140danglemerge.sh` | `m_merged` | green | green | 71 | PASS |

Logs: `build/t7b_10c_gclease.log`, `build/t7b_10c_gcshardincarnation.log`,
`build/t7b_10c_gcrounddefer.log` (first run, red) + `build/t7b_10c_gcrounddefer_rerun.log`
(post-fix, green), `build/t7b_10c_b140danglemerge.log`.

## Deviation: a classifier bug found and fixed in `run_gcrounddefer.sh`

**Not in the brief's file list** (the brief named it only as the reference shape to copy, already
assumed correct) — but the first run of the 10c battery produced a red row I could not paper over
per the standing rule ("a red row = STOP and report").

`run_gcrounddefer.sh`'s `sab_unbounded_defer` row expects a `temporal` verdict and classified as
`error` instead. The underlying TLC log
(`tmp/tlc_CaGcRoundDeferCore_sab_unbounded_defer.log`) shows the checker actually found the
correct counterexample: `Error: Temporal property EventuallyFolded was violated.` — the same
11-state lasso trace already narrated in `CaGcRoundDeferCore_RESULTS.md`'s
`sab_unbounded_defer` section. The classifier's `elif grep -q "Temporal properties were
violated"` only matches the generic plural message; every other `tmp/tlc_*.log` in the tree that
hits this branch was produced by the older, no-longer-accepted TLC 2.19 (rev `5a47802`), which
prints exactly that generic form. The pinned official jar (`2026.07.18.145032`) instead names the
one declared `PROPERTY` when there is only one, printing the singular
`"Temporal property <Name> was violated."` — a message form the classifier had never been
exercised against under the pinned jar.

Fix: widened the pattern to
`grep -qE 'Temporal propert(y|ies)( [A-Za-z0-9_]+)? (was|were) violated'`, which matches both
forms (verified against both exact log lines before editing). Re-ran `run_gcrounddefer.sh`
end-to-end after the fix — the row now resolves to `temporal:EventuallyFolded` and the whole
suite is `ALL EXPECTATIONS MET` (`build/t7b_10c_gcrounddefer_rerun.log`). This is a load-bearing
mutation demonstration performed after implementation; mutation reverted; patch and failing
output preserved (`build/t7b_10c_gcrounddefer.log` is the pre-fix red, kept alongside the
post-fix green).

This is disclosed here rather than silently folded in because it touches a file outside my
assigned Files list; the fix itself is a one-line classifier regex widening, not a model or
production change, and TLC's actual verdict (the violation, the invariant name, the trace) was
correct throughout — only the shell script's string match was stale.

## Step 3 — RESULTS artifacts

- Created `CaGcLeaseCore_RESULTS.md` and `CaB140DangleMerge_RESULTS.md` (house shape: frontmatter,
  `{#anchors}`, checker identity, per-config table, sabotage narrative, verdict, reproduction
  command — modelled on `CaGcDestructiveGateCore_RESULTS.md`).
- Extended `CaGcRoundDeferCore_RESULTS.md` and `CaGcShardIncarnationCore_RESULTS.md` with a dated
  `## 2026-08-03 — runner-suite run` section each, recording this run's table, checker identity,
  and — for `CaGcRoundDeferCore` — the classifier-bug finding and fix.
- `design`/`m_merged`/`stage1` green distinct-state counts matched their prior recorded runs
  exactly (BFS explores the full reachable space regardless of checker version or worker count);
  sabotage counterexample counts differ run to run by construction (BFS reports the first-found,
  not the shortest, counterexample) and are noted as such rather than treated as a discrepancy.

## Step 4 — prose fixes (10c closure content)

- `docs/superpowers/models/README.md`: the four `(inline TLC)` summary-table rows for
  `CaGcLeaseCore.tla`, `CaGcShardIncarnationCore.tla`, `CaGcRoundDeferCore.tla`, and
  `CaB140DangleMerge.tla` now name their runners.
- `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md`'s `{#fix-runners}` closing paragraph
  ("Three further models have no runner at all …") rewritten to record that all four now have
  asserted whole-suite runners, each pinned via `check_tlc_pin`, with results in their own
  `*_RESULTS.md` files.

## 10f disclosure

- `CaGcDestructiveGateCore_RESULTS.md`: one paragraph added after the code-correspondence section
  stating `UniverseAuthoritative` is pinned `TRUE` because the model gates only the post-flip
  posture; the opposite posture (universe not yet knowable → suppress everything) is production's
  current default and is covered directly by the C++ test
  `CasGcFrontierGate.HiddenPlusOneInAnUnknownNamespaceIsRefusedByTheProductionDefault`.
- `CaGcDestructiveGateCore.tla`: a matching short comment above `UniverseAuthoritative == TRUE`
  stating the same reason (no task/plan reference, per the comment policy).

## Commit

One commit, `ca: tla — 10c runners pinned and recorded; runner table corrected`, containing: the
three pinned runners, the `run_gcrounddefer.sh` classifier fix, the two new RESULTS files, the two
extended RESULTS files, the README + `{#fix-runners}` prose fixes, the 10f disclosure (RESULTS +
`.tla` comment), and this report — via `git add -f` for the paths under
`docs/superpowers/models/` and `.superpowers/sdd/...`.

## Concerns / notes for review

- The `run_gcrounddefer.sh` fix is a deviation from the literal file list in the brief; disclosed
  above with full reasoning. No production or model semantics changed — only a shell classifier
  regex.
- Did not touch `run_gc_partmanifest.sh` or any `CaGcRootLocalPartManifestCore*` file (lane A's
  territory) — verified via `git diff --stat` before committing that its pre-existing dirty state
  is unchanged by my edits.
- Left the numerous untracked `*_TTrace_*.tla` debris files in `docs/superpowers/models/`
  (produced by this run and by concurrent lane-A/other-session runs) alone — not part of this
  commit, not deleted.
