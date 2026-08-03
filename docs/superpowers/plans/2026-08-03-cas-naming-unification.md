# CAS Naming Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename every user-facing spelling of the content-addressed-storage feature (`Cas`, `CA`, `content_addressed`, `CONTENT ADDRESSED`, …) to the canonical `CAS` (CamelCase/SQL/prose) / `cas` (snake_case), per the spec `docs/superpowers/specs/2026-08-03-cas-naming-unification-design.md`.

**Architecture:** Surface-by-surface commits (metrics → SQL/grants → tables/settings/config → gtest suites → stateless tests/CI → integration tests → docs → final sweep). Each task is a mechanical rename driven by exact-name lists (never blanket `Cas`→`CAS` seds), verified by build + unit tests + grep gates.

**Tech Stack:** ClickHouse C++ (ninja build in `build/`), gtest (`build/src/unit_tests_dbms`), bash/sed/git, praktika CI generator.

**Repo:** `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`. All paths below are relative to this root. Run all commands from this root unless stated.

## Global Constraints

- **NEVER touch compare-and-swap identifiers.** These contain `Cas`/`cas` meaning *compare-and-swap*, not the feature, and MUST NOT be renamed: `casPut`, `CasOutcome`, `CasResult`, `CasOp::Cas`, `CasOp::CasConflict`, `GcMaintenanceCasOutcome`, `GcMaintenanceCasResult`, `casGcMaintenanceState`, `kMaxCatalogCasAttempts`, `cas_result`, and the whole `CasRequestControl.h` family (`CasWriteOutcome`, `CasUnresolvedReason`, `CasRequestBudget`, `CasRequestController`, `CasCreateOutcome`, `CasCreateResult`, `CasOverwriteOutcome`, `CasOverwriteResult`, `throwCasWriteRetryLater`, `throwCasTransientUnavailable`, `classifyConditionalWriteResult`, `recordConditionalWriteOutcome`, `validateCasRequestBudget`, `cas_request_budget`).
- **Internal C++ identifiers stay.** Do not rename: namespace `DB::Cas`, production classes (`CasPool`, `CasGc`, `CasLayout`, `CasMountRuntime`, `ContentAddressedLog`, `StorageSystemContentAddressedMounts`, …), source file names (`ContentAddressedLog.cpp`, `gtest_cas_*.cpp`, …), the AST field `content_addressed_gc_rebuild_force`, the string key `"cas_owner"` (already canonical).
- **Replication wire protocol IS renamed** (user decision 2026-08-03): the `DataPartsExchange.cpp` HTTP parameter/cookie literals become `cas_pool_uuid`, `cas_relink`, `cas_confirm`, `cas_source_token`, `cas_confirm_answer` (Task 3), and the integration test assertions follow (Task 6). No compatibility fallback — a mixed-build cluster silently degrades CAS fetches to full copies, so soak clusters must run a single build after this lands. The C++ constant names (`CA_POOL_UUID_PARAM`, …) stay.
- **`docs/superpowers/**` is out of scope** (internal artifacts). So are `utils/ca-soak` / `utils/cas-gate` *directory names* (contents ARE updated where they reference renamed user-facing names). Untracked run debris (`*.stdout`, `*.stderr`, `_instances-gw*/`, `ci/tmp/`, `logs_archive/`) is ignored.
- **Per-disk CAS settings stay unprefixed** (`scratch_path`, `gc_enabled`, `server_root_id`, … in `ContentAddressedSettings.cpp`) — they are scoped by the disk block by design.
- **Verification per task:** `ninja -C build clickhouse unit_tests_dbms` must succeed; unit tests as specified per task; aggressive grep re-check of every remaining suspicious hit. No stateless/integration test runs in this effort (per spec).
- **Grep hygiene:** when sweeping for leftovers always exclude false positives with `grep -vE 'cast|Cast|CAST|case|Case|CASE|cascad|Cascad|CASCADE|replicas|Cassandra'` and exclude `docs/superpowers/`, `.superpowers/`, untracked debris.
- **Commits:** one commit per task, message prefix `cas: `, ending with the standard co-author line.

---

### Task 1: Metrics — `Cas*` → `CAS*`, compare-and-swap tails → `CompareSwap`

**Files:**
- Modify: `src/Common/ProfileEvents.cpp:759-914` (156 events), `src/Common/CurrentMetrics.cpp:233-241` (9 metrics)
- Modify: `src/Interpreters/ServerAsynchronousMetrics.cpp:385-391` (4 format strings)
- Modify: ~44 files in `src/` with `ProfileEvents::Cas*` / `CurrentMetrics::Cas*` externs and uses (mechanical, via exact-name sed)
- Modify: `utils/ca-soak/**` (metric name strings + `LIKE 'Cas%'` patterns), `tests/integration/test_cas_replicated_relink/test.py` (`CasBlobPut`)

**Interfaces:**
- Produces: metric names `CASBlobPut`, `CASGcHeadMiss`, `CASBlobCompareSwap`, … used by later tasks' docs and by the final sweep. The 13 special renames: `CasBlobCas`→`CASBlobCompareSwap`, `CasBlobCasConflict`→`CASBlobCompareSwapConflict`, same pattern for `CasManifestCas[Conflict]`, `CasRootCas[Conflict]`, `CasGcCas[Conflict]`, `CasServerCas[Conflict]`, `CasOtherCas[Conflict]`, and `CasMetaCas`→`CASMetaCompareSwap`.

- [ ] **Step 1: Build the exact rename map and record the baseline**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
mkdir -p /tmp/cas-rename
{ grep -oP '^\s*M\(\KCas\w+' src/Common/ProfileEvents.cpp
  grep -oP '^\s*M\(\KCas\w+' src/Common/CurrentMetrics.cpp; } | sort -u > /tmp/cas-rename/metric_names.txt
wc -l /tmp/cas-rename/metric_names.txt   # expect 165
python3 - <<'EOF'
special = {
 "CasBlobCas":"CASBlobCompareSwap","CasBlobCasConflict":"CASBlobCompareSwapConflict",
 "CasManifestCas":"CASManifestCompareSwap","CasManifestCasConflict":"CASManifestCompareSwapConflict",
 "CasRootCas":"CASRootCompareSwap","CasRootCasConflict":"CASRootCompareSwapConflict",
 "CasGcCas":"CASGcCompareSwap","CasGcCasConflict":"CASGcCompareSwapConflict",
 "CasServerCas":"CASServerCompareSwap","CasServerCasConflict":"CASServerCompareSwapConflict",
 "CasOtherCas":"CASOtherCompareSwap","CasOtherCasConflict":"CASOtherCompareSwapConflict",
 "CasMetaCas":"CASMetaCompareSwap",
}
with open("/tmp/cas-rename/metric_names.txt") as f, open("/tmp/cas-rename/metric_map.sed","w") as out:
    for name in (l.strip() for l in f if l.strip()):
        new = special.get(name, "CAS" + name[3:])
        out.write(f"s/\\b{name}\\b/{new}/g\n")
EOF
wc -l /tmp/cas-rename/metric_map.sed     # expect 165
```

- [ ] **Step 2: Apply the map to src/, ca-soak, integration tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*.cpp' 'src/*.h' 'utils/ca-soak/*' 'tests/integration/*' \
  | xargs grep -lP '\bCas[A-Z]' \
  | xargs sed -i -f /tmp/cas-rename/metric_map.sed
# LIKE-pattern prefix queries (case-sensitive LIKE):
sed -i "s/LIKE 'Cas%'/LIKE 'CAS%'/g" utils/ca-soak/scripts/dump_cas_metrics.py \
  utils/ca-soak/scenarios/framework/observe.py utils/ca-soak/scripts/smoke_relink_validate.sh
# Async metric name format strings:
grep -n 'CasGc' src/Interpreters/ServerAsynchronousMetrics.cpp   # then fix by hand if sed missed the fmt strings
```
Check the four format strings at `src/Interpreters/ServerAsynchronousMetrics.cpp:385-391` became `CASGcIsLeader_{}`, `CASGcPendingReclaim_{}`, `CASGcLastSuccessAgeSeconds_{}`, `CASGcWedgedNamespaces_{}` (the sed map covers `CasGcIsLeader` etc. only if those exact tokens are in the map — `CasGcIsLeader` is NOT a ProfileEvent, so edit these four strings manually with Edit if unchanged).

- [ ] **Step 3: Fix descriptions where "CAS" means compare-and-swap**

In `src/Common/ProfileEvents.cpp`, reword the description *text* (not names) so "CAS" is never used for compare-and-swap:
- line ~800 (`CASRefCkptPublished`): "durably updated by a token-CAS" → "durably updated by a token compare-and-swap"
- line ~908 (`CASRefRecoveryEpochSealed`): "MINTED by a recovery CAS-walk" → "MINTED by a recovery compare-and-swap walk"
- line ~909 (`CASRefRecoveryEpochSealAdopted`): same rewording ("a recovery CAS-walk" → "a recovery compare-and-swap walk")
Verify no other description uses "CAS" in the compare-and-swap sense:
```bash
grep -n 'CAS' src/Common/ProfileEvents.cpp | grep -vE '^\s*[0-9]+:\s*M\(CAS' | grep -iE 'swap|conditional|token|walk'
```

- [ ] **Step 4: Verify no compare-and-swap identifier was harmed and no old names remain**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# these MUST still exist unchanged:
grep -rn 'casPut' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h | head -3
grep -c 'CasOutcome' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h  # >0
# no old metric name anywhere tracked:
git ls-files | xargs grep -nP '\bCas(BlobPut|GcHeadMiss|BlobCas|MetaCas|RootCas|DedupCacheHits)\b' \
  | grep -v 'docs/superpowers' ; echo "expect no output above"
# review the full diff for accidental renames of non-metric identifiers:
git diff --stat | tail -3 && git diff src/ | grep -E '^[-+].*Cas' | grep -vE 'CAS|compare-and-swap' | head -20
```

- [ ] **Step 5: Build and run unit tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
./build/src/unit_tests_dbms --gtest_filter='Cas*' --gtest_brief=1 2>&1 | tail -5   # suites still Cas* until Task 4
```
Expected: build OK, all tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A src/ utils/ca-soak tests/integration
git commit -m "cas: metrics -- Cas* -> CAS*, compare-and-swap tails -> CompareSwap"
```

---

### Task 2: SQL commands and grants — `CONTENT ADDRESSED` → `CAS`

**Files:**
- Modify: `src/Parsers/ASTSystemQuery.h:150-156` (enum constants), `src/Parsers/ASTSystemQuery.cpp` (switch cases ~:158,265-330), `src/Parsers/ParserSystemQuery.cpp` (switch cases ~:460-524)
- Modify: `src/Access/Common/AccessType.h:351-357`
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (dispatch ~:1021-1072, required-access ~:3243-3275, message/comment texts :2331-2680)
- Modify: user-visible strings in `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (`ContentAddressedMetadataStorage.cpp:930,939,973`, `Pool/CasPool.cpp:322,335,900`, `Pool/CasMountRuntime.cpp:368`, `Gc/CasGc.cpp:1724,2123,2778,3205,3217`), `src/Storages/System/StorageSystemContentAddressedMounts.cpp:58`, `src/Common/ProfileEvents.cpp:888,904,905`
- Modify: gtests asserting the strings: `src/Parsers/tests/gtest_Parser.cpp:481-521`, `src/Disks/tests/gtest_cas_lifecycle_snapshot.cpp:132`, `gtest_cas_forget.cpp:26,52,232,487,492,520`, `gtest_cas_gc_stop_start.cpp:25,444`, `gtest_cas_gc_rebuild.cpp:574`, `gtest_cas_mount.cpp:1161`
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference:161-167`, the 11 stateless tests issuing the commands, 2 integration tests, `utils/ca-soak/scenarios/**` (framework/gc.py, cards, READMEs)

**Interfaces:**
- Produces: SQL `SYSTEM CAS GC RUN|GC REBUILD [FORCE]|GC START|GC STOP|FSCK|FORGET|DROP POOL MEMBER`, grants `SYSTEM CAS GC RUN` etc. (AccessType enum `SYSTEM_CAS_*`). Task 7 documents these spellings.

- [ ] **Step 1: Rename the enum constants (parser + formatter change atomically via magic_enum)**

In `src/Parsers/ASTSystemQuery.h:150-156` rename `CONTENT_ADDRESSED_GC_RUN`→`CAS_GC_RUN`, `CONTENT_ADDRESSED_GC_REBUILD`→`CAS_GC_REBUILD`, `CONTENT_ADDRESSED_DROP_POOL_MEMBER`→`CAS_DROP_POOL_MEMBER`, `CONTENT_ADDRESSED_FSCK`→`CAS_FSCK`, `CONTENT_ADDRESSED_FORGET`→`CAS_FORGET`, `CONTENT_ADDRESSED_GC_STOP`→`CAS_GC_STOP`, `CONTENT_ADDRESSED_GC_START`→`CAS_GC_START`. Then:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*.cpp' 'src/*.h' | xargs grep -l 'CONTENT_ADDRESSED_' \
  | xargs sed -i 's/\bCONTENT_ADDRESSED_\(GC_RUN\|GC_REBUILD\|DROP_POOL_MEMBER\|FSCK\|FORGET\|GC_STOP\|GC_START\)\b/CAS_\1/g'
git ls-files 'src/*.cpp' 'src/*.h' | xargs grep -l 'SYSTEM_CONTENT_ADDRESSED_' \
  | xargs sed -i 's/\bSYSTEM_CONTENT_ADDRESSED_/SYSTEM_CAS_/g'
```
Then fix `src/Access/Common/AccessType.h:351-357` alias strings by hand: each row becomes e.g. `M(SYSTEM_CAS_GC_RUN, "SYSTEM CAS GC RUN", GLOBAL, SYSTEM)`. Do NOT keep old aliases (branch unreleased, spec says no back-compat).

- [ ] **Step 2: Rename the SQL text in strings, comments, tests, soak**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'tests/queries/*' 'tests/integration/*' 'utils/ca-soak/*' \
  | xargs grep -l 'CONTENT ADDRESSED' | xargs sed -i 's/CONTENT ADDRESSED/CAS/g'
```
This covers `SYSTEM CONTENT ADDRESSED …` inside exception texts, gtest expectations, stateless `.sh`/`.sql` (GRANT + SYSTEM statements), `utils/ca-soak/scenarios/framework/gc.py` (`GC_SQL`), cards, and READMEs.

- [ ] **Step 3: Regenerate the privileges reference by hand**

Edit `tests/queries/0_stateless/01271_show_privileges.reference:161-167`: replace the 7 lines with the `SYSTEM CAS *` spellings, e.g. `SYSTEM CAS GC RUN	['SYSTEM CAS GC RUN']	GLOBAL	SYSTEM` (tab-separated, same order). Keep the surrounding lines untouched.

- [ ] **Step 4: Verify**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files | xargs grep -n 'CONTENT ADDRESSED\|CONTENT_ADDRESSED_\|SYSTEM_CONTENT_ADDRESSED' \
  | grep -v 'docs/superpowers' | grep -v 'docs/en'   # docs/en handled in Task 7; expect only docs/en hits or none
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
./build/src/unit_tests_dbms --gtest_filter='*Parser*:Cas*' --gtest_brief=1 2>&1 | tail -5
```
Expected: build OK, parser + CAS gtests pass (they now assert the new `SYSTEM CAS …` strings).

- [ ] **Step 5: Commit**

```bash
git add -A src/ tests/ utils/ca-soak
git commit -m "cas: sql -- SYSTEM CONTENT ADDRESSED ... -> SYSTEM CAS ..., grants CAS_*"
```

---

### Task 3: System tables, server settings, config, metadata_type

**Files:**
- Modify: `src/Interpreters/SystemLog.h:20-21` (macro member names), `src/Interpreters/Context.cpp:6246,6254` (member accesses)
- Modify: `src/Storages/System/attachSystemTables.cpp:250` (`"content_addressed_mounts"` → `"cas_mounts"`)
- Modify: `programs/server/config.xml:1201-1213,1321-1330` (log sections)
- Modify: `src/Core/ServerSettings.cpp:151-161`, `programs/server/Server.cpp:398-399,1723-1726`, `programs/local/LocalServer.cpp:203-204,429-432`, `src/Disks/.../Pool/CasBlobUploadPool.cpp:39,202`, `.h:127`, `programs/disks/DisksApp.cpp:557` (settings rename)
- Modify: `src/Disks/DiskType.h:35`, `src/Disks/DiskType.cpp:22`, `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:219` (+7 more `MetadataStorageType::ContentAddressed` users)
- Modify: prose/exception texts naming `system.content_addressed_*` in src/ (list in Step 3)
- Modify: stateless/integration/soak files referencing the table names and setting names (Task 5/6 rename the files themselves; here only src/ + `programs/`)

**Interfaces:**
- Produces: tables `system.cas_log`, `system.cas_garbage_collection_log`, `system.cas_mounts`; config sections `<cas_log>`, `<cas_garbage_collection_log>`; server settings `cas_blob_upload_pool_size`, `cas_condemned_upload_memory_bytes`; `metadata_type` value `cas` (sole accepted spelling, no alias); `system.disks.metadata_type` shows `CAS`.

- [ ] **Step 1: SystemLog member rename (renames table name + config section together)**

In `src/Interpreters/SystemLog.h:20-21` change the macro rows' second argument: `content_addressed_garbage_collection_log` → `cas_garbage_collection_log`, `content_addressed_log` → `cas_log` (class names `ContentAddressedGarbageCollectionLog` / `ContentAddressedLog` stay). Update the member accesses:
```bash
sed -i 's/->content_addressed_garbage_collection_log/->cas_garbage_collection_log/; s/->content_addressed_log/->cas_log/' src/Interpreters/Context.cpp
```
Update `programs/server/config.xml`: section tags and `<table>` values `content_addressed_log`→`cas_log`, `content_addressed_garbage_collection_log`→`cas_garbage_collection_log`.

- [ ] **Step 2: system.cas_mounts + server settings**

- `src/Storages/System/attachSystemTables.cpp:250`: `"content_addressed_mounts"` → `"cas_mounts"`.
- Settings rename across src/ and programs/:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'programs/*' | xargs grep -l 'content_addressed_blob_upload_pool_size\|content_addressed_condemned_upload_memory_bytes' \
  | xargs sed -i 's/content_addressed_blob_upload_pool_size/cas_blob_upload_pool_size/g; s/content_addressed_condemned_upload_memory_bytes/cas_condemned_upload_memory_bytes/g'
```
(This also renames the `ServerSetting::…` symbols and doc-text self-references — intended.)

- [ ] **Step 3: Fix user-visible texts naming the tables in src/**

Replace `system.content_addressed_mounts`→`system.cas_mounts`, `system.content_addressed_log`→`system.cas_log`, `system.content_addressed_garbage_collection_log`→`system.cas_garbage_collection_log` and bare-name mentions in comments/messages:
```bash
git ls-files 'src/*' | xargs grep -l 'content_addressed_mounts\|content_addressed_log\|content_addressed_garbage_collection_log' \
  | xargs sed -i 's/content_addressed_garbage_collection_log/cas_garbage_collection_log/g; s/content_addressed_mounts/cas_mounts/g; s/content_addressed_log/cas_log/g'
```
Files expected to change: `ContentAddressedMetadataStorage.{h,cpp}`, `Pool/CasPool.h`, `Pool/CasServerRoot.{h,cpp}`, `Pool/CasMountRuntime.{h,cpp}`, `Pool/CasRefLedger.cpp`, `Parts/PartFolderAccess.cpp`, `Gc/CasGc.{h,cpp}`, `Gc/CasGcScheduler.h`, `src/Common/ProfileEvents.cpp:900`, `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:37`, `src/Storages/System/StorageSystemContentAddressedMounts.h:11`, gtests `gtest_cas_lifecycle_snapshot.cpp`, `gtest_cas_gc_log.cpp`, `gtest_cas_ref_writer.cpp`.

- [ ] **Step 4: metadata_type — canonical `cas`, no alias, enum → `CAS`**

- `src/Disks/DiskType.h:35`: rename enum constant `ContentAddressed` → `CAS`; update all 8 `MetadataStorageType::ContentAddressed` users (`DiskType.cpp`, `RegisterDiskObjectStorage.cpp`, `DiskObjectStorage.{h,cpp}`, `MergeTreeData.cpp`, `MergeTreeDeduplicationLog.cpp`, `ContentAddressedMetadataStorage.h`, `gtest_cas_operation_gate.cpp`). `system.disks.metadata_type` will now render `CAS` via magic_enum.
- `src/Disks/DiskType.cpp:22` (`metadataTypeFromString`): `"content_addressed"` → `"cas"` (single accepted spelling, per spec — NO compat alias; old ATTACH `.sql` files, backups, and soak configs carrying `metadata_type = content_addressed` will fail to parse and need a manual edit or recreation).
- `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:219`: `registerMetadataStorageType("cas", …)` — the old registration string is replaced, not kept.
- Sweep the remaining `"content_addressed"` metadata_type value occurrences in `utils/ca-soak/configs/*.xml` (~30 files, `<metadata_type>` and comments): `grep -rl 'content_addressed' utils/ca-soak/configs/ | xargs sed -i 's/content_addressed/cas/g'`.

- [ ] **Step 5: Replication wire-protocol names**

In `src/Storages/MergeTree/DataPartsExchange.cpp:117-141` change the five string literals (constant names `CA_POOL_UUID_PARAM` etc. stay): `"content_addressed_pool_uuid"`→`"cas_pool_uuid"`, `"content_addressed_relink"`→`"cas_relink"`, `"content_addressed_confirm"`→`"cas_confirm"`, `"content_addressed_source_token"`→`"cas_source_token"`, `"content_addressed_confirm_answer"`→`"cas_confirm_answer"`. Both sides of the protocol live in this one file, so the rename is atomic within a build. The integration-test assertions on these names are updated by Task 6's sed.

- [ ] **Step 6: Verify + build + unit tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/*' 'programs/*' | xargs grep -n 'content_addressed' \
  | grep -vE 'ContentAddressed|content_addressed_gc_rebuild_force|content_addressed_allow_shared_pool|content_addressed_gc_grace_sec'
# expect: no output (only the allowed internal identifiers remain)
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
./build/src/unit_tests_dbms --gtest_filter='Cas*' --gtest_brief=1 2>&1 | tail -5
```

- [ ] **Step 7: Commit**

```bash
git add -A src/ programs/
git commit -m "cas: tables/settings/config/wire -- cas_log, cas_garbage_collection_log, cas_mounts, cas_* settings, metadata_type=cas, cas_* fetch protocol names"
```

---

### Task 4: gtest suite prefix `Cas*` → `CAS*` and the cas-gate

**Files:**
- Modify: `src/Disks/tests/gtest_ca*.cpp` (131 files, 266 `Cas*` suites) and any shared fixture headers (`src/Disks/tests/cas_test_helpers.h` etc.) — suite/fixture identifiers only
- Modify: `utils/cas-gate/generate_cas_suites.sh`, `utils/cas-gate/run_cas_gate_per_suite.sh`

**Interfaces:**
- Consumes: metric renames from Task 1 (gtest files already reference `ProfileEvents::CAS*`).
- Produces: all CAS suites named `CAS*`; gate invariant "every suite in `gtest_ca*.cpp` is `CAS`-prefixed or excluded"; gate filter `--gtest_filter='CAS*'` semantics.

- [ ] **Step 1: Extract the exact suite-name list**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/src/Disks/tests
grep -ohP '^TEST(_F|_P)?\(\s*\K\w+' gtest_ca*.cpp | sort -u > /tmp/cas-rename/suites_all.txt
grep '^Cas' /tmp/cas-rename/suites_all.txt | grep -v '^Cascade' > /tmp/cas-rename/suites_cas.txt
wc -l /tmp/cas-rename/suites_cas.txt   # expect ~265 (266 Cas* minus CascadeWriteBuffer)
# also the INSTANTIATE prefixes:
grep -ohP 'INSTANTIATE_TEST_SUITE_P\(\s*\K\w+' gtest_ca*.cpp | sort -u   # expect CasInMemory, CasLocal
```
`CascadeWriteBuffer` (accidental `Cas` prefix, in `gtest_cascade_and_memory_write_buffer.cpp`) must NOT be renamed — hence the `grep -v '^Cascade'`.

- [ ] **Step 2: Check for collisions with production identifiers, then apply scoped rename**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# any suite name that is ALSO a production symbol would be over-renamed — must be empty:
while read s; do git grep -lw "$s" -- 'src/' ':!src/Disks/tests' ':!src/*/tests' | head -1 | sed "s/^/COLLISION $s: /"; done < /tmp/cas-rename/suites_cas.txt
```
If collisions appear, handle those names file-scoped by hand (rename only inside `src/Disks/tests/`). Then:
```bash
python3 - <<'EOF'
with open("/tmp/cas-rename/suites_cas.txt") as f, open("/tmp/cas-rename/suites_map.sed","w") as out:
    for name in (l.strip() for l in f if l.strip()):
        out.write(f"s/\\b{name}\\b/CAS{name[3:]}/g\n")
    out.write("s/\\bCasInMemory\\b/CASInMemory/g\n")
    out.write("s/\\bCasLocal\\b/CASLocal/g\n")
EOF
cd src/Disks/tests && sed -i -f /tmp/cas-rename/suites_map.sed gtest_ca*.cpp cas_test_helpers*.h 2>/dev/null; cd -
```
Fixture classes share the suite name (`TEST_F(CASFoo, …)` needs `class CASFoo`), so the word-boundary sed inside the tests dir covers both. Compile errors in Step 4 catch any fixture defined elsewhere — fix those by renaming the fixture identifier at its definition site (test helpers only, never production headers).

- [ ] **Step 3: Update the gate scripts**

`utils/cas-gate/generate_cas_suites.sh`: change the case-sensitive prefix literals — line 57 `== Cas*` → `== CAS*`, line 81 `'^TEST(_F|_P)?\(\s*Cas'` → `...\s*CAS'`, line 126 `!= Cas*` → `!= CAS*`, line 146 `== Cas*` → `== CAS*`, line 181 `grep '^Cas'` → `grep '^CAS'`. Add `CascadeWriteBuffer` to the exclusion list `EXCLUDE_REASONS` (lines 46-50) with reason "cascade write buffer, not CAS — accidental Cas prefix no longer matches CAS*". `KNOWN_COMPILE_GUARDED` DeathTest names (lines 105-113): apply the same `Cas`→`CAS` prefix rename to each entry.
`utils/cas-gate/run_cas_gate_per_suite.sh`: no `Cas` literal expected besides comments — verify with `grep -n 'Cas' utils/cas-gate/run_cas_gate_per_suite.sh` and update any.

- [ ] **Step 4: Build, run the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
bash utils/cas-gate/generate_cas_suites.sh 2>&1 | tail -5    # read script header first if it needs env/args
bash utils/cas-gate/run_cas_gate_per_suite.sh 2>&1 | tail -5
./build/src/unit_tests_dbms --gtest_filter='Cas*' --gtest_brief=1 2>&1 | tail -3   # expect: only CascadeWriteBuffer-family runs now
```
Expected: build OK, generator passes the `CAS*` invariant, per-suite gate green.

- [ ] **Step 5: Commit**

```bash
git add -A src/Disks/tests utils/cas-gate
git commit -m "cas: tests -- gtest suites Cas* -> CAS*, gate verifies the CAS prefix"
```

---

### Task 5: Stateless tests, tag, runner flags, CI configs, workflows

**Files:**
- Rename: 36 stateless test bases (`.sql`/`.sh` + `.reference`) — `content_addressed`→`cas`, plus `05008_ca_gc_snap_prune.*`→`05008_cas_gc_snap_prune.*`, `05013_system_content_addressed_drop_pool_member.*`→`05013_system_cas_drop_pool_member.*`
- Modify: contents of those tests (pool/table identifiers, `metadata_type = cas`, system table names, FLUSH LOGS names)
- Modify: tag `no-content-addressed-storage`→`no-cas-storage` in 14 test files + `tests/clickhouse-test` (:1355, :2882-2886, :2895, :2905, :5642-5650, :6367-6377) + `src/.../ContentAddressed/README.md:185`
- Rename: `tests/config/config.d/content_addressed_storage_policy_for_merge_tree_by_default.xml`→`cas_storage_policy_for_merge_tree_by_default.xml`, `content_addressed_s3_…`→`cas_s3_…` + contents (disk/policy names `cas`, `cas_s3`, `cas_s3_cache`, `metadata_type` value `cas`) + `tests/config/install.sh` (:32,33,378,382,383,391)
- Modify: `ci/defs/job_configs.py:780-833` (param names `content_addressed`→`cas`), `ci/jobs/functional_tests.py` (:145,146,160,161,249,303-309,650), `ci/jobs/scripts/clickhouse_proc.py` (:194,1301-1337 — the `<metadata_type>content_addressed</metadata_type>` literals)
- Regenerate: `.github/workflows/{master,pull_request,pull_request_community,release_builds}.yml`

**Interfaces:**
- Consumes: SQL `SYSTEM CAS …` (Task 2), table/settings/metadata_type names (Task 3).
- Produces: tag `no-cas-storage`; runner flags `--cas-storage`, `--cas-s3-storage`; CI job params "cas storage" / "cas s3 storage"; policies/disks `cas`, `cas_s3`.

- [ ] **Step 1: git mv the stateless tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tests/queries/0_stateless
for f in *content_addressed*; do case "$f" in *.stdout|*.stderr) continue;; esac; git mv "$f" "${f//content_addressed/cas}"; done
git mv 05008_ca_gc_snap_prune.sh 05008_cas_gc_snap_prune.sh
git mv 05008_ca_gc_snap_prune.reference 05008_cas_gc_snap_prune.reference 2>/dev/null || true
git status -s | grep '^R' | wc -l   # expect ~74 renames (36 bases x2 + 05008 pair)
```

- [ ] **Step 2: Update test contents (identifiers, values, table names)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tests/queries/0_stateless
grep -l 'content_addressed' *.sql *.sh *.reference 2>/dev/null \
  | xargs sed -i 's/content_addressed_garbage_collection_log/cas_garbage_collection_log/g; s/content_addressed_mounts/cas_mounts/g; s/content_addressed_log/cas_log/g; s/content_addressed/cas/g'
```
The final catch-all `s/content_addressed/cas/g` intentionally rewrites test-local pool/disk identifiers (`04278_content_addressed_pool`→`04278_cas_pool`), `metadata_type = content_addressed`→`= cas`, and `content_addressed_s3` policy references. Then re-check nothing unrelated changed:
```bash
git diff -- . | grep '^[-+]' | grep -iE 'cast|case|cascad' | head   # expect no output
```

- [ ] **Step 3: Tag + runner flags**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'tests/queries/*' 'src/*README*' | xargs grep -l 'no-content-addressed-storage' \
  | xargs sed -i 's/no-content-addressed-storage/no-cas-storage/g'
sed -i 's/no-content-addressed-storage/no-cas-storage/g; s/content-addressed-s3-storage/cas-s3-storage/g; s/content-addressed-storage/cas-storage/g; s/content_addressed_s3_storage/cas_s3_storage/g; s/content_addressed_storage/cas_storage/g; s/CONTENT_ADDRESSED_STORAGE/CAS_STORAGE/g' tests/clickhouse-test
grep -n 'content.addressed' tests/clickhouse-test   # expect no output
```

- [ ] **Step 4: tests/config + CI configs**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git mv tests/config/config.d/content_addressed_storage_policy_for_merge_tree_by_default.xml tests/config/config.d/cas_storage_policy_for_merge_tree_by_default.xml
git mv tests/config/config.d/content_addressed_s3_storage_policy_for_merge_tree_by_default.xml tests/config/config.d/cas_s3_storage_policy_for_merge_tree_by_default.xml
sed -i 's/content_addressed/cas/g' tests/config/config.d/cas_storage_policy_for_merge_tree_by_default.xml tests/config/config.d/cas_s3_storage_policy_for_merge_tree_by_default.xml
sed -i 's/content_addressed/cas/g' tests/config/install.sh
sed -i 's/content_addressed/cas/g' ci/defs/job_configs.py ci/jobs/functional_tests.py ci/jobs/scripts/clickhouse_proc.py
grep -n '"cas" not in\|cas storage' ci/jobs/functional_tests.py | head   # sanity: the :303 substring gate now keys on "cas"
```
Manually review `ci/jobs/functional_tests.py:303-309` — the condition `if "s3 storage" in to and "content_addressed" not in to:` must now read `"cas" not in to` and still exclude the cas-s3 jobs correctly.

- [ ] **Step 5: Regenerate the GitHub workflows**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
PYTHONPATH=ci python3 -m praktika yaml
git diff --stat .github/workflows/ | tail -3
git diff .github/workflows/master.yml | grep -E '^[-+].*(content_addressed|cas)' | head -10
```
Expected: job ids `…_content_addressed[_s3]_storage_…` become `…_cas[_s3]_storage_…`; no other churn. (If `python3 -m praktika` fails, check `ci/README.md` for the exact invocation.)

- [ ] **Step 6: Sweep + commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'tests/queries/*' 'tests/config/*' 'ci/*' | xargs grep -n 'content.addressed' | grep -v 'docs/superpowers'
# expect no output
git add -A tests/queries tests/config tests/clickhouse-test ci .github src
git commit -m "cas: stateless tests, tag no-cas-storage, runner flags, CI params and workflows"
```

---

### Task 6: Integration tests

**Files:**
- Rename: `tests/integration/test_content_addressed_{drop_pool_member,gc_s3,ref_snaplog,s3,shared_pool}/` → `test_cas_*/`
- Modify: their `test.py` + `configs/*.xml` and the 5 existing `test_cas_*` dirs' configs (`metadata_type` value, policy/disk names, `system.cas_*` table names — Task 2/3 seds already fixed SQL command text and metric names here)
- Create: `tests/integration/test_cas_ref_snaplog/__init__.py` (empty; the dir is the only sibling missing it)

**Interfaces:**
- Consumes: names produced by Tasks 1-3.
- Produces: integration dirs uniformly `test_cas_*`.

- [ ] **Step 1: Rename dirs, add missing __init__.py**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tests/integration
for d in test_content_addressed_*; do git mv "$d" "${d/content_addressed/cas}"; done
touch test_cas_ref_snaplog/__init__.py && git add test_cas_ref_snaplog/__init__.py
```

- [ ] **Step 2: Update contents**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tests/integration
# one catch-all sed: table names, metadata_type, disk/policy names, endpoints, AND the
# wire-protocol assertions in test_cas_replicated_relink/test.py:629-644 (renamed in Task 3 Step 5)
grep -rl 'content_addressed' test_cas_*/ | xargs sed -i 's/content_addressed/cas/g'
rm -f test_cas_replicated_relink/configs/storage_conf-preprocessed.xml   # stale artifact, untracked; skip if git-tracked
```
Then verify the wire-protocol assertions now expect `cas_pool_uuid`, `cas_relink`, `cas_confirm`, `cas_source_token`, `cas_confirm_answer` — matching the literals set in Task 3 Step 5:
```bash
grep -n 'cas_pool_uuid\|cas_relink\|cas_confirm\|cas_source_token' test_cas_replicated_relink/test.py | head
```

- [ ] **Step 3: Verify + commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rn 'content_addressed' tests/integration/ --include='*.py' --include='*.xml' \
  | grep -v '_instances'   # expect no output
python3 -m py_compile tests/integration/test_cas_*/test.py && echo PY-OK
git add -A tests/integration
git commit -m "cas: integration tests -- test_content_addressed_* -> test_cas_*"
```

---

### Task 7: Documentation

**Files:**
- Rename: `docs/en/operations/system-tables/content_addressed_log.md`→`cas_log.md`, `content_addressed_garbage_collection_log.md`→`cas_garbage_collection_log.md`, `content_addressed_mounts.md`→`cas_mounts.md`
- Modify: those three + `docs/en/operations/storing-data.md` + `docs/en/sql-reference/statements/system.md`

**Interfaces:**
- Consumes: every name produced by Tasks 1-5.
- Produces: docs referencing only `CAS`/`cas` spellings; slugs `/operations/system-tables/cas_*`; anchors `#system-cas-gc-run`, `#system-cas-gc-rebuild`, `#system-cas-drop-pool-member`.

- [ ] **Step 1: Rename files and update frontmatter/slugs/links**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/docs/en/operations/system-tables
for f in content_addressed_*.md; do git mv "$f" "${f/content_addressed/cas}"; done
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rln 'content_addressed' docs/en/ | xargs sed -i 's/content_addressed_garbage_collection_log/cas_garbage_collection_log/g; s/content_addressed_mounts/cas_mounts/g; s/content_addressed_log/cas_log/g; s/content_addressed_s3/cas_s3/g; s/content_addressed/cas/g'
```
This fixes frontmatter (`slug`, `title`, `sidebar_label`), the inbound links in `storing-data.md:464-466` and `system.md:470`, the XML examples (`metadata_type` value → `cas`, example disk `s3_cas`), and the two server settings names.

- [ ] **Step 2: SQL command text, headings, anchors**

In `docs/en/sql-reference/statements/system.md`:
- `:460` heading → `### SYSTEM CAS GC RUN {#system-cas-gc-run}` (also fixes the previously inconsistent `#content-addressed-garbage-collection` anchor), `:474` → `{#system-cas-gc-rebuild}`, `:503` → `{#system-cas-drop-pool-member}`; syntax blocks `:465,486,518` → `SYSTEM CAS …`.
- Update the three anchor links in `docs/en/operations/storing-data.md:461-463` to the new anchors.
- `sed -i 's/CONTENT ADDRESSED/CAS/g' docs/en/operations/storing-data.md docs/en/operations/system-tables/cas_garbage_collection_log.md docs/en/operations/system-tables/cas_mounts.md docs/en/sql-reference/statements/system.md` for any remaining command mentions.

- [ ] **Step 3: Prose — abbreviation `CA` → `CAS`, compare-and-swap disambiguation, `Cas*` → `CAS*`**

Manual edits (grep-guided, these are prose, not sed-able blindly):
- All 14 `CA`-as-feature occurrences → `CAS`; first mention per page stays spelled out: "content-addressed storage (CAS)". Files/lines: `cas_log.md:2,13,25`, `cas_garbage_collection_log.md:2,13,155`, `cas_mounts.md:2,13,18,68`, `storing-data.md:457`, `system.md:462,476,477`.
- `cas_garbage_collection_log.md:72,73,84`: the four `CAS` occurrences meaning S3 compare-and-swap → reword as "compare-and-swap" (e.g. "`gc/state` `GET` + compare-and-swap write").
- `cas_garbage_collection_log.md:55`: "`Cas*` counters" → "`CAS*` counters".
- `storing-data.md:478-495`: the documented nested `<content_addressed>` settings block does not exist in code (keys are read flat off the disk block) — rewrite the example/prose to flat keys under the disk element while renaming; keep the line-496 sentence about unprefixed per-disk keys (still true).
- Headings: "### Using Content-Addressed Storage" and the `#…-content-addressed` parameter anchors in `storing-data.md` may keep the spelled-out form (first-mention rule); do not invent `#…-cas` anchors unless links break.

- [ ] **Step 4: Verify + commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rn 'content_addressed\|CONTENT ADDRESSED\|Cas[A-Z]' docs/en/ | grep -vE 'cast|Cast|case|Case|cascad|Cassandra' 
# expect: no output
grep -rnE '\bCA\b' docs/en/operations/system-tables/cas_*.md docs/en/operations/storing-data.md docs/en/sql-reference/statements/system.md | grep -v 'certificate' 
# expect: no output (feature-CA all gone)
git add -A docs/en
git commit -m "cas: docs -- canonical CAS/cas naming, renamed system-table pages, fixed config-block description"
```

---

### Task 8: Final sweep and full gate

**Files:** none new — verification only, plus fixes for anything the sweep finds.

- [ ] **Step 1: Full-tree grep gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files | grep -vE '^docs/superpowers/|^\.superpowers/' | xargs grep -nE 'content[_ -]addressed|Content[- ]Addressed|CONTENT[_ ]ADDRESSED' 2>/dev/null \
  | grep -vE 'ContentAddressed[A-Za-z]*|content_addressed_allow_shared_pool|content_addressed_gc_grace_sec' > /tmp/cas-rename/final_sweep.txt
wc -l /tmp/cas-rename/final_sweep.txt
```
Manually review EVERY line in `final_sweep.txt`. Legitimate survivors: C++ class/file names (`ContentAddressed*`), the two dead skip-listed config keys, spelled-out English prose "content-addressed storage (CAS)" at first mentions and headings, and `contrib/`. Anything else gets fixed and folded into a follow-up commit.

```bash
git ls-files | grep -vE '^docs/superpowers/|^\.superpowers/|^contrib/' | xargs grep -nP '\bCas[A-Z]' 2>/dev/null \
  | grep -vE 'Cas(Pool|Gc|Layout|Mount|Ref|Server|Blob(?!Put\b)|Backend|Probe|Request|Write|Create|Overwrite|Unresolved|Outcome|Result|Op\b|InMemory|Local|ObjectStorage|InstrumentedBackend|NamespaceJanitor|PlainObjects|PartWriteTxn|Decommission|DiskLifecycle)' | head -40
```
Review the remainder: every hit must be an internal C++ identifier (production `Cas*` classes, `casPut` family) — no user-facing string, metric, test name, or doc may remain `Cas`-spelled.

- [ ] **Step 2: Full build + full unit gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build clickhouse unit_tests_dbms 2>&1 | tail -5
bash utils/cas-gate/run_cas_gate_per_suite.sh 2>&1 | tail -10
```
Expected: build OK, all `CAS*` suites green.

- [ ] **Step 3: Commit any sweep fixes**

```bash
git status -s
git add -A && git commit -m "cas: final naming sweep fixes"   # only if the sweep changed anything
```
