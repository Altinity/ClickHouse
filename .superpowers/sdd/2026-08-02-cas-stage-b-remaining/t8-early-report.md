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

## Battery phase (Step 2, executed after this report's first version) {#battery-phase}

Commits: `da09c996104` (integration batch A), `5b36eeda55c` (batch B), `43aff19ed51` (stateless
four), `44db0d3739` covers item-4/F1/F2 already, and separately `c9147d312bd` +
`5db304f1566` (the sharded-GC arc). Full detail and all logs live in
`docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md`'s `{#integration-lane-results}` and
`{#ex-known-red-stateless}` sections; this section adds the narrative trail for the one lane that
took real work.

**Integration lanes**: 10 `test_cas_*` dirs run in two batches of 5, strictly sequential, never
overlapping any other heavy stage (build/gate/praktika/soak) per the standing box rule. Batch A:
6 passed, 1 skipped (`test_cas_gc_sharded`, see below). Batch B: 17 passed, 0 failed.

**Ex-known-red stateless four**: `05008_cas_gc_snapshot_prune`, `04290_cas_no_leftovers`,
`04295_cas_mutation_no_leftovers`, `05010_cas_mounts_gc_health` — 4/4 passed via the CAS-default
`Stateless tests (amd_binary, cas storage, parallel)` job, one invocation.

**`test_cas_gc_sharded` — the campaign's no-skip rule applied.** A CA test skipped as needs-infra
is not an acceptable "no action": it either runs, or becomes a named, placed return-item. This one
had a cheap path — every other CAS integration test already uses RustFS (not MinIO) for exactly
the conditional-delete capability this test's own skip reason named as missing — so option (a) was
pursued rather than filing a return-item. Six-run RCA arc, in order:

1. **Run #1** (rustfs/minio swap only, `gc_sharded_rustfs.log`): red in **13s**, at the very first
   query — `KeyError: 'replica'`. RCA: a pre-existing Python `.format()`/ClickHouse-macro
   brace-escaping bug (`'{replica}'` instead of `'{{replica}}'`) in `live_table`'s `CREATE TABLE`,
   latent since the module has been permanently skipped since it was written. Not caused by the
   rustfs swap. Fixed (2 sites); the correctly-escaped form already existed elsewhere in the same
   file to copy from.
2. **Run #2** (`gc_sharded_rustfs2.log`): red in the real test body — "GC did not complete even
   one generation within 90.0s". Deep RCA required per the controller's branch-3 concern (real GC
   defect vs stale premise vs config gap). Found: the test polled S3 keys named `completion_seal`
   under `gc/gen/<generation>/` — `grep -rn completion_seal src/` returns **zero hits anywhere in
   production**; the real key is `fold_seal`, and the real path additionally has an
   `attempt/<attempt>/` segment the test's prefix omitted entirely
   (`Layout::gcGenAttemptPrefix`/`foldSealKey`/`blobTargetRunKey`, `CasLayout.h`). Confirmed
   branch (1), stale test premise, not a GC defect, with hard evidence (a zero-hit grep, not an
   absence claim).
3. Authority for the rewrite, verified before writing any code: `Gc/CasOrphanManifestSweep.cpp`'s
   `readAdoptedFoldSeal` — read `gc/state`, take `(snap_generation, snap_attempt)`, GET that exact
   `foldSealKey`. `gc/state`'s wire format (`CasGcStateFormat.cpp`) is a JSON-like text object with
   fields literally spelled `"sg"`/`"sa"`, so a direct regex read from Python is exact without a
   C++ decoder.
4. **Run #3** (`gc_sharded_rustfs3.log`, first B/C rewrite): red again, but at the SAME latent-bug
   class as run #1 — `Code: 60, Unknown table 'system.text_log'` right after
   `node2.restart_clickhouse(kill=True)`. A second independent pre-existing bug (the underlying
   table takes a moment to become queryable post-restart), also latent forever. Fixed with
   `query_with_retry` (an existing helper, not a new mechanism).
5. **Run #4** (`gc_sharded_rustfs4.log`): the B/C rewrite reached the real assertions for the
   first time and failed on substance — "node2 has CA fatal/error entries matching 'DANGLE',
   6 == 0". Deep RCA (no fix attempted before reporting, per the controller's regime-change
   instruction once mechanics stopped being the failure mode): pulled all 6 lines verbatim from
   `_instances-gw0/node2/logs/clickhouse-server.log` (read via a throwaway root container since
   the docker-created directories are not otherwise readable). All 6 were `Code: 60. Unknown table
   'system.text_log'` errors from the RETRY loop itself, echoing the failing query's own text
   (which contains the literal substring `%DANGLE%`, the LIKE pattern) — a self-referential
   artifact: `query_with_retry`'s failed attempts logged `<Error>` lines that, once `text_log`
   became queryable, were themselves counted as "DANGLE" matches by the very query that had just
   started succeeding. Zero real dangle-shaped content found anywhere (grepped the log and the raw
   MergeTree part data directly). Classified and reported before touching code, per instruction.
   Fixed: split a keyword-free readiness probe (via `query_with_retry`) from the real keyword
   checks (plain `.query()`, no retry, run only once the table is confirmed queryable).
6. **Run #5** (`gc_sharded_rustfs5.log`): text_log fix worked, assertion B passed for the first
   time. Assertion C failed: "no blob_target keys found ... generation=8 attempt=40". RCA: the
   carry-forward argument in the original C design was inverted — a seal's `blob_target_runs`
   carry a PARENT's runs forward as REFERENCES, but the physical objects stay under the
   `(generation, attempt)` prefix where they were originally WRITTEN; a late-soak adopted attempt
   whose own round produced no new deltas can have an empty `blob_target/` prefix of its own.
   Rewrote assertion C to scan the whole `gc/gen/` subtree (every generation/attempt) rather than
   only the currently-adopted pair — matching the original test's actual intent ("both shards
   produced runs somewhere over the soak") without needing to decode any seal body.
7. **Run #6** (`gc_sharded_rustfs6.log`): **green — 1 passed in 78.01s.**

None of the four bugs found were in production code; all four were pre-existing, latent test-only
defects in a module that had literally never executed before this task (permanently
`pytest.mark.skip`d since it was written). The B/C rewrite's structural correctness rests on two
verified production facts, not assumption: `readAdoptedFoldSeal`'s two-hop `gc/state` → seal
lookup (the authority for "adopted"), and `blob_target_runs`' carry-forward-by-reference semantics
(why C needs the whole subtree, not one attempt).

**Deviation disclosed**: the anti-vacuity requirement (log the observed key set on the first green
run) was implemented as a `print()` before the assertion that consumes it — genuinely executes,
proven by the pass — but its captured stdout was not preserved in this harness's per-test log
artifact for a PASSING test (`--report-log-exclude-logs-on-passed-tests`, a `pytest` invocation
flag this job's `ci/jobs/integration_test_job.py` sets, not something this task's scope covers
changing). The pass itself (both C sub-assertions succeeded) is the evidence the listing was
non-empty and covered both shards; the literal key list was not captured as an artifact.

## What remains for the battery phase (soaks) {#remaining-soaks}

The controller (team lead) orchestrates the soak stage from here, per the plan's task ownership.
Remaining, not started by this task:

1. The four required soaks (churn, S44 rebirth, S45 decommission, 90-minute general), each
   smoke-then-full per the pinned commands in the RESULTS doc.
2. The Step-3c cost inventory and the six result criteria, both measured off the soak's
   `system.content_addressed_garbage_collection_log` rows.
3. The specimen preservation (Step 3f) and the insert-path guard (Step 3e).
4. The final results file completion (soak rows, the verdict line).
