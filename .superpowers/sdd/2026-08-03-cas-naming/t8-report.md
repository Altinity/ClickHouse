# Task 8 report — final sweep and full gate

## Step 1 — full-tree sweep, every line reviewed
`git ls-files | grep -v docs/superpowers|.superpowers | xargs grep -nE 'content[_ -]addressed|…' | grep -v 'ContentAddressed[A-Za-z]*'`
-> **270** lines. Classified by form rather than read as one list, because the three forms have different verdicts:

| form | count | verdict |
|---|---|---|
| hyphenated English `content-addressed` | 229 | legitimate — the spelled-out prose the Global Constraints keep |
| snake_case `content_addressed` | 40 | reviewed individually; **7 were real defects** (below) |
| `CONTENT ADDRESSED` (SQL) | 1 | in a foreign file, reported not edited (below) |

### Real defects found and fixed
1. **A RED TEST.** `utils/ca-soak/scenarios/tests/test_render_tuned_config.py:20` asserted
   `"<metadata_type>content_addressed</metadata_type>" in xml`. Task 3 renamed the value, so this pinned the
   retired spelling. Running the suite confirmed it: **1 failed, 335 passed**. Fixed to `cas`; suite back to
   **336 passed**. The tree was red between Task 3 and here, and nothing before this sweep had caught it —
   the ca-soak suite was last run during Task 1.
2. **`utils/ca-soak/scripts/soak_watch.sh:47`** issued `SYSTEM FLUSH LOGS content_addressed_log` — a table
   that no longer exists. Live tooling, would fail at runtime.
3. `predown_dump.sh` dumped to `content_addressed_log.tsv` and named the table in prose;
   `smoke_relink_validate.sh` and `framework/observe.py` named the retired tables; a scenario label in
   `s15_s18_shards_lifecycle.py` did too. All corrected.
4. `scenarios/README.md`, `scenarios/__init__.py`, `cards/s12_s14_faults.py` documented
   `metadata_type = content_addressed` — the value the server now rejects.
5. **Task 1's file glob never included `programs/`.** `src/*.cpp src/*.h utils/ca-soak/* tests/integration/*`
   omits it, so `programs/disks/CommandFsck.cpp:108` still named the metric `CasGcNamespaceCleanupLeaks`.
   A programmatic re-scan of all of `programs/` against the 165-name registry found this as the **only**
   occurrence — the miss was one comment, no code, which is why every build stayed green.
6. **`src/Disks/.../ContentAddressedSettings.cpp:29`** and **`ContentAddressed/README.md`** still described
   `metadata_type=content_addressed`; the README additionally documented the old config filename, the old
   disk name `<content_addressed>`, old `path`/`scratch_path` values, and the old CI job names
   "`content_addressed storage`" / "`content_addressed s3 storage`". All corrected against the real
   `tests/config/config.d/cas_storage_policy_for_merge_tree_by_default.xml`.
7. **`utils/ca-soak/scripts/smoke_relink_validate.sh`** queried
   `name LIKE '%CasObjectPut%' OR name LIKE '%CASBlobPut%'`. **`CasObjectPut` has never existed in the
   registry under any spelling** (`grep -c ObjectPut src/Common/ProfileEvents.cpp` = 0) — the same
   never-existed class as `CasRefApplyPoisoned`. The dead alternative was removed; the working
   `CASBlobPut` one remains, so behaviour is unchanged and the query no longer implies a phantom counter.

Also fixed `tmp/test_stand_ca_storage.xml` (tracked, another agent's stand config), whose
`<metadata_type>content_addressed</metadata_type>` the server now rejects — one value, and it was clean in
the worktree.

### Reported, deliberately NOT edited
**`utils/cas-carve/carve.sh`** (11 hits, incl. the sole `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION`).
This is a concurrent agent's active work — its two commits interleave with this task's — and the strings sit
inside `<<'MSG'` heredocs that the script emits as **commit messages describing historical commits**.
Whether those should carry the new spelling is that effort's call, not a naming sweep's, and editing an
in-flight foreign file risks a conflict. Flagged for the lead.

### Reviewed and kept (lead ruling)
`ci/praktika/settings.py:87`, `ci/praktika/native_jobs.py:227`, `ci/defs/altinity_jobs.py:65` use
"content-addressed" in the GENERIC sense — CI-cache content-addressed hashing, unrelated to the feature.

### Dated history, kept per the ruling in `t1-report.md`
`utils/ca-soak/scenarios/BACKLOG.md` (24) and `RUN_HISTORY.md` record what runs observed and what commands
were issued at the time.

## Step 1b — the second gate, which the plan wrote incorrectly
The plan's `Cas[A-Z]` grep passes a PCRE lookahead `Cas(…|Blob(?!Put\b)|…)` to `grep -vE`. `-E` has no
lookahead: the command **errors out and prints nothing**, and `wc -l` reports `0`. A zero that means "the
gate is broken", read as "the tree is clean". Replaced with a Python classifier over every tracked file
outside `docs/superpowers/`, `.superpowers/`, `contrib/`:

**174 distinct `Cas[A-Z]` identifiers, 4046 occurrences; 61 are not production symbols.** Every one of the
61 was reviewed and falls into: gtest **test-case** names (the second `TEST(...)` argument, e.g.
`CasPutCreateAndSwap`, `CasUpdateRefusesWhenAbsent`, `CasAdmitEntry*` — these mean compare-and-swap and the
Global Constraints protect them); **string literals** (`"CasGcStopStartTest"` is a scheduler name argument,
not a suite); historical names of **removed** suites cited in comments (`CasGcTrim`, `CasGcFence`, …);
ca-soak **stack-frame symbol filters** (`CasRef`, `CasText`, `CasProtocol`, `CasBuild`); the deliberately
invalid `CasGood` in a SQL-injection rejection test; the two **retired** metric names documented in
`t1-report.md`; and dated BACKLOG entries. **No live suite, user-facing string, metric, or doc remains
`Cas`-spelled** — independently confirmed by the gate: `--gtest_list_tests | grep '^Cas'` returns only
`CascadeWriteBuffer`.

## Step 2 — full build and full gate, BOTH builds
| build | ninja | generator | per-suite gate |
|---|---|---|---|
| release (`build`) | `NINJA_EXIT=0`, 0 `error:` lines | 278 suites, 4 excluded, 0 unclaimed | **pass=278 fail=0 abort=0** |
| ASan (`build_asan`) | `NINJA_EXIT=0` | 296 suites, 4 excluded, 0 unclaimed | **pass=296 fail=0 abort=0** |

The 296 − 278 = **18** delta is exactly the compile-guarded death-test suites ASan exposes and release does
not — enumerated by `comm`, not assumed. The 19th, `CASRefInstallSafetyDeathTest`, is gated on
`MEMORY_TRACKER_DEBUG_CHECKS` (debug-only, since sanitizer builds define `NDEBUG`) and was proven present in
`build_debug` during Task 4. Both arms of the death-test split are therefore accounted for by listing, not
by a pass/fail run.

`utils/ca-soak` unit suites: **336 passed**.

## What this effort does NOT claim
No stateless or integration suites were executed — the spec excludes them. The evidence here is: the tree
builds clean in two configurations, every CAS gtest suite passes in both, the ca-soak tooling suite passes,
and no configuration names a key or value the server rejects. It is not evidence that the stateless or
integration lanes pass.
