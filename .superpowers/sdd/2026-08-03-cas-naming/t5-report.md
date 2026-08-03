# Task 5 report — stateless tests, tag, runner flags, CI configs, workflows

## Step 1 — renames (count re-derived, drifted)
**33** tracked bases (66 files) + the `05008_ca_gc_snap_prune` pair = **68** renames. The plan expected 36
bases / ~74. Same shape, −3 bases; tests were removed since the plan was written. The `.stdout`/`.stderr`
run debris in that directory is untracked and was skipped, as the plan requires.
The special case landed exactly as specified: `05013_system_content_addressed_drop_pool_member.*` ->
`05013_system_cas_drop_pool_member.*` (the generic substitution produces the wanted name).
Zero tracked `content_addressed` filenames remain under `tests/queries/0_stateless`.

## Step 2 — contents
41 files rewritten (gc-log table first, then the catch-all). The plan's mandatory safety re-check,
`git diff | grep -iE 'cast|case|cascad'`, returns **no output**.

## Step 3 — tag and runner flags
`no-content-addressed-storage` -> `no-cas-storage` across 14 test files + the CAS `README.md`;
`tests/clickhouse-test` flags/enums renamed. Three prose leftovers the sed could not reach were fixed by
hand — two of them are `--help` text a user sees ("Run tests with a CAS disk as the default MergeTree
storage").

## Step 4 — the step that found real breakage
`tests/config/config.d/*` renamed and swept; `install.sh`, `job_configs.py`, `functional_tests.py`,
`clickhouse_proc.py` swept.

**The plan's sed silently missed the runner flags, which would have broken every CAS CI lane.** The sed
substitutes `content_addressed` (underscores), but the flags are hyphenated, so
`ci/jobs/functional_tests.py` kept mapping its job options to `--content-addressed-storage` /
`--content-addressed-s3-storage` — flags `tests/clickhouse-test` no longer accepts after Step 3, and which
`tests/config/install.sh` no longer parsed. Every `cas storage` / `cas s3 storage` job would have failed at
startup on an unrecognised argument. Fixed in all three files, and the chain re-verified end to end:
job parameter (`ci/defs/job_configs.py`) -> option key (`functional_tests.py`) -> `--cas-storage` /
`--cas-s3-storage` -> `clickhouse-test` argparse -> `install.sh` case arm. A programmatic cross-check
confirms every job parameter resolves to a known option key (`unresolved: none`).

Also renamed `install.sh`'s internal `USE_CONTENT_ADDRESSED_*_FOR_MERGE_TREE` shell variables (uppercase, so
outside the plan's sed) — 4 occurrences in one file, kept consistent so Task 8's sweep does not trip on them.
`bash -n` on `install.sh`, and `py_compile` on `clickhouse-test` and the three CI modules, all pass.

### The `:303` condition the plan asked me to review by hand — it is correct
`if "s3 storage" in to and "cas" not in to:`. My first read flagged a hazard: `"ParallelReplicas"` contains
the substring `cas` (Repli-**cas**). It does not bite, and the reason is structural rather than lucky: `to`
is a **single option per loop iteration**, not the joined option string (`assert False, f"Unknown option
[{to}]"`, `to.startswith("amd_")`), and the two predicates are ANDed on that same single option — so
`ParallelReplicas` never satisfies `"s3 storage" in to`. Confirmed by enumeration rather than argument: the
only option keys containing `s3 storage` are exactly `s3 storage` and `cas s3 storage`, so the branch
partitions them correctly.

## Step 5 — workflow regeneration
`PYTHONPATH=ci python3 -m praktika yaml` regenerated all four workflows: **191 insertions / 191 deletions**.

**A reading correction worth recording:** `git diff --stat .github/workflows/` came back empty and I first
took that as "no change". It was wrong — praktika `git add`s the files it writes, so the change was already
in the index and only `git diff --cached` showed it. Checked before drawing any conclusion from the empty
diff.

Every one of the 191 lines is a CAS job rename and nothing else: job `name`, `test_name`, the
`praktika run '<job name>'` invocation, and the base64 cache key. The base64 is not taken on faith —
`U3RhdGVsZXNzIHRlc3RzIChhbWRfYmluYXJ5LCBjYXMgczMgc3RvcmFnZSwgcGFyYWxsZWwp` decodes to
`Stateless tests (amd_binary, cas s3 storage, parallel)` and its removed counterpart to the
`content_addressed` spelling. Job ids became `stateless_tests_*_cas[_s3]_storage_*` as the plan predicted.
Nothing outside the CAS jobs changed, so there was nothing to stop for.

## Step 6 — sweep
- The identifier form `content_addressed` in `tests/queries/`, `tests/config/`, `ci/`, `.github/`,
  `tests/clickhouse-test`: **zero**.
- The flag/tag forms `content-addressed-storage`, `content-addressed-s3`, `no-content-addressed`: **zero**.
- 37 hits of the bare English phrase `content-addressed` survive in test comments. The plan's Step-6 grep
  pattern is `content.addressed`, where `.` also matches the hyphen, so it flags this legitimate prose;
  the Global Constraints keep spelled-out English. Reported rather than "fixed".

**One of those 37 is not prose and had to be checked:** `05020_cas_fsck.sh:52` greps the server output for
`"is not a content-addressed disk"`. That message still exists verbatim in `InterpreterSystemQuery.cpp`
(7 sites, spelled-out English), so the assertion still matches. Had Task 3 reworded it, this test would
have gone green-but-vacuous or red; verified rather than assumed.

## Carried in from Task 4
`src/Disks/tests/gtest_cas_settings.cpp:65` now cites
`tests/config/config.d/cas_storage_policy_for_merge_tree_by_default.xml`, and that file exists at the new
path. This is the one `src/` file Task 5 touches.

## Not done here, by design
No stateless run: the spec excludes stateless/integration execution from this effort, and the tree is
deliberately mid-migration until Task 6 (integration configs still carry the legacy keys removed in Task 3).

## Staging
Explicit paths only. `git show --stat HEAD` verified to contain exactly this task's files — a concurrent
agent is committing to `src/Disks/` in the same worktree and index.
