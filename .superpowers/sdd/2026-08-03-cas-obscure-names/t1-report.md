# Task 1 — metrics and gtest suites: `Gc`→`GC`, `Ckpt`→`Checkpoint`, `Txns`→`Transactions`, `Dedup`→`Deduplication`

## Enumeration

The plan's file glob (`src/*.cpp src/*.h utils/ca-soak/* utils/cas-gate/* tests/* docs/en/*`) filtered by the
loose pattern `CASGc|CASRefCkpt|FoldedTxns|Dedup` returned 163 files — but most of those matched only the
unrelated core-ClickHouse `Deduplication*` identifiers (`MergeTreeDeduplicationLog`, `InsertDeduplication`, …).

Re-derived against the tokens the sed actually rewrites
(`CASGc|CASRefCkpt|FoldedTxns|PutDedup\b|DedupCache`), tree-wide over `git ls-files` minus
`docs/superpowers/`, `.superpowers/`, `contrib/`: **72 files**.

**Deviation (widening):** one of those 72, `programs/disks/CommandFsck.cpp`, is NOT matched by the plan's
glob (`programs/` is absent from it). It carries a comment naming `CASGcNamespaceCleanupLeaks`. The sed was
applied to the tree-wide set instead of the plan's glob, so this file was covered.

After the edit, `git diff --name-only` = exactly those 72 files (verified by `diff` against the enumeration;
the only extra entry in `git status` was `.superpowers/sdd/task-5-report.md`, a pre-existing foreign
modification that was NOT staged).

## Applied

- `CASGc` → `CASGC` (85 distinct metric/suite tokens), `CASRefCkpt` → `CASRefCheckpoint`,
  `UnappliedFoldedTxns` → `UnappliedFoldedTransactions`, `PutDedup\b` → `PutDeduplicated`,
  `DedupCache` → `DeduplicationCache`.
- Production classes `CasGc` / `CasRefCkpt` (lowercase `as`) untouched — confirmed: 97 files still
  reference them, and the only `CasGc`-bearing diff line is a comment that cites a gtest suite name.
- Async format strings in `ServerAsynchronousMetrics.cpp` are `CASGCIsLeader_{}`,
  `CASGCPendingReclaim_{}`, `CASGCLastSuccessAgeSeconds_{}`, `CASGCWedgedNamespaces_{}` — the sed covered
  them, no manual fix needed.
- Prose: `CASGCRetiredSparedByReref`'s "a fresh dedup-adopt" → "a fresh deduplicating adopt";
  `CASDeduplicationCacheBytes` / `CASDeduplicationCacheEntries` "(dedup) cache" → "(deduplication) cache".
  No lowercase `dedup` outside `deduplicat*` remains in `ProfileEvents.cpp` / `CurrentMetrics.cpp`.
- `utils/cas-gate/generate_cas_suites.sh` KNOWN_COMPILE_GUARDED now names `CASGCHoldGrammarDeathTest`
  and `CASGCStateFormatDeathTest`.

## Verification

- `grep -nE 'CASGc|CASRefCkpt|FoldedTxns|PutDedup\b|DedupCache'` tree-wide (minus the excluded dirs):
  **empty**.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t1_build.log`).
- `utils/cas-gate/generate_cas_suites.sh`: 278 suites, 4 excluded, **0 unclaimed**.
- `utils/cas-gate/run_cas_gate_per_suite.sh build` under the unit-tests flock:
  **TOTALS: pass=278 fail=0 abort=0**, `GATE_EXIT=0` (`build/obscure_t1_gate.log`,
  `build/per_suite_results.txt`).
