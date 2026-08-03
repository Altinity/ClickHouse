# CAS Obscure Names Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove obscure abbreviations from user-facing CAS names per the ClickHouse naming policy: no abbreviations in user-facing API except widely-known ones; abbreviations that stay are written all-caps.

**Architecture:** Small surface-by-surface commits over the already-CAS-renamed tree. Exact-name seds plus hand edits, verified by build + unit tests + grep gates (no stateless/integration runs — same verification policy as the parent effort).

**Tech Stack:** ClickHouse C++ (ninja build in `build/`), gtest (`build/src/unit_tests_dbms`), bash/sed/git.

**Repo:** `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`.

**PRECONDITION:** Runs strictly AFTER `docs/superpowers/plans/2026-08-03-cas-naming-unification.md` is fully executed. Every name below is the post-rename spelling (`CASGc*` metrics, `system.cas_log` / `cas_gc_log` / `cas_mounts` tables, `cas_*` test names, `docs/en/operations/system-tables/cas_*.md`).

## Decisions (user, 2026-08-03)

- `Gc` in CamelCase names → all-caps `GC` (`CASGcHead` → `CASGCHead`); precedent: `OSCPUVirtualTimeMicroseconds`.
- `Ref`, `Meta`, `FSCK` — keep as-is.
- `Dedup` → spell out: `Deduplication` / `Deduplicated`.
- `started_at_ms` / `expires_at_ms` (system.cas_mounts) — verified: already `DateTime64(3)`, only the `_ms` suffix lies about the type → rename to `started_at` / `expires_at` (no type change needed).
- `duration_ms` (system.cas_gc_log) — verified: `UInt64` count of milliseconds → keep.
- Clear violations to fix: `srid` → `server_root_id`; `Ckpt` → `Checkpoint`; `gen` → `generation`; `indeg` → `indegree`; `snap` → `snapshot`; `Txns` → `Transactions`; `phase_duration_us` → `phase_duration_microseconds`.

## Global Constraints

- **Persisted formats are NOT user-facing — never touch them:** the serialization key `"gen"` in `src/Disks/.../ContentAddressed/Formats/CasFoldSealFormat.cpp` (fold-seal format written to object storage), the `_snap` / `_ckpt` / `_log` object-name suffixes in `CasLayout`, and any other string written into the pool. Only table/column/metric/setting/doc/test spellings change.
- Internal C++ identifiers stay (enum constants `CasEventType::IndegZero`, `CasEventObjectKind::Snap`, struct members `lease.started_at_ms`, local `col_srid`, file `Pool/CasRefCkpt.cpp`, …) — only the *string literals* that reach the user are renamed. Renaming a nearby identifier for readability is allowed but never required.
- Same verification policy as the parent plan: `ninja -C build clickhouse unit_tests_dbms` per task, unit tests only, aggressive grep re-checks; no stateless/integration runs.
- Grep hygiene: exclude `docs/superpowers/`, `.superpowers/`, untracked debris (`*.stdout`, `*.stderr`, `_instances*`, `ci/tmp/`, `logs_archive/`, unreadable `*.tmp.*`). Add `2>/dev/null` to greps — the stateless dir contains unreadable root-owned debris.
- Commits: one per task, message prefix `cas: `, standard co-author line.

---

### Task 1: Metrics and gtest suites — `Gc`→`GC`, `Ckpt`→`Checkpoint`, `Txns`→`Transactions`, `Dedup`→`Deduplication`

**Files:**
- Modify: `src/Common/ProfileEvents.cpp`, `src/Common/CurrentMetrics.cpp`, `src/Interpreters/ServerAsynchronousMetrics.cpp` (4 async format strings)
- Modify: every `src/` file referencing the renamed `ProfileEvents::…` / `CurrentMetrics::…` symbols (mechanical)
- Modify: `utils/ca-soak/**` metric-name strings; `utils/cas-gate/*.sh` if any renamed suite is listed literally
- Modify: gtest suites `CASGc*` → `CASGC*` in `src/Disks/tests/gtest_ca*.cpp`

**Interfaces:**
- Produces: metric spellings `CASGCHead`, `CASGCCompareSwap`, `CASRefCheckpointPublished`, `CASGCUnappliedFoldedTransactions`, `CASBlobPutDeduplicated`, `CASDeduplicationCacheHits`, async `CASGCIsLeader_<disk>` — used by Task 2-4 docs edits and the final sweep.

- [ ] **Step 1: Apply the four token renames across the tree**

The tokens are unique to the renamed names (production classes stay `CasGc`/`CasRefCkpt` with lowercase letters and are untouched by these case-sensitive patterns):
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
FILES=$(git ls-files 'src/*.cpp' 'src/*.h' 'utils/ca-soak/*' 'utils/cas-gate/*' 'tests/*' 'docs/en/*' | xargs grep -lE 'CASGc|CASRefCkpt|FoldedTxns|Dedup' 2>/dev/null)
sed -i 's/CASGc/CASGC/g; s/CASRefCkpt/CASRefCheckpoint/g; s/UnappliedFoldedTxns/UnappliedFoldedTransactions/g; s/PutDedup\b/PutDeduplicated/g; s/DedupCache/DeduplicationCache/g' $FILES
```
Covers: 156-block ProfileEvents + 9 CurrentMetrics + extern re-declarations + `cas_event_table` + gtest suites `CASGCRound`-style + ca-soak cards/signals + `LIKE 'CASGc%'`-style patterns if any (`grep -rn "CASGc" utils/ca-soak/` afterwards must be empty) + doc mentions.

- [ ] **Step 2: Async metric format strings + descriptions prose**

- `src/Interpreters/ServerAsynchronousMetrics.cpp`: confirm the four format strings became `CASGCIsLeader_{}`, `CASGCPendingReclaim_{}`, `CASGCLastSuccessAgeSeconds_{}`, `CASGCWedgedNamespaces_{}` (Step 1's sed covers them; fix manually if not).
- ProfileEvents/CurrentMetrics description *texts*: `grep -n 'dedup' src/Common/ProfileEvents.cpp src/Common/CurrentMetrics.cpp` — reword lowercase prose "dedup"/"dedup cache" → "deduplication"/"deduplication cache"; leave grammar intact.

- [ ] **Step 3: gate + suite-name fallout check**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rn 'CASGc\|CASRefCkpt\|FoldedTxns' src/ utils/ tests/ docs/en/ 2>/dev/null | grep -v 'docs/superpowers'   # expect empty
grep -n 'CASGC\|Checkpoint' utils/cas-gate/generate_cas_suites.sh | head   # prefix checks still 'CAS*' — unaffected; just confirm no literal old suite names
```

- [ ] **Step 4: Build, unit tests, gate**

```bash
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
bash utils/cas-gate/run_cas_gate_per_suite.sh 2>&1 | tail -5
```
Expected: build OK, all `CAS*` suites green (the `CAS*` prefix invariant is unaffected by `Gc`→`GC` inside names).

- [ ] **Step 5: Commit**

```bash
git add -A src/ utils/ tests/ docs/en
git commit -m "cas: names -- GC all caps, Checkpoint, Transactions, Deduplication spelled out"
```

---

### Task 2: system.cas_log — `gen`→`generation`, `indeg`→`indegree`, `snap`→`snapshot`

**Files:**
- Modify: `src/Interpreters/ContentAddressedLog.cpp` (column `"gen"`:32, description texts :24, :39)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.cpp` (`"indeg_zero"`:34, `"snap"`:87) and whichever producer emits the `prev_indeg` detail key (find in Step 1)
- Modify: `docs/en/operations/system-tables/cas_log.md`, stateless tests referencing the names (expected: `05009_cas_event_log.*`)

**Interfaces:**
- Produces: column `generation`; `event_type` value `indegree_zero`; `detail` key `prev_indegree`; `object_kind` value `snapshot`.

- [ ] **Step 1: Locate every producer/consumer of the four strings**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'tests/*' 'docs/en/*' | xargs grep -n '"gen"\|indeg_zero\|prev_indeg\|"snap"' 2>/dev/null | grep -v 'docs/superpowers'
```
Sort the hits: `CasFoldSealFormat.cpp` `"gen"` hits are PERSISTED format — leave them (Global Constraints). Everything else (log column definition, `CasEvent.cpp` value strings, the `prev_indeg` emitter, docs, test SQL/references) gets renamed.

- [ ] **Step 2: Apply**

- `ContentAddressedLog.cpp:32`: `{"gen", …}` → `{"generation", …}`; update the `indeg_zero`/`prev_indeg` mentions in the column description strings (:24, :39).
- `CasEvent.cpp:34`: `return "indeg_zero"` → `return "indegree_zero"`; `:87`: `return "snap"` → `return "snapshot"`; rename the `prev_indeg` detail-key literal at its emitter.
- Docs `cas_log.md`: lines 25 (`indeg_zero` in the event list), 39 (`prev_indeg`), the `gen` column entry, `object_kind` value list `snap`→`snapshot`.
- Tests: update hits found in Step 1 (SQL selecting `gen`, references asserting `indeg_zero`/`snap` output).

- [ ] **Step 3: Verify, build, unit tests, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'tests/*' 'docs/en/*' | xargs grep -n 'indeg_zero\|prev_indeg' 2>/dev/null | grep -v 'docs/superpowers'   # expect empty
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5 && ./build/src/unit_tests_dbms --gtest_filter='CAS*Log*:CAS*Event*' --gtest_brief=1 2>&1 | tail -3
git add -A src/ tests/ docs/en && git commit -m "cas: cas_log -- generation, indegree_zero, prev_indegree, snapshot spellings"
```

---

### Task 3: system.cas_gc_log — `srid`→`server_root_id`, `phase_duration_us`→`phase_duration_microseconds`

**Files:**
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` (column `"srid"`:37, `"phase_duration_us"`:61-62) — internal member names (`srid`, `phase_duration_us`) may stay
- Modify: `docs/en/operations/system-tables/cas_gc_log.md` (column list + the three example queries using `phase_duration_us`), `docs/en/sql-reference/statements/system.md` (DROP POOL MEMBER prose: `srid` mentions ~:505-521, placeholder `'srid'` in the syntax block → `'server_root_id'`)
- Modify: `utils/ca-soak/scenarios/framework/observe.py` and other soak files querying `srid`; stateless tests querying either column (expected: `05007_cas_gc_introspection.*`, `05010_cas_mounts_gc_health.*`)

**Interfaces:**
- Produces: columns `server_root_id`, `phase_duration_microseconds` in `system.cas_gc_log`; `system.md` uses `server_root_id` wording.

- [ ] **Step 1: Locate consumers**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'tests/*' 'utils/*' 'docs/en/*' | xargs grep -nw 'srid\|phase_duration_us' 2>/dev/null | grep -v 'docs/superpowers'
```
`srid` hits that are internal C++ variables/members (`col_srid`, `m.srid`, `local_srid`, struct fields) may stay; only the column-name string, SQL query texts, and prose change. Watch the false positive `test_storage_iceberg_with_spark` (unrelated `srid` = spatial reference id in geometry tests) — do NOT touch it.

- [ ] **Step 2: Apply**

- `ContentAddressedGarbageCollectionLog.cpp:37`: `{"srid", …}` → `{"server_root_id", …}` and drop the now-redundant "The `server_root_id` of…" description opener; `:61`: `"phase_duration_us"` → `"phase_duration_microseconds"` (keep the sub-millisecond rationale text, fix the self-reference to `duration_ms` untouched).
- `cas_gc_log.md`: column entries and example queries (`p50_us` etc. aliases in doc examples may be rewritten as `p50_microseconds` for consistency).
- `system.md` DROP POOL MEMBER section: replace `srid` wording with `server_root_id` (the parenthetical "(`server_root_id`, or `srid`)" collapses to just `server_root_id`).
- Soak + stateless SQL: rename the identifiers inside queries found in Step 1.

- [ ] **Step 3: Verify, build, unit tests, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/Interpreters/*' 'tests/queries/*' 'utils/ca-soak/*' 'docs/en/*' | xargs grep -nw 'srid\|phase_duration_us' 2>/dev/null | grep -v 'docs/superpowers'
# expect: only internal C++ identifiers in src (if kept) — no strings, no SQL, no docs
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5 && ./build/src/unit_tests_dbms --gtest_filter='CAS*' --gtest_brief=1 2>&1 | tail -3
git add -A src/ tests/ utils/ docs/en && git commit -m "cas: cas_gc_log -- server_root_id and phase_duration_microseconds columns"
```

---

### Task 4: system.cas_mounts — `started_at_ms`/`expires_at_ms` → `started_at`/`expires_at`

**Files:**
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp:47-48` (column names only — they are already `DateTime64(3)`; struct members `lease.started_at_ms` stay)
- Modify: `tests/queries/0_stateless/05012_cas_mounts_typed_columns.sql` + `.reference` (asserts exactly these names and types)
- Modify: `docs/en/operations/system-tables/cas_mounts.md:31-32`

**Interfaces:**
- Produces: columns `started_at`, `expires_at` (`DateTime64(3)`).

- [ ] **Step 1: Apply**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
sed -i 's/"started_at_ms"/"started_at"/; s/"expires_at_ms"/"expires_at"/' src/Storages/System/StorageSystemContentAddressedMounts.cpp
git ls-files 'tests/queries/*' 'docs/en/*' | xargs grep -l 'started_at_ms\|expires_at_ms' 2>/dev/null \
  | xargs sed -i 's/started_at_ms/started_at/g; s/expires_at_ms/expires_at/g'
```
Then re-check `05012_cas_mounts_typed_columns.reference` by hand — it prints the column names, so the expected output must list `started_at` / `expires_at` with `DateTime64(3)`.

- [ ] **Step 2: Verify, build, unit tests, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/Storages/*' 'tests/queries/*' 'docs/en/*' | xargs grep -n 'started_at_ms\|expires_at_ms' 2>/dev/null | grep -v 'docs/superpowers'  # expect empty
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
git add -A src/ tests/ docs/en && git commit -m "cas: cas_mounts -- started_at/expires_at (type was already DateTime64(3))"
```

---

### Task 5: Per-disk settings and test names — `dedup_*`, `gc_snap_*`

**Files:**
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedSettings.cpp` (`dedup_cache_bytes`, `dedup_head_first_min_bytes`, `gc_snap_generations_to_keep` declarations) + every consumer in `src/` (symbols `…Settings::dedup_cache_bytes` etc.)
- Modify: config XMLs setting these keys: `tests/config/config.d/cas*_storage_policy_*.xml`, `tests/integration/test_cas_*/configs/*.xml`, `utils/ca-soak/configs/*.xml`, `utils/ca-soak/docker-compose-small_dedup_cache.yml`
- Modify: `docs/en/operations/storing-data.md` (key list)
- Rename: `tests/queries/0_stateless/04285_cas_dedup_window_inline_disk.*` → `04285_cas_deduplication_window_inline_disk.*`, `05006_cas_dedup_blob_insert.*` → `05006_cas_deduplication_blob_insert.*`, `05008_cas_gc_snap_prune.*` → `05008_cas_gc_snapshot_prune.*` (+ inline `dedup`/`snap` identifiers within)

**Interfaces:**
- Produces: per-disk keys `deduplication_cache_bytes`, `deduplication_head_first_min_bytes`, `gc_snapshot_generations_to_keep`.

- [ ] **Step 1: Rename the keys everywhere (code symbol + config key share the spelling)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'tests/*' 'utils/*' 'docs/en/*' | xargs grep -l 'dedup_cache_bytes\|dedup_head_first_min_bytes\|gc_snap_generations_to_keep' 2>/dev/null \
  | xargs sed -i 's/dedup_cache_bytes/deduplication_cache_bytes/g; s/dedup_head_first_min_bytes/deduplication_head_first_min_bytes/g; s/gc_snap_generations_to_keep/gc_snapshot_generations_to_keep/g'
```
No compat alias (policy of this effort); old configs must be edited. The dead-key skip-list is already gone (parent plan).

- [ ] **Step 2: Test renames + contents**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tests/queries/0_stateless
for b in 04285_cas_dedup_window_inline_disk 05006_cas_dedup_blob_insert; do
  for f in "$b".*; do git mv "$f" "${f/_dedup_/_deduplication_}"; done; done
for f in 05008_cas_gc_snap_prune.*; do git mv "$f" "${f/_snap_/_snapshot_}"; done
grep -l '\bdedup\|_snap\b' *cas*.sql *cas*.sh 2>/dev/null | xargs -r grep -n 'dedup\|snap'   # review remaining inline uses, rename identifiers/pool names by hand
```

- [ ] **Step 3: Verify, build, unit tests, commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files | xargs grep -nw 'dedup_cache_bytes\|dedup_head_first_min_bytes\|gc_snap_generations_to_keep' 2>/dev/null | grep -v 'docs/superpowers'  # expect empty
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5 && ./build/src/unit_tests_dbms --gtest_filter='CAS*Settings*:CAS*Dedup*:CAS*Deduplication*' --gtest_brief=1 2>&1 | tail -3
git add -A src/ tests/ utils/ docs/en && git commit -m "cas: settings -- deduplication_* and gc_snapshot_generations_to_keep spelled out"
```

---

### Task 6: Final sweep

- [ ] **Step 1: Grep gate over the decided renames**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files | grep -vE '^docs/superpowers/|^\.superpowers/|^contrib/' \
  | xargs grep -nE 'CASGc[A-Z]|CASRefCkpt|FoldedTxns|PutDedup\b|DedupCache|indeg_zero|prev_indeg|phase_duration_us|started_at_ms|expires_at_ms|dedup_cache_bytes|gc_snap_generations' 2>/dev/null \
  | grep -v 'CasFoldSealFormat'   # expect empty
git ls-files 'docs/en/*' 'tests/queries/*' | xargs grep -nw 'srid\|gen\|snap' 2>/dev/null | grep -v 'docs/superpowers'
```
Review the second grep's hits manually (`gen`/`snap` are common substrings — only whole-word user-facing leftovers count; the iceberg `srid` is unrelated and stays).

- [ ] **Step 2: Full build + gate + commit any fixes**

```bash
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
bash utils/cas-gate/run_cas_gate_per_suite.sh 2>&1 | tail -5
git status -s && git add -A && git commit -m "cas: obscure-names sweep fixes"   # only if the sweep changed anything
```
