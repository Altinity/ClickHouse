# Task 3 report — system tables, server settings, config, metadata_type, wire protocol

## Step 1 — SystemLog members (table + config section together)
`SystemLog.h`: `content_addressed_garbage_collection_log` -> `cas_gc_log`, `content_addressed_log` -> `cas_log`
(class names `ContentAddressedGarbageCollectionLog` / `ContentAddressedLog` unchanged). Member accesses in
`Context.cpp` follow. `programs/server/config.xml`: both section tags and both `<table>` values.

**Why one rename moves both — verified, not assumed.** `SystemLogs::SystemLogs` expands
`CREATE_PUBLIC_MEMBERS(log_type, member, descr)` as
`createSystemLog<log_type>(global_context, "system", #member, config, #member, descr)`: the member name is
stringified for the default table name *and* for the config prefix, and `SystemLog.cpp` then reads
`config.getString(config_prefix + ".table", default_table_name)`.

## Step 2 — `system.cas_mounts` + server settings
`attachSystemTables.cpp` -> `"cas_mounts"`. Settings renamed across **6** files:
`cas_blob_upload_pool_size`, `cas_condemned_upload_memory_bytes`. No old spelling remains in `src/`/`programs/`.

## Step 3 — user-visible texts naming the tables
19 files swept (the plan's predicted list plus `ContentAddressed/README.md`). `src/` is clean of
`content_addressed_mounts` / `content_addressed_log` / `content_addressed_garbage_collection_log`.

## Step 4 — metadata_type, canonical `cas`, no alias
- `DiskType.h`: enum `ContentAddressed` -> `CAS`; all **8** users updated (`DiskType.cpp`,
  `RegisterDiskObjectStorage.cpp`, `DiskObjectStorage.{h,cpp}`, `MergeTreeData.cpp`,
  `MergeTreeDeduplicationLog.cpp`, `ContentAddressedMetadataStorage.h`, `gtest_cas_operation_gate.cpp`) —
  the count was re-derived from `git grep`, matching the plan's 8.
- `metadataTypeFromString`: `"content_addressed"` -> `"cas"`. `registerMetadataStorageType("cas", …)`.
- 37 `utils/ca-soak/configs/*.xml` swept.

### The no-alias rule holds — checked three ways, including against the running binary
1. No `"content_addressed"` string survives anywhere in `src/`/`programs/`.
2. `metadataTypeFromString` ends in `throw Exception(UNKNOWN_ELEMENT_IN_CONFIG, …)` — unknown values fail
   loud; there is no default arm. The only compatibility path,
   `MetadataStorageFactory::getCompatibilityMetadataTypeHint`, maps object-storage types to `"local"`/`"web"`
   only and can never yield a content-addressed value, so it cannot act as a back door.
3. Live, on the built binary:
   - `metadata_type='content_addressed'` -> `Code: 137 … unknown metadata storage type: content_addressed`
     `(UNKNOWN_ELEMENT_IN_CONFIG)`.
   - `metadata_type='cas'` -> resolves to the CAS storage and fails later on CAS-specific validation
     (`Expected 'server_root_id' in config for a content-addressed disk`), proving the type was accepted.
   - A fully-specified CAS disk: `SELECT name, metadata_type FROM system.disks` -> `d3  CAS`.
   - `SELECT name FROM system.server_settings` -> `cas_blob_upload_pool_size`,
     `cas_condemned_upload_memory_bytes`; `system.tables` shows `cas_mounts`.

### Legacy skip-list keys deleted — and the breakage this creates, deliberately
Removed `"content_addressed_allow_shared_pool"` and `"content_addressed_gc_grace_sec"` from `non_cas_keys`,
together with the paragraph explaining why they were skip-listed (the reason — "several integration-test
configs still set them" — is exactly what stops being true).

**Consequence, stated plainly:** `loadFromConfig` passes any non-skip-listed key to `impl->set`, which throws
on an unregistered setting. Every integration config still setting either key now **fails to start the
server**, so `test_cas_*` integration tests are broken between this commit and Task 6. The plan accepts this
(no integration runs in this effort, Task 6 lands before any future run) — recording it here so the breakage
is a known, dated, scheduled condition rather than a surprise.

## Step 5 — replication wire protocol
The five literals in `DataPartsExchange.cpp` are now `cas_pool_uuid`, `cas_relink`, `cas_confirm`,
`cas_source_token`, `cas_confirm_answer` (constant names `CA_*_PARAM`/`_COOKIE` unchanged), plus the two
comments at `:95` and `:392` that quote `cas_pool_uuid`.

**No other file defines or quotes the old literals.** A tree-wide grep for all five returns, outside
`docs/`: only `DataPartsExchange.cpp`, `tests/integration/test_cas_replicated_relink/test.py` (assertions —
Task 6's scope, per plan) and `utils/ca-soak/scripts/smoke_relink_validate.sh:105`, which the plan does not
list. That script greps a log for `"relink\|part_manifest_v1\|content_addressed_relink"`; the literal was
updated to `cas_relink` for truth, though the bare `relink` alternative already dominates the match, so
behaviour is unchanged either way. Both sides of the protocol live in the one file, so the rename is atomic
within a build — a mixed-build cluster degrades to full copies, as the Global Constraints state.

## Step 6 — verification
### The plan's grep expected no output; it had 17 hits. Each was resolved
`git ls-files 'src/*' 'programs/*' | xargs grep -n 'content_addressed' | grep -vE 'ContentAddressed'`:
- `IDisk.h` (`metadata_type = content_addressed`), `LocalObjectStorage.cpp`, `DisksApp.cpp`,
  `LocalServer.cpp` (x2), `DataPartStorageOnDiskBase.cpp` (x4), `MergeTreeData.cpp` (x2) — comments and **two
  user-visible exception messages** ("not supported on a content_addressed disk") that named the old value.
  All fixed: `cas` for the literal setting value, `CAS` for prose.
- `gtest_cas_settings.cpp:60` built a config with `<metadata_type>content_addressed</metadata_type>`.
  I first read this as a test that would now fail; **that was wrong** — `metadata_type` is in `non_cas_keys`,
  so the value is skipped and never resolved. It was stale, not broken. Renamed to `cas` anyway, with the
  test-local `path`/`name` values.

### Three deliberate survivors
- `cas_test_helpers.h:135` and `gtest_ca_wiring.cpp:15` cite `gtest_content_addressed_metadata.cpp`, a PoC
  file that **no longer exists in the tree**. A `cas`-spelled name would invent a filename that never was.
- `gtest_cas_settings.cpp:65` cites
  `tests/config/config.d/content_addressed_storage_policy_for_merge_tree_by_default.xml`, which is accurate
  **today** and which **Task 5 renames**. Task 5's seds cover `tests/` and `ci/`, not `src/`, so this line
  would be missed: it must be updated as part of Task 5, at `src/Disks/tests/gtest_cas_settings.cpp:65`.

### Build and tests
- `build/naming_t3_build.log` -> `NINJA_EXIT=0`, 0 lines matching `error:`.
- `build/naming_t3_unit.log`: `--gtest_filter='Cas*'` under `flock` -> **2006 tests from 279 suites, all
  PASSED**, exit 0.

## Step 7 — commit
Staged by explicit 80-file list. **Deviation from the plan's `git add -A src/ programs/`:** that command
would have dropped the 37 `utils/ca-soak/configs/*.xml` files Step 4 requires, and `git add -A` is unsafe in
this worktree anyway. `.superpowers/sdd/task-5-report.md` (foreign, pre-existing) stays unstaged.

## Pre-existing finding, not fixed here
The two exception messages carry internal ticket ids into user-visible text — "not supported on a CAS disk
yet (B16/B34)" in `DataPartStorageOnDiskBase.cpp` and the sibling in `MergeTreeData.cpp`. That violates the
comment/prose policy on internal references and is worse in an exception than in a comment, but it predates
this effort and is out of a naming task's scope. Flagged rather than silently edited.
