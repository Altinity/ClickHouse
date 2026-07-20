# Consistency Findings F1-F11 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the 11 consistency-review findings per the approved spec `docs/superpowers/specs/2026-07-21-consistency-findings-f1-f11-design.md`: SYSTEM `GC RUN` rename, `CasGc` casing, mounts-table columns, `ContentAddressedSettings` struct, `ObjectStorageRetryProfile` token, thread names, and five polish items.

**Architecture:** All work on branch `cas-gc-rebuild`, one commit per task, no rebase/amend. Renames are grep-verified sweeps; F4 and F5 are real refactors with unit tests. Everything must keep the existing `Cas*:CA*` gtest gate green.

**Tech Stack:** C++ (Allman braces), CMake/ninja, gtest (`unit_tests_dbms`), ClickHouse stateless tests.

## Global Constraints

- Branch: `cas-gc-rebuild`. Never rebase/amend; every task ends in a new commit.
- NO compatibility aliases for renamed surfaces (pre-release, per spec Non-goals).
- NEVER edit files under `docs/superpowers/reports/`, `docs/superpowers/worklogs/`, or `tmp/` in sweeps — they are historical records.
- Allman braces; comments state constraints only; write function names as `f` not `f()` in prose; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- Builds: `ninja -C build <target> > build/task_<N>_build.log 2>&1` (no `-j`, no `nproc`); analyze the log with a subagent returning a concise summary. If `build/` does not exist, use the first existing `build*` directory (`ls -d build*`).
- Unit tests: `build/src/unit_tests_dbms --gtest_filter=<filter> > build/task_<N>_test.log 2>&1`; analyze via subagent.
- The spec section for each task is authoritative; this plan restates everything needed inline.
- One known spec deviation (approved rationale in plan preamble): the SDK-retry tripwire event `CasConditionalWriteSdkRetries` is replaced by generic `S3SingleAttemptRetryConsultations` in Task 7, because the strategy that increments it moves into the generic S3 layer and a Cas-named event incremented from `src/IO/S3` would recreate the layering leak F5 removes. Same semantics: only the CAS single-attempt profile uses that strategy.

---

### Task 1: F1 core — rename `CONTENT_ADDRESSED_GARBAGE_COLLECTION` → `CONTENT_ADDRESSED_GC_RUN`

**Files:**
- Modify: `src/Parsers/ASTSystemQuery.h` (~line 150)
- Modify: `src/Parsers/ASTSystemQuery.cpp` (~line 265-300)
- Modify: `src/Parsers/ParserSystemQuery.cpp` (~line 460)
- Modify: `src/Access/Common/AccessType.h` (~line 351)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (~lines 1003, 2990) and `src/Interpreters/InterpreterSystemQuery.h`
- Modify: `src/Parsers/tests/gtest_Parser.cpp` (the `ParserSystemQuery` INSTANTIATE block at ~line 482)
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference:161`

**Interfaces:**
- Produces: enum value `ASTSystemQuery::Type::CONTENT_ADDRESSED_GC_RUN`; access type `AccessType::SYSTEM_CONTENT_ADDRESSED_GC_RUN` with grant string `"SYSTEM CONTENT ADDRESSED GC RUN"`; interpreter method `BlockIO runContentAddressedGcRun(const String & disk_name)`. Task 2 sweeps depend on these final spellings.
- The SQL keyword phrase is auto-derived from the enum name (`_` → space) by the `magic_enum` loop in `ParserSystemQuery.cpp`, so renaming the enum IS the parser change; the `magic_enum::customize::enum_range` specialization at the bottom of `ASTSystemQuery.h` is position-based and unaffected.

- [ ] **Step 1: Add failing round-trip parser tests**

In `src/Parsers/tests/gtest_Parser.cpp`, inside the existing `INSTANTIATE_TEST_SUITE_P(ParserSystemQuery, ParserTest, ...)` initializer list (which currently holds the `DROP POOL MEMBER` cases), add:

```cpp
        {
            "SYSTEM CONTENT ADDRESSED GC RUN",
            "SYSTEM CONTENT ADDRESSED GC RUN"
        },
        {
            "SYSTEM CONTENT ADDRESSED GC RUN disk1",
            "SYSTEM CONTENT ADDRESSED GC RUN disk1"
        },
        {
            "SYSTEM CONTENT ADDRESSED GC RUN disk1 ON CLUSTER my_cluster",
            "SYSTEM CONTENT ADDRESSED GC RUN disk1 ON CLUSTER my_cluster"
        },
```

- [ ] **Step 2: Build and run to verify the new cases fail**

```bash
ninja -C build unit_tests_dbms > build/task_1_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='ParserSystemQuery*' > build/task_1_test.log 2>&1
```
Expected: FAIL — `SYSTEM CONTENT ADDRESSED GC RUN` does not parse (old keyword phrase still registered).

- [ ] **Step 3: Rename the enum value**

`src/Parsers/ASTSystemQuery.h`: change `CONTENT_ADDRESSED_GARBAGE_COLLECTION,` → `CONTENT_ADDRESSED_GC_RUN,` (keep position). Also update the doc comment above `content_addressed_gc_rebuild_force` if it references the old phrase.

`src/Parsers/ASTSystemQuery.cpp`: in the `case Type::RELOAD_DICTIONARY:` group, change `case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:` → `case Type::CONTENT_ADDRESSED_GC_RUN:` and update the comment line `CONTENT ADDRESSED GARBAGE COLLECTION's disk is optional` → `CONTENT ADDRESSED GC RUN's disk is optional`.

`src/Parsers/ParserSystemQuery.cpp`: change `case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:` → `case Type::CONTENT_ADDRESSED_GC_RUN:` and update its comment header to `SYSTEM CONTENT ADDRESSED GC RUN [<disk>] [ON CLUSTER cluster]`; also update the comment inside the `GC REBUILD` case that says "Unlike the per-round GC-collection command" → "Unlike the per-round GC RUN command".

- [ ] **Step 4: Rename the AccessType**

`src/Access/Common/AccessType.h`:

```cpp
    M(SYSTEM_CONTENT_ADDRESSED_GC_RUN, "SYSTEM CONTENT ADDRESSED GC RUN", GLOBAL, SYSTEM) \
```
(replacing the `SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION` line in place).

- [ ] **Step 5: Rename in the interpreter**

`src/Interpreters/InterpreterSystemQuery.cpp`: the `case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:` block becomes:

```cpp
        case Type::CONTENT_ADDRESSED_GC_RUN:
        {
            getContext()->checkAccess(AccessType::SYSTEM_CONTENT_ADDRESSED_GC_RUN);
            result = runContentAddressedGcRun(query.disk);
            break;
        }
```

Rename the method definition `BlockIO InterpreterSystemQuery::runContentAddressedGarbageCollection(const String & disk_name)` → `runContentAddressedGcRun`, its declaration in `InterpreterSystemQuery.h`, and the `getRequiredAccessForDDLOnCluster` case to the new enum/AccessType names.

- [ ] **Step 6: Update the privileges reference**

`tests/queries/0_stateless/01271_show_privileges.reference:161` becomes:

```
SYSTEM CONTENT ADDRESSED GC RUN	['SYSTEM CONTENT ADDRESSED GC RUN']	GLOBAL	SYSTEM
```
(tab-separated, matching the surrounding lines).

- [ ] **Step 7: Build and run parser tests to verify pass**

```bash
ninja -C build unit_tests_dbms > build/task_1_build2.log 2>&1
build/src/unit_tests_dbms --gtest_filter='ParserSystemQuery*' > build/task_1_test2.log 2>&1
```
Expected: PASS. Then verify no stale identifiers in `src`:

```bash
rg -n "CONTENT_ADDRESSED_GARBAGE_COLLECTION|runContentAddressedGarbageCollection" src && echo LEFTOVERS || echo CLEAN
```
Expected: `CLEAN`.

- [ ] **Step 8: Commit**

```bash
git add -u src tests/queries/0_stateless/01271_show_privileges.reference
git commit -m "cas: rename SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION -> GC RUN (F1: one spelling per concept in the command family)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: F1 sweep — old SQL phrase in tests, docs, ca-soak

**Files (complete live-reference list, from a scoped grep at plan time):**
- Modify: `tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh`
- Modify: `tests/queries/0_stateless/05008_ca_gc_snap_prune.sh`
- Modify: `tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh`
- Modify: `tests/queries/0_stateless/05011_cas_gc_rebuild_access.sh`
- Modify: `docs/en/sql-reference/statements/system.md`, `docs/en/operations/storing-data.md`, `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md`, `docs/en/operations/system-tables/content_addressed_mounts.md`
- Modify: `docs/superpowers/cas/review1.md`, `docs/superpowers/cas/ROADMAP.md`, `docs/superpowers/cas/08-testing-and-soak.md`, `docs/superpowers/cas/refactoring-ideas.md`
- Modify: `utils/ca-soak/scenarios/README.md`, `utils/ca-soak/scenarios/ASSUMPTIONS.md`, `utils/ca-soak/scenarios/BACKLOG.md`, `utils/ca-soak/scenarios/framework/gc.py`, `utils/ca-soak/scenarios/cards/s12_s14_faults.py`, `utils/ca-soak/scenarios/cards/s28_s33_corner.py`

**Interfaces:**
- Consumes: the Task 1 final spelling `SYSTEM CONTENT ADDRESSED GC RUN`.

- [ ] **Step 1: Apply the phrase rename to every live file**

```bash
rg -l "CONTENT ADDRESSED GARBAGE COLLECTION" src tests programs utils/ca-soak docs/superpowers/cas docs/en \
  | xargs sed -i 's/CONTENT ADDRESSED GARBAGE COLLECTION/CONTENT ADDRESSED GC RUN/g'
```
Then `git diff --stat` and eyeball each hunk: docs prose around the phrase must still read correctly (e.g. "the `SYSTEM CONTENT ADDRESSED GC RUN` command runs one synchronous round"); fix grammar where the sentence named the command as "garbage collection" verbally. In `docs/en/sql-reference/statements/system.md` keep the section anchor/frontmatter rules intact (headers keep their existing `{#anchors}`).

- [ ] **Step 2: Verify zero live references**

```bash
rg -n "CONTENT ADDRESSED GARBAGE COLLECTION" src tests programs utils docs/en docs/superpowers/cas && echo LEFTOVERS || echo CLEAN
```
Expected: `CLEAN` (reports/worklogs/tmp intentionally untouched).

- [ ] **Step 3: Sanity-run one renamed stateless test if a CA-capable build+env is available; otherwise verify by inspection**

```bash
bash -n tests/queries/0_stateless/05007_content_addressed_gc_introspection.sh && bash -n tests/queries/0_stateless/05011_cas_gc_rebuild_access.sh && echo SYNTAX-OK
```
Expected: `SYNTAX-OK`. (The CA stateless lane runs these for real in the final gate, Task 14.)

- [ ] **Step 4: Commit**

```bash
git add -u tests docs utils
git commit -m "cas: sweep SYSTEM CONTENT ADDRESSED GC RUN spelling through tests, docs, ca-soak (F1)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: F2 — unify async-metric casing to `CasGc*`

**Files:**
- Modify: `src/Interpreters/ServerAsynchronousMetrics.cpp:385-392`

**Interfaces:**
- Produces: asynchronous metric names `CasGcIsLeader_{disk}`, `CasGcPendingReclaim_{disk}`, `CasGcLastSuccessAgeSeconds_{disk}`, `CasGcWedgedNamespaces_{disk}`. Task 4's mounts-table doc references and Task 2's already-updated docs must agree with these spellings.

- [ ] **Step 1: Rename the four format strings**

In `src/Interpreters/ServerAsynchronousMetrics.cpp` replace `CasGCIsLeader_{}` → `CasGcIsLeader_{}`, `CasGCPendingReclaim_{}` → `CasGcPendingReclaim_{}`, `CasGCLastSuccessAgeSeconds_{}` → `CasGcLastSuccessAgeSeconds_{}`, `CasGCWedgedNamespaces_{}` → `CasGcWedgedNamespaces_{}` (descriptions unchanged).

- [ ] **Step 2: Sweep remaining `CasGC` spellings in live dirs**

```bash
rg -n "CasGC" src tests utils/ca-soak docs/en docs/superpowers/cas && echo LEFTOVERS || echo CLEAN
```
Expected: `CLEAN`. If `docs/en/operations/system-tables/content_addressed_mounts.md` or test `05010` reference the old uppercase names, update them here.

- [ ] **Step 3: Compile the file**

```bash
ninja -C build clickhouse > build/task_3_build.log 2>&1
```
Expected: success (subagent-verified log).

- [ ] **Step 4: Commit**

```bash
git add -u src tests docs utils
git commit -m "cas: CasGC* -> CasGc* asynchronous metrics (F2: one casing for the CAS GC concept)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: F3 — `system.content_addressed_mounts` column renames + description cleanup

**Files:**
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp:34-49`
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (`DROP POOL MEMBER` result column `srid`)
- Modify: `docs/en/operations/system-tables/content_addressed_mounts.md`
- Modify: column references in `tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh` (and any other stateless test found by grep), `utils/ca-soak/scenarios/framework/observe.py`, `utils/ca-soak/scenarios/cards/s06_s08_manifest_parts.py`

**Interfaces:**
- Produces: column names `server_root_id`, `process_id`, `renewal_sequence`, `min_active_build_sequence` (all other columns keep their names).

- [ ] **Step 1: Replace the column list**

In `StorageSystemContentAddressedMounts.cpp` replace the four renamed entries and normalize all descriptions to 1-2 factual sentences. The resulting block:

```cpp
        {"disk", std::make_shared<DataTypeString>(), "Name of the content-addressed disk."},
        {"server_root_id", std::make_shared<DataTypeString>(), "Server root id owning the mount slot."},
        {"server_uuid", std::make_shared<DataTypeUUID>(), "UUID of the server incarnation holding the lease."},
        {"hostname", std::make_shared<DataTypeString>(), "Hostname recorded in the lease body."},
        {"process_id", std::make_shared<DataTypeUInt64>(), "Process id recorded in the lease body."},
        {"writer_epoch", std::make_shared<DataTypeUInt64>(), "Fenced writer epoch of the incarnation."},
        {"renewal_sequence", std::make_shared<DataTypeUInt64>(), "Lease renewal sequence number."},
        {"started_at_ms", std::make_shared<DataTypeDateTime64>(3), "Time when the lease started."},
        {"expires_at_ms", std::make_shared<DataTypeDateTime64>(3), "Time when the lease expires."},
        {"min_active_build_sequence", std::make_shared<DataTypeUInt64>(), "Oldest in-flight build sequence (UINT64_MAX means the mount said farewell)."},
        {"gc_fenced", std::make_shared<DataTypeUInt8>(), "1 if GC fenced this slot out (terminal)."},
        {"state", std::make_shared<DataTypeString>(), "Mount slot state: live, expired, terminated, fenced or corrupt."},
        {"is_leader", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt8>()), "1 if this server's GC scheduler holds this disk's leadership lease. NULL on rows describing other servers' mounts."},
        {"pending_reclaim", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeInt64>()), "Cumulative condemned-minus-deleted backlog observed by this process's GC on this disk. NULL on rows describing other servers' mounts."},
        {"last_success_age_seconds", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt64>()), "Seconds since this disk's GC last led a round (0 if it never led). NULL on rows describing other servers' mounts."},
        {"wedged_namespace_count", std::make_shared<DataTypeNullable>(std::make_shared<DataTypeUInt64>()), "Ref-append lanes currently wedged on this disk. NULL on rows describing other servers' mounts."},
```

If the retired-`CasGcIsLeader` historical note matters, move one line into the header comment of the class; do not keep it in a column description.

- [ ] **Step 2: Rename the interpreter result column**

In `InterpreterSystemQuery.cpp`'s `CONTENT_ADDRESSED_DROP_POOL_MEMBER` block change `{"srid", std::make_shared<DataTypeString>()}` → `{"server_root_id", std::make_shared<DataTypeString>()}` (the insert order/value code is positional and unchanged).

- [ ] **Step 3: Sweep query-side references**

```bash
rg -n "\bsrid\b|\bpid\b|\bseq\b|\bmin_active\b" tests/queries/0_stateless/0501*.sh utils/ca-soak docs/en/operations/system-tables/content_addressed_mounts.md
```
Update ONLY references that address the renamed columns of `system.content_addressed_mounts` or the `DROP POOL MEMBER` result set (e.g. `SELECT srid, ...` in `05010`, dict keys in `observe.py`). Do NOT blind-`sed`: `server_root_id` config keys and unrelated `seq` identifiers must not change. Re-run the grep after editing; remaining hits must each be provably unrelated to the renamed columns (state why in the task report).

- [ ] **Step 4: Build + gtest gate**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_4_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/task_4_test.log 2>&1
```
Expected: build OK, gate PASS.

- [ ] **Step 5: Commit**

```bash
git add -u src tests docs utils
git commit -m "cas: spell out system.content_addressed_mounts columns (F3: server_root_id/process_id/renewal_sequence/min_active_build_sequence)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: F4a — `ContentAddressedSettings` struct (declaration + tests)

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- Test: `src/Disks/tests/gtest_content_addressed_settings.cpp` (new)

**Interfaces:**
- Produces (used verbatim by Task 6):

```cpp
struct ContentAddressedSettings
{
    ContentAddressedSettings();
    ContentAddressedSettings(const ContentAddressedSettings &);
    ~ContentAddressedSettings();
    CONTENT_ADDRESSED_SETTINGS_SUPPORTED_TYPES(ContentAddressedSettings, DECLARE_SETTING_SUBSCRIPT_OPERATOR)

    /// Loads every key under `config_prefix`, rejects unknown non-object-storage keys,
    /// anchors a relative scratch_path / defaults it, expands macros in server_root_id, validates.
    void loadFromConfig(const Poco::Util::AbstractConfiguration & config, const std::string & config_prefix,
                        const std::string & default_scratch_path, const MacroExpander & expand_macros);
    void validate();

    /// Typed accessors for the enum-valued string settings (parsed+cached by validate, fail-closed).
    Cas::BlobHashAlgo blobHashAlgo() const;
    Cas::StagingBackend stagingBackend() const;
    Cas::PartFolderValidate partFolderValidate() const;
private:
    std::unique_ptr<ContentAddressedSettingsImpl> impl;
    ...
};
```
where `using MacroExpander = std::function<std::string(const std::string &)>;` (keeps `Context`/`Macros` out of the header). Settings are read as `settings[ContentAddressedSetting::gc_shards].value` etc., mirroring `FileCacheSetting`.

- [ ] **Step 1: Write the failing test file**

Create `src/Disks/tests/gtest_content_addressed_settings.cpp`. Model config loading on Poco `XMLConfiguration` from a string (see existing `src/Disks/tests/` fixtures for the established helper; if none exists there, use `Poco::AutoPtr<Poco::Util::XMLConfiguration>` over `std::istringstream`). Tests:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Poco/AutoPtr.h>
#include <sstream>

using namespace DB;

namespace
{
Poco::AutoPtr<Poco::Util::XMLConfiguration> makeConfig(const std::string & inner)
{
    std::istringstream iss("<clickhouse><disk>" + inner + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(iss);
}
const auto identity_macros = [](const std::string & s) { return s; };
}

TEST(ContentAddressedSettings, DefaultsAndOverridesLand)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shards>4</gc_shards>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data/default_scratch", identity_macros);
    EXPECT_EQ(s[ContentAddressedSetting::gc_shards].value, 4u);
    EXPECT_EQ(s[ContentAddressedSetting::gc_interval_sec].value, 60u);          /// table default
    EXPECT_EQ(s[ContentAddressedSetting::dedup_cache_bytes].value, 64ULL << 20); /// table default
    EXPECT_EQ(s[ContentAddressedSetting::scratch_path].value, "/data/default_scratch");
}

TEST(ContentAddressedSettings, UnknownKeyRejected)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shardz>4</gc_shardz>");
    ContentAddressedSettings s;
    EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
}

TEST(ContentAddressedSettings, ObjectStorageKeysSkipped)
{
    auto cfg = makeConfig(
        "<metadata_type>content_addressed</metadata_type><type>object_storage</type>"
        "<object_storage_type>s3</object_storage_type><endpoint>http://x/y</endpoint>"
        "<access_key_id>k</access_key_id><secret_access_key>s</secret_access_key>"
        "<server_root_id>srv1</server_root_id>");
    ContentAddressedSettings s;
    EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros));
}

TEST(ContentAddressedSettings, ValidateFailsClosed)
{
    {   /// missing server_root_id
        auto cfg = makeConfig("<gc_shards>1</gc_shards>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
    }
    {   /// zero gc_shards
        auto cfg = makeConfig("<server_root_id>srv1</server_root_id><gc_shards>0</gc_shards>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
    }
    {   /// unknown blob_hash spelling
        auto cfg = makeConfig("<server_root_id>srv1</server_root_id><blob_hash>md5</blob_hash>");
        ContentAddressedSettings s;
        EXPECT_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", identity_macros), Exception);
    }
}

TEST(ContentAddressedSettings, RelativeScratchPathAnchored)
{
    auto cfg = makeConfig("<server_root_id>srv1</server_root_id><scratch_path>rel/dir</scratch_path>");
    ContentAddressedSettings s;
    s.loadFromConfig(*cfg, "disk", "/data/default_scratch", identity_macros);
    /// Relative override anchored to the server data path prefix passed by the caller, never CWD.
    EXPECT_TRUE(s[ContentAddressedSetting::scratch_path].value.starts_with("/"));
}
```

Register the file in `src/Disks/tests/` the same way sibling `gtest_cas_*` files are (they are glob-collected into `unit_tests_dbms`; confirm by checking how `gtest_cas_request_control.cpp` is built — if a CMake list enumerates files, append there).

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/task_5_build.log 2>&1
```
Expected: FAIL to compile — `ContentAddressedSettings.h` does not exist.

- [ ] **Step 3: Implement the settings struct**

`ContentAddressedSettings.h`: mirror `src/Interpreters/FileCache/FileCacheSettings.h` shape exactly (`CONTENT_ADDRESSED_SETTINGS_SUPPORTED_TYPES(CLASS_NAME, M)` with `M(CLASS_NAME, String)`, `M(CLASS_NAME, Bool)`, `M(CLASS_NAME, UInt64)`; `DECLARE_SETTING_TRAIT` / `DECLARE_SETTING_SUBSCRIPT_OPERATOR`; pimpl `ContentAddressedSettingsImpl`).

`ContentAddressedSettings.cpp`: macro table with the 22 keys and defaults copied from the current factory lambda (`MetadataStorageFactory.cpp:214-333`) — each `DECLARE(Type, name, default, "description", 0)`; carry over the existing per-key rationale comments as `///` lines above their `DECLARE` rows:

```cpp
#define LIST_OF_CONTENT_ADDRESSED_SETTINGS(DECLARE, ALIAS) \
    DECLARE(String, scratch_path, "", "Server-local scratch dir for the write-buffer spill; a relative value is anchored to the server data path", 0) \
    DECLARE(Bool,   gc_enabled, true, "Run the background GC scheduler on this disk", 0) \
    DECLARE(UInt64, gc_interval_sec, 60, "Seconds between background GC rounds (>= 1)", 0) \
    DECLARE(String, blob_hash, "cityhash128", "Pool blob content-hash function (cityhash128 | xxh3-128 | sha256); fixed at pool creation", 0) \
    DECLARE(Bool,   blob_hash_allow_new, false, "Explicit opt-in to admit a NEW hash algo into an existing pool's algos_used", 0) \
    DECLARE(Bool,   skip_access_check, false, "Skip the boot-time capability probe (start now, fix later)", 0) \
    DECLARE(UInt64, dedup_cache_bytes, 64ULL << 20, "Byte budget of the blob presence cache (0 disables)", 0) \
    DECLARE(UInt64, dedup_head_first_min_bytes, 1ULL << 20, "Minimum blob size to try a HEAD before uploading the body", 0) \
    DECLARE(UInt64, gc_snap_generations_to_keep, 3, "GC snapshot generations retained", 0) \
    DECLARE(UInt64, gc_shards, 1, "Blob-hash-prefix reducer shards (>= 1); creation-time only", 0) \
    DECLARE(UInt64, manifest_sweep_list_budget_keys, 1000, "Orphan-manifest sweep LIST budget per round", 0) \
    DECLARE(UInt64, manifest_sweep_delete_budget_keys, 100, "Orphan-manifest sweep DELETE budget per round", 0) \
    DECLARE(String, server_root_id, "", "REQUIRED explicit layout subtree identity; macros expand as in the s3 endpoint", 0) \
    DECLARE(UInt64, gcs_max_conditional_put_bytes, 1ULL << 30, "GCS single-PUT budget for conditional writes (generation-token stores only)", 0) \
    DECLARE(UInt64, part_folder_cache_bytes, 64ULL << 20, "Part-folder view cache byte budget (0 disables retention)", 0) \
    DECLARE(UInt64, part_folder_cache_max_entries, 10000, "Part-folder view cache entry cap", 0) \
    DECLARE(UInt64, part_folder_cache_max_entry_bytes, 16ULL << 20, "Oversized part-folder views bypass retention above this size", 0) \
    DECLARE(String, part_folder_validate, "always", "ForceFresh body re-proof policy (always | ...same forms parsePartFolderValidate accepts today)", 0) \
    DECLARE(UInt64, manifest_decode_cache_bytes, 128ULL << 20, "Manifest DECODE cache byte budget (0 disables)", 0) \
    DECLARE(UInt64, gc_meta_pool_size, 16, "Bounded pool size for GC per-hash freshness-meta writes", 0) \
    DECLARE(UInt64, materialization_grace_ms, 30000, "Post-reclaim wait when opening over an unclean predecessor", 0) \
    DECLARE(String, staging_backend, "local", "Blob staging backend (local | s3); s3 is opt-in", 0) \
```

(Before finalizing, diff this list against the actual factory lambda: every `config.getX(config_prefix + ".key", ...)` call must have exactly one row, defaults byte-identical. The lambda is authoritative.)

`loadFromConfig`: FileCacheSettings-style key loop with a skip-set:

```cpp
/// Keys in the same disk block that belong to the disk/object-storage layers, not CAS.
/// Seed list built from utils/ca-soak/configs/*.xml and tests/integration CA configs at
/// implementation time (enumerate with:
///   rg -o "<([a-z_]+)>" -r '$1' utils/ca-soak/configs/*.xml | sort -u
/// plus the equivalent over the integration-test CA disk configs) — verify every enumerated
/// key is either a CAS setting or in this set. The rejection message tells the operator to
/// add genuinely new object-storage keys here.
static const std::set<std::string> non_cas_keys = {
    "type", "object_storage_type", "metadata_type", "endpoint", "access_key_id",
    "secret_access_key", "region", "use_environment_credentials", "readonly",
    "s3_check_objects_after_upload", "request_timeout_ms", "skip_access_check_on_disk",
    /* ...complete during implementation from the enumeration above... */};
```

Note `skip_access_check` is a CAS setting AND a generic disk key; CAS consumes it (current behavior — the factory reads it directly because `IDisk::startupImpl` drops the flag before `metadata_storage->startup()` runs; keep that comment). `validate()` performs the three fail-closed checks + parses the three enum strings via the existing `Cas::parseBlobHashAlgo` and the two `ContentAddressedMetadataStorage::parse*` helpers refactored to take a `const String &` (move their string-parsing bodies here or call the refactored statics). `loadFromConfig` tail: anchor/deflault `scratch_path`, expand macros in `server_root_id` via the passed `MacroExpander`, then `validate()`.

- [ ] **Step 4: Run tests to verify pass**

```bash
ninja -C build unit_tests_dbms > build/task_5_build2.log 2>&1
build/src/unit_tests_dbms --gtest_filter='ContentAddressedSettings*' > build/task_5_test.log 2>&1
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
        src/Disks/tests/gtest_content_addressed_settings.cpp
git add -u src
git commit -m "cas: ContentAddressedSettings — declarative BaseSettings table with unknown-key rejection (F4a)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: F4b — rewire the factory and collapse the constructor

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:214-333`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` (constructor)
- Modify: every in-tree direct construction site (find with `rg -ln "ContentAddressedMetadataStorage\(" src` — expect several under `src/Disks/tests/`)

**Interfaces:**
- Consumes: Task 5's `ContentAddressedSettings` exactly as declared there.
- Produces: the new constructor

```cpp
ContentAddressedMetadataStorage(
    ObjectStoragePtr object_storage_,
    String storage_path_prefix_,
    String server_id_,
    String disk_name_,
    ContextPtr context_,               /// may be null in tests (disables scheduling/logs, as today)
    const ContentAddressedSettings & settings_);
```

- [ ] **Step 1: Collapse the constructor**

Replace the ~28-positional-parameter constructor with the 6-argument form above; delete every header-side parameter default (the settings table is now the single source). The ctor body maps `settings_` fields to the same members / `Cas::PoolConfig` fields the old positional parameters fed — a pure renaming of sources, no value changes. `server_root_id` and `local_scratch_path` now come from `settings_` (`server_root_id`, `scratch_path`).

- [ ] **Step 2: Shrink the factory lambda**

`registerContentAddressedMetadataStorage` becomes:

```cpp
        checkSingleLocation(cluster);
        const auto local_object_storage = object_storages->takePointingTo(cluster->getLocalLocation());
        std::string key_compatibility_prefix = getObjectKeyCompatiblePrefix(local_object_storage, config, config_prefix);

        auto global_context = Context::getGlobalContextInstance();
        ContentAddressedSettings settings;
        settings.loadFromConfig(
            config, config_prefix,
            /*default_scratch_path=*/ fs::path(global_context->getPath()) / "disks" / name / "cas_scratch" / "",
            [&](const std::string & s) { return global_context->getMacros()->expand(s); });
        fs::create_directories(settings[ContentAddressedSetting::scratch_path].value);

        return std::make_shared<ContentAddressedMetadataStorage>(
            local_object_storage, key_compatibility_prefix, toString(ServerUUID::get()),
            name, global_context, settings);
```
Block-level design comments worth keeping (multi-writer pool rationale, `skip_access_check` boot-time note) move to the settings table or the ctor; nothing rationale-bearing is deleted.

- [ ] **Step 3: Update all direct construction sites**

For each hit of `rg -ln "ContentAddressedMetadataStorage\(" src` (tests included): build a `ContentAddressedSettings` (defaults) with per-test overrides via `settings[ContentAddressedSetting::x] = value;`, then call the 6-arg ctor. Old positional argument N maps to the table row with the same name — the old header is in git history if disambiguation is needed.

- [ ] **Step 4: Build everything + run the CA gates**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_6_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedSettings*' > build/task_6_test.log 2>&1
```
Expected: PASS. Then confirm the duplication is gone: `rg -n "= 64ULL << 20|= 30000|= 10000" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` → no default-value hits.

- [ ] **Step 5: Run one CA integration smoke (config-driven path)**

```bash
python -m ci.praktika run "integration" --test test_content_addressed  > build/task_6_integration.log 2>&1 || true
```
Use the CA integration test name found via `rg -l content_addressed tests/integration | head`; if no runnable environment, state so in the report — the final gate (Task 14) covers it.

- [ ] **Step 6: Commit**

```bash
git add -u src
git commit -m "cas: factory + constructor on ContentAddressedSettings; 25 positional args and duplicated defaults removed (F4b)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: F5a — generic single-attempt retry profile in the S3 layer

**Files:**
- Modify: `src/IO/WriteSettings.h`
- Modify: `src/IO/S3/Client.h`, `src/IO/S3/Client.cpp` (new `S3::SingleAttemptRetryStrategy`)
- Modify: `src/Common/ProfileEvents.cpp` (new generic event)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.{h,cpp}`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` (`supportsRetryProfile`)

**Interfaces:**
- Produces:
  - `enum class ObjectStorageRetryProfile : uint8_t { Default, SingleAttempt };` + `ObjectStorageRetryProfile object_storage_retry_profile = ObjectStorageRetryProfile::Default;` in `WriteSettings` (replacing nothing yet — the old field is removed in Task 8).
  - `IObjectStorage::supportsRetryProfile(ObjectStorageRetryProfile) const` → default `profile == Default`; `S3ObjectStorage` overrides to `true`.
  - `S3::SingleAttemptRetryStrategy` (in `namespace DB::S3`), incrementing new ProfileEvent `S3SingleAttemptRetryConsultations`.
  - `std::shared_ptr<const S3::Client> S3ObjectStorage::getSingleAttemptClient() const` — lazily built, rebuilt when the disk client rotates.
- Task 8 consumes all four.

- [ ] **Step 1: Write the failing strategy test**

In `src/IO/S3/tests/gtest_aws_s3_client.cpp` add:

```cpp
TEST(IOTestAwsS3Client, SingleAttemptRetryStrategyRefusesAndCounts)
{
    using ProfileEvents::global_counters;
    const auto before = global_counters[ProfileEvents::S3SingleAttemptRetryConsultations].load();
    DB::S3::SingleAttemptRetryStrategy strategy;
    const Aws::Client::AWSError<Aws::Client::CoreErrors> retryable_5xx(
        Aws::Client::CoreErrors::INTERNAL_FAILURE, /*isRetryable=*/true);
    EXPECT_FALSE(strategy.ShouldRetry(retryable_5xx, /*attempted=*/0));
    EXPECT_EQ(strategy.GetMaxAttempts(), 1);
    EXPECT_EQ(global_counters[ProfileEvents::S3SingleAttemptRetryConsultations].load() - before, 1u);
}
```
(Mirror the existing `Cas::detail` test in `src/Disks/tests/gtest_cas_request_control.cpp:~240` — same shape, generic seam.)

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/task_7_build.log 2>&1
```
Expected: compile FAIL — `S3::SingleAttemptRetryStrategy` / event undefined.

- [ ] **Step 3: Implement**

`src/Common/ProfileEvents.cpp`, next to the S3 events:

```cpp
    M(S3SingleAttemptRetryConsultations, "Number of AWS SDK retry consultations refused by the single-attempt S3 retry profile. Non-zero means the SDK attempted a transparent retry on a write that must make exactly one HTTP attempt.", ValueType::Number) \
```

`src/IO/S3/Client.h` (public, near `RetryStrategy`):

```cpp
/// Refuses every SDK-transparent retry and counts each consultation. Used by the
/// ObjectStorageRetryProfile::SingleAttempt per-write profile (conditional writes whose retry
/// decisions live ABOVE the SDK: the caller must resolve an uncertain PUT before reissuing).
class SingleAttemptRetryStrategy final : public Aws::Client::RetryStrategy
{
public:
    bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const override; // NOLINT(google-runtime-int)
    long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const override { return 0; } // NOLINT(google-runtime-int)
    long GetMaxAttempts() const override { return 1; } // NOLINT(google-runtime-int)
};
```
`Client.cpp`: `ShouldRetry` increments `ProfileEvents::S3SingleAttemptRetryConsultations` and returns `false` (copy the body from `Cas::detail::SingleAttemptRetryStrategy` in `CasObjectStorageBackend.cpp`, swapping the event).

`src/IO/WriteSettings.h` (backend-neutral, top of file after includes):

```cpp
/// Per-write retry-behavior selector, resolved by the object storage that executes the write.
/// SingleAttempt: exactly one HTTP attempt, no SDK-transparent retries — for conditional writes
/// whose retry loop lives above the storage client (it must resolve an uncertain PUT before
/// reissuing). Backends without a SingleAttempt implementation report it via
/// IObjectStorage::supportsRetryProfile; writers must fail closed rather than fall through.
enum class ObjectStorageRetryProfile : uint8_t
{
    Default,
    SingleAttempt,
};
```
and the member `ObjectStorageRetryProfile object_storage_retry_profile = ObjectStorageRetryProfile::Default;` next to the other override fields.

`src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h`:

```cpp
    /// True when this object storage can execute writes under the given retry profile.
    /// A caller that sets a non-Default profile on WriteSettings MUST check this first and
    /// fail closed if unsupported (the profile is advisory only to backends that opt in).
    virtual bool supportsRetryProfile(ObjectStorageRetryProfile profile) const { return profile == ObjectStorageRetryProfile::Default; }
```

`S3ObjectStorage.h`: `bool supportsRetryProfile(ObjectStorageRetryProfile) const override { return true; }` plus:

```cpp
    /// Lazily-built clone of the current disk client with the single-attempt retry profile
    /// (SingleAttemptRetryStrategy, max_retries=0, Expect:100-continue floor). Rebuilt whenever the
    /// disk client rotates (applyNewSettings/credentials refresh) — the cached clone is keyed by the
    /// base client's identity, so a stale clone can never outlive a rotation.
    std::shared_ptr<const S3::Client> getSingleAttemptClient() const;
private:
    mutable std::mutex single_attempt_client_mutex;
    mutable std::shared_ptr<const S3::Client> single_attempt_client;
    mutable const S3::Client * single_attempt_client_base = nullptr;
```

`S3ObjectStorage.cpp`:

```cpp
std::shared_ptr<const S3::Client> S3ObjectStorage::getSingleAttemptClient() const
{
    auto base = client.get();
    std::lock_guard lock(single_attempt_client_mutex);
    if (single_attempt_client && single_attempt_client_base == base.get())
        return single_attempt_client;

    auto cfg = base->getClientConfiguration();
    cfg.retry_strategy.max_retries = 0;
    cfg.retryStrategy = std::make_shared<S3::SingleAttemptRetryStrategy>();
    /// A server can reject an If-Match/If-None-Match request before accepting its body; waiting for
    /// the 100-continue response avoids uploading a large body that cannot commit. Respect the
    /// disk's configured expect_continue_min_bytes; if unset, use the established 1 MiB floor.
    static constexpr uint64_t fallback_expect_continue_min_bytes = 1024 * 1024;
    if (cfg.expect_continue_min_bytes == 0)
        cfg.expect_continue_min_bytes = fallback_expect_continue_min_bytes;

    single_attempt_client = base->cloneWithConfigurationOverride(cfg);
    single_attempt_client_base = base.get();
    return single_attempt_client;
}
```
In `writeObject`, replace the client selection expression and its comment:

```cpp
    /// The SingleAttempt profile rides on WriteSettings instead of changing this disk's shared
    /// client — every other write keeps using client.get() and its normal retry policy unchanged.
    auto used_client = write_settings.object_storage_retry_profile == ObjectStorageRetryProfile::SingleAttempt
        ? getSingleAttemptClient()
        : client.get();
```
and pass `used_client` to `WriteBufferFromS3`. Update the nearby comment that references `s3_client_override` (the one above `max_unexpected_write_error_retries`) to say "a client-level profile override".

- [ ] **Step 4: Run tests to verify pass**

```bash
ninja -C build unit_tests_dbms > build/task_7_build2.log 2>&1
build/src/unit_tests_dbms --gtest_filter='IOTestAwsS3Client.SingleAttempt*' > build/task_7_test.log 2>&1
```
Expected: PASS. (The old `WriteSettings::s3_client_override` field still exists and compiles — it is removed with its producer in Task 8.)

- [ ] **Step 5: Cover client-rotation staleness (spec Testing item)**

The spec asks for a test that the single-attempt clone is rebuilt after `applyNewSettings` rotates the disk client. Attempt it in `gtest_aws_s3_client.cpp` using the existing `TestPocoHTTPServer` + `ClientFactory` harness: build two distinct clients A and B, construct an `S3ObjectStorage` if its constructor is satisfiable with the test URI/settings, call `getSingleAttemptClient` (expect clone of A), swap the client via `applyNewSettings` or a test seam, call again (expect a DIFFERENT clone, derived from B). If `S3ObjectStorage` construction proves impractical in the unit-test binary, do NOT fake it: state that in the task report and rely on the structural guarantee (the cache is keyed by the base client's pointer identity — a rotated client cannot return a stale clone by construction), which the reviewer must re-verify by reading `getSingleAttemptClient`.

- [ ] **Step 6: Commit**

```bash
git add -u src
git commit -m "cas: generic ObjectStorageRetryProfile + S3 single-attempt client owned by S3ObjectStorage (F5a)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: F5b — CAS backend on the profile; remove `s3_client_override`

**Files:**
- Modify: `src/IO/WriteSettings.h` (delete the override field + `#if USE_AWS_S3` + `S3::Client` fwd-decl + `<memory>` include)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.{h,cpp}`
- Modify: `src/Disks/tests/gtest_cas_request_control.cpp`
- Modify: `src/Common/ProfileEvents.cpp` (delete `CasConditionalWriteSdkRetries`), `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.{h,cpp}` (delete `recordConditionalWriteSdkRetryConsidered`)

**Interfaces:**
- Consumes: Task 7's `ObjectStorageRetryProfile`, `supportsRetryProfile`, `S3::SingleAttemptRetryStrategy`.

- [ ] **Step 1: Switch the backend**

`CasObjectStorageBackend.cpp`:
- Delete the ctor block that builds `single_attempt_s3_client` (lines ~85-105) and the member `single_attempt_s3_client` (`.h:204`), plus the now-unused `Cas::detail::SingleAttemptRetryStrategy` class (`.h:40-48`) and its `.cpp` bodies.
- `conditionalWriteSettings()`: replace the `#if USE_AWS_S3 ... ws.s3_client_override = ...` tail with:

```cpp
    /// Exactly one HTTP attempt for every conditional write: the object storage resolves the
    /// profile to its own single-attempt client. A backend that cannot honor it is rejected for
    /// writable Native mounts by checkConditionalWriteSingleAttemptSupport (fail closed).
    ws.object_storage_retry_profile = ObjectStorageRetryProfile::SingleAttempt;
```
- `checkConditionalWriteSingleAttemptSupport` (~line 143-165): replace the `has_single_attempt_client` derivation with

```cpp
    const bool single_attempt_supported = object_storage->supportsRetryProfile(ObjectStorageRetryProfile::SingleAttempt);
```
keeping the existing throw/fail-closed structure and updating its comment (the property checked is now backend capability, not clone presence).
- Delete `recordConditionalWriteSdkRetryConsidered` (CasRequestControl.{h,cpp}) and the `CasConditionalWriteSdkRetries` event row in `ProfileEvents.cpp`. Sweep references:

```bash
rg -n "CasConditionalWriteSdkRetries|recordConditionalWriteSdkRetryConsidered|s3_client_override|single_attempt_s3_client" src tests utils/ca-soak docs
```
Update `utils/ca-soak` observers/docs that watch the old event to watch `S3SingleAttemptRetryConsultations` (same must-stay-zero contract). Expected end state: zero hits outside reports/worklogs.

- [ ] **Step 2: Rework the gtests**

In `src/Disks/tests/gtest_cas_request_control.cpp`:
- `ClientOverrideAbsentOverNonS3ObjectStorage` becomes `SingleAttemptProfileRequestedAndLocalBackendRejected`:

```cpp
TEST(CasRequestControl, SingleAttemptProfileRequestedAndLocalBackendRejected)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    const auto ws = b->conditionalWriteSettingsForTest();
    EXPECT_EQ(ws.object_storage_retry_profile, ObjectStorageRetryProfile::SingleAttempt);
    /// LocalObjectStorage does not implement the profile — the capability check must say no.
    EXPECT_FALSE(DB::Cas::tests::makeLocalObjectStorageForTest()->supportsRetryProfile(ObjectStorageRetryProfile::SingleAttempt));
}
```
- Delete `SingleAttemptRetryStrategyRefusesAndCountsEveryConsultation` (superseded by the generic test added in Task 7; keep any assertions unique to it by folding them into the Task 7 test if missing).

- [ ] **Step 3: Build + full CA gate**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_8_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:IOTestAwsS3Client.SingleAttempt*' > build/task_8_test.log 2>&1
```
Expected: PASS; `rg -n "USE_AWS_S3" src/IO/WriteSettings.h` → no hits.

- [ ] **Step 4: Commit**

```bash
git add -u src tests utils docs
git commit -m "cas: conditional writes select the retry profile; WriteSettings loses the S3 client pointer (F5b); SDK tripwire -> S3SingleAttemptRetryConsultations

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: F6 — name the CAS background threads

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp` (`loop` ~203, `heartbeatLoop` ~263)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp` (remount lambda ~245)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp` (`SingleWriterSlot::backgroundLoop` ~1031)

**Interfaces:** none (self-contained observability change).

- [ ] **Step 1: Add the names**

Add `#include <Common/setThreadName.h>` to each file and the first statement of each loop body (all names ≤ 15 chars):

```cpp
void CasGcScheduler::loop()
{
    setThreadName("CasGcSched");
    ...
```
`heartbeatLoop` → `setThreadName("CasGcHeartbeat");`; the remount lambda's first statement → `setThreadName("CasRemount");`; `SingleWriterSlot::backgroundLoop` first statement (before taking `background_mutex`) → `setThreadName("CasLeaseKeeper");`.

- [ ] **Step 2: Audit the remaining spawn sites**

```bash
rg -n "ThreadFromGlobalPool" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```
For each hit not covered above (known: `CasRefLedger.cpp` ~1514, `CasPool.cpp` ~885): if the body is a loop, it MUST get a name; if it is a one-shot task, add a short name anyway when a natural one exists (e.g. `CasRefSnapPub` for the snapshot publish) — a one-shot with an inherited misleading name is still confusing in a stack dump. Record the decision per site in the task report.

- [ ] **Step 3: Build + gate**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_9_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/task_9_test.log 2>&1
```
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add -u src
git commit -m "cas: setThreadName for GC scheduler/heartbeat/remount/lease-keeper workers (F6)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: F7 — rewrite the 128 `Cas*` ProfileEvents descriptions

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (the `Cas*` block, ~lines 756-885)

**Interfaces:** none; event NAMES and `ValueType`s must be byte-identical after this task (descriptions only). Note the block will contain 127 events after Task 8 removed `CasConditionalWriteSdkRetries` — rewrite whatever is present.

- [ ] **Step 1: Rewrite every description to the hybrid form**

Transformation rules (apply to each event in the `Cas*` block, one by one — no other lines):
1. First sentence states the quantity in the file's dominant convention: count events → `"Number of ..."`; `ValueType::Microseconds` events → `"Total time ... in microseconds."`; `ValueType::Bytes` events → `"Total bytes ..."`.
2. Keep the interpretive guidance as a SECOND sentence, rephrased as prose (no `"; growing values indicate"` clause form).
3. Keep technical terms and cross-references intact (e.g. the B168 attribution comment above the block stays).

Worked examples (source → target), to be matched in spirit for the rest:

```
"Counts CAS blob PUT requests; grows with blob uploads and indicates storage write traffic."
→ "Number of CAS blob PUT requests. Grows with blob uploads and indicates storage write traffic."

"Counts CAS blob compare-and-swap conflicts; non-zero values indicate concurrent update contention."
→ "Number of CAS blob compare-and-swap conflicts. A non-zero value indicates concurrent update contention."

"Counts cumulative time CAS ref writers spent queued; rising values indicate ref-write contention or backend latency."
→ "Total time CAS ref writers spent queued, in microseconds. A rising value indicates ref-write contention or backend latency."

"Counts bytes written to CAS ref-table snapshots; high values indicate frequent or large snapshot publication."
→ "Total bytes written to CAS ref-table snapshots. A high value indicates frequent or large snapshot publication."
```

This is mechanical-with-judgment: per the standing delegation policy it may be dispatched to codex (`codex exec -m gpt-5.6-luna`, prompt via file) with these rules plus the block; the reviewing session must then diff-check every line for rule compliance and unchanged names/ValueTypes.

- [ ] **Step 2: Verify names/ValueTypes untouched and style applied**

```bash
git diff src/Common/ProfileEvents.cpp | grep '^[+-]' | grep -oP 'M\(\w+' | sort | uniq -c | awk '$1 != 2 {print; exit 1}' && echo NAMES-STABLE
rg -n '"Counts ' src/Common/ProfileEvents.cpp && echo LEFTOVERS || echo CLEAN
```
Expected: `NAMES-STABLE` (every touched event appears exactly once as `-` and once as `+`) and `CLEAN`.

- [ ] **Step 3: Build**

```bash
ninja -C build clickhouse > build/task_10_build.log 2>&1
```
Expected: success.

- [ ] **Step 4: Commit**

```bash
git add -u src/Common/ProfileEvents.cpp
git commit -m "cas: ProfileEvents descriptions to 'Number of ... / Total ...' hybrid style (F7)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: F8 — standard `content_addressed_log` config section

**Files:**
- Modify: `programs/server/config.xml:1198-1208`
- Check/modify: `programs/server/config.yaml.example`; `rg -l "content_addressed_log" utils/ca-soak tests/integration tests/config` copies

**Interfaces:** none.

- [ ] **Step 1: Replace the section**

```xml
    <!-- Per-event audit log of the content-addressed (CAS) storage layer. Enabled by default while
         the feature is experimental (events are emitted only by content-addressed disks; it costs
         nothing otherwise). Remove the section to disable. -->
    <content_addressed_log>
        <database>system</database>
        <table>content_addressed_log</table>
        <partition_by>toYYYYMM(event_date)</partition_by>
        <flush_interval_milliseconds>7500</flush_interval_milliseconds>
        <max_size_rows>1048576</max_size_rows>
        <reserved_size_rows>8192</reserved_size_rows>
        <buffer_size_rows_flush_threshold>524288</buffer_size_rows_flush_threshold>
        <flush_on_crash>false</flush_on_crash>
        <!-- example of a retention policy; disabled by default like every other log's TTL
        <ttl>event_date + INTERVAL 7 DAY DELETE</ttl>
        -->
    </content_addressed_log>
```
Values for the standard keys copied from the `content_addressed_garbage_collection_log` sibling (`config.xml:1316-1325`). Keep the section where it is (before `query_log`).

- [ ] **Step 2: Align copies**

For each `content_addressed_log` section found in `config.yaml.example`, ca-soak configs, and integration-test configs: apply the same key set (or confirm the copy intentionally minimal — e.g. a test fixture that only overrides `flush_interval_milliseconds` may stay; state the decision per file).

- [ ] **Step 3: Validate XML**

```bash
xmllint --noout programs/server/config.xml && echo XML-OK
```
Expected: `XML-OK`.

- [ ] **Step 4: Commit**

```bash
git add -u programs utils tests
git commit -m "cas: content_addressed_log config section gets the standard key set; TTL becomes a commented example (F8)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 12: F9 — `s3_check_objects_after_upload_override`

**Files:**
- Modify: `src/IO/WriteSettings.h:41`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:315-316`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` (`conditionalWriteSettings`)
- Modify: any other producer/consumer found by `rg -n "s3_skip_check_objects_after_upload" src`

**Interfaces:**
- Produces: `std::optional<bool> s3_check_objects_after_upload_override;` in `WriteSettings` (tri-state; `std::nullopt` = no override, symmetric with the sibling `*_override` fields).

- [ ] **Step 1: Rename with polarity fix**

`WriteSettings.h` — replace the `bool s3_skip_check_objects_after_upload = false;` field and move its rationale comment:

```cpp
    /// Overrides S3RequestSetting::check_objects_after_upload for this write (nullopt = no
    /// override). Writers of CAS-MUTABLE keys (content-addressed shard manifests) set `false`:
    /// such a key is legitimately replaced by a concurrent conditional PUT between this upload and
    /// the check's HEAD, so the size comparison false-positives ("it's a bug in S3") under normal
    /// contention. Integrity for those keys is the conditional PUT outcome + token, not a recheck.
    std::optional<bool> s3_check_objects_after_upload_override;
```
`S3ObjectStorage.cpp`:

```cpp
    if (write_settings.s3_check_objects_after_upload_override)
        request_settings[S3RequestSetting::check_objects_after_upload] = *write_settings.s3_check_objects_after_upload_override;
```
`CasObjectStorageBackend.cpp` `conditionalWriteSettings`: `ws.s3_check_objects_after_upload_override = false;` (comment above it stays, trimmed of the parts moved to WriteSettings.h).

- [ ] **Step 2: Sweep + build + gate**

```bash
rg -n "s3_skip_check_objects_after_upload" src tests && echo LEFTOVERS || echo CLEAN
ninja -C build clickhouse unit_tests_dbms > build/task_12_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/task_12_test.log 2>&1
```
Expected: `CLEAN`, build OK, gate PASS.

- [ ] **Step 3: Commit**

```bash
git add -u src tests
git commit -m "cas: s3_skip_check_objects_after_upload -> optional s3_check_objects_after_upload_override (F9: same polarity as the setting it overrides)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 13: F10 — shared S3 error-name predicates in `S3Common`

**Files:**
- Modify: `src/IO/S3Common.h`, `src/IO/S3Common.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:44-77`

**Interfaces:**
- Produces (in `namespace DB::S3`, `#if USE_AWS_S3`-guarded like the surrounding declarations):

```cpp
/// Error-name classifiers shared by S3Exception::isRetryableError and the CAS conditional-write
/// outcome mapping — one maintained list per family, so the two consumers cannot drift.
bool isMalformedRequestError(const S3Exception & e);
bool isEntityTooLargeError(const S3Exception & e);
bool isAccessDeniedError(const S3Exception & e);
```

- [ ] **Step 1: Move the predicates**

Move the three function bodies from the anonymous namespace of `CasRequestControl.cpp` (lines 51-77) verbatim into `S3Common.cpp` (renamed with the `Error` suffix as declared above; keep their comments, including the "name-first matching" rationale). Declare them in `S3Common.h` next to `isPreconditionFailedError`, with the drift-prevention comment. Add the required `#include <aws/s3/S3Errors.h>` to `S3Common.cpp` if not already present.

- [ ] **Step 2: Repoint the CAS classifier**

`CasRequestControl.cpp` — `classifyConditionalWriteResult` now reads:

```cpp
        if (S3::isMalformedRequestError(*s3e) || S3::isEntityTooLargeError(*s3e) || S3::isAccessDeniedError(*s3e))
            return CasWriteOutcome::DefiniteFailure;
```
and the local anonymous-namespace copies are deleted. `S3Exception::isRetryableError` is NOT touched (spec non-goal — different contract).

- [ ] **Step 3: Build + tests**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_13_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasRequestControl*' > build/task_13_test.log 2>&1
```
Expected: PASS (the existing classification tests pin behavior across the move).

- [ ] **Step 4: Commit**

```bash
git add -u src
git commit -m "cas: S3 error-name predicates shared via S3Common (F10: one list per family, no drift with isRetryableError)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 14: F11 — cache CurrentMetrics + final regression gate

**Files:**
- Modify: `src/Common/CurrentMetrics.cpp:233-234` area
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasManifestReader.cpp:30-34`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:116-119`

**Interfaces:** none beyond the new metric names.

- [ ] **Step 1: Declare the metrics**

`src/Common/CurrentMetrics.cpp`, next to the existing `CasPartFolderCache*` pair:

```cpp
    M(CasManifestDecodeCacheBytes, "Bytes retained by the CA manifest decode cache") \
    M(CasManifestDecodeCacheEntries, "Entries retained by the CA manifest decode cache") \
    M(CasDedupCacheBytes, "Bytes retained by the CA blob presence (dedup) cache") \
    M(CasDedupCacheEntries, "Entries retained by the CA blob presence (dedup) cache") \
```

- [ ] **Step 2: Wire them**

`CasManifestReader.cpp` — add the `namespace CurrentMetrics { extern const Metric CasManifestDecodeCacheBytes; extern const Metric CasManifestDecodeCacheEntries; }` declarations (matching the file's existing convention) and change the constructor call:

```cpp
        manifest_cache = std::make_unique<ManifestDecodeCache>(
            "LRU", CurrentMetrics::CasManifestDecodeCacheBytes, CurrentMetrics::CasManifestDecodeCacheEntries,
            manifest_decode_cache_bytes, /*max_count=*/16384, ManifestDecodeCache::DEFAULT_SIZE_RATIO);
```
`CasPool.cpp` — same shape with `CasDedupCacheBytes`/`CasDedupCacheEntries` replacing the two `CurrentMetrics::end()` arguments.

- [ ] **Step 3: Build + FULL final gate for the whole plan**

```bash
ninja -C build clickhouse unit_tests_dbms > build/task_14_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedSettings*:IOTestAwsS3Client.*' > build/task_14_test.log 2>&1
```
Then the CA stateless lane (covers the renamed 0500x tests and `01271_show_privileges`):

```bash
python -m ci.praktika run "stateless" --test "01271_show_privileges 05007_content_addressed_gc_introspection 05008_ca_gc_snap_prune 05010_content_addressed_mounts_gc_health 05011_cas_gc_rebuild_access" > build/task_14_stateless.log 2>&1
```
And a phase-1 ca-soak smoke per the runbook (`utils/ca-soak`, `--ops` mode — see `reference_ca_soak_duration_phase3`/README there). All three must be green; any red gets an RCA before commit (no known-reds rule).

- [ ] **Step 4: Commit**

```bash
git add -u src
git commit -m "cas: CurrentMetrics for the manifest-decode and dedup caches (F11) — closes the F1-F11 consistency pass

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```
