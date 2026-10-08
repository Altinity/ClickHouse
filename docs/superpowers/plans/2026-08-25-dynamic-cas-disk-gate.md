---
description: 'Implementation plan for disabling dynamic CAS disks by default behind a server-owned safety gate'
sidebar_label: 'Dynamic CAS disk gate'
sidebar_position: 9
slug: /superpowers/plans/dynamic-cas-disk-gate
title: 'Dynamic CAS disk gate implementation plan'
doc_type: 'guide'
---

# Dynamic CAS Disk Gate Implementation Plan {#dynamic-cas-disk-gate-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Disable SQL-defined dynamic `CAS` disks by default, while retaining an explicit server-owned opt-in for isolated test environments.

**Architecture:** `RegisterDiskObjectStorage` uses the existing `custom_disk` factory argument to reject an explicit `metadata_type=cas` before any disk construction side effect unless the new `cas_allow_unsafe_dynamic_disks` server setting is enabled. The shared stateless test configuration opts in centrally; server-configured disks are unaffected.

**Tech Stack:** C++ (`DiskFactory`, `ServerSettings`, Poco configuration), gtest (`unit_tests_dbms`), pytest integration tests, Praktika stateless tests, Markdown documentation.

**Spec:** [Dynamic CAS disk gate design](../specs/2026-08-25-dynamic-cas-disk-gate-design.md)

## Global Constraints {#global-constraints}

- The branch is exactly `cas-gc-rebuild` for the entire execution. Before the first edit and
  immediately before and after every commit, require
  `test "$(git rev-parse --abbrev-ref HEAD)" = cas-gc-rebuild`. Do not run `git checkout`,
  `git switch`, create a worktree, rebase, or amend.
- Keep the change limited to `metadata_type=cas`. Do not alter privileges or other metadata types.
- Run the rejection before object-storage construction, filesystem creation, metadata mounting, or background-task startup.
- Preserve current behavior exactly when `cas_allow_unsafe_dynamic_disks=true` and for every disk declared in `storage_configuration`.
- Use Allman braces and refer to functions without trailing parentheses in prose and comments.
- Every build and test command writes to a unique log under `build/`. Dispatch a subagent to read each log and return only a concise pass/fail summary and the first error when present.
- Never pass `-j` to ninja and never use `nproc`.
- Stage only the explicit files named by the task; the worktree contains unrelated user changes.

---

## Task 1: Implement the server-owned gate with TDD {#task-1-implement-the-server-owned-gate-with-tdd}

**Files:**

- Modify: `src/Core/ServerSettings.cpp`
- Modify: `src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp`
- Modify: `src/Disks/tests/gtest_disk_object_storage.cpp`
- Create: `tests/integration/test_cas_dynamic_disk_gate/__init__.py`
- Create: `tests/integration/test_cas_dynamic_disk_gate/test.py`
- Create: `tests/config/config.d/cas_dynamic_disks.xml`
- Modify: `tests/config/install.sh`

- [ ] **Step 1: Extend the fixture with a valid local `CAS` disk configuration**

Add this sibling of `local_object_storage_disk` inside the fixture's `storage_configuration.disks`:

```xml
<cas_object_storage_disk>
    <type>object_storage</type>
    <object_storage_type>local_blob_storage</object_storage_type>
    <path>cas_dynamic_disk_pool/</path>
    <metadata_type>cas</metadata_type>
    <cas_server_root_id>cas-dynamic-disk-test</cas_server_root_id>
    <cas_scratch_path>cas_dynamic_disk_scratch/</cas_scratch_path>
</cas_object_storage_disk>
```

Extend `TearDown` with:

```cpp
fs::remove_all("./cas_dynamic_disk_pool");
fs::remove_all("./cas_dynamic_disk_scratch");
```

- [ ] **Step 2: Add precise default-off scope tests**

Declare `BAD_ARGUMENTS` next to the existing error code declaration:

```cpp
extern const int BAD_ARGUMENTS;
```

Give `getDiskObjectStorage` a `custom_disk` parameter that defaults to `true`, and forward it to
`DiskFactory::create` instead of the current literal `true`:

```cpp
DB::DiskPtr getDiskObjectStorage(
    const std::string & name = "local_object_storage_disk",
    bool custom_disk = true)
```

```cpp
            /*custom_disk*/ custom_disk,
```

Add these tests before `CreateDisk`:

```cpp
TEST_F(DiskObjectStorageTest, DynamicCasDiskRequiresServerOptIn)
{
    bool was_rejected = false;
    try
    {
        getDiskObjectStorage("cas_object_storage_disk");
    }
    catch (const DB::Exception & e)
    {
        was_rejected = true;
        EXPECT_EQ(e.code(), DB::ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(e.message().find("cas_allow_unsafe_dynamic_disks"), std::string::npos);
    }

    EXPECT_TRUE(was_rejected);
    EXPECT_FALSE(fs::exists("./cas_dynamic_disk_pool"));
    EXPECT_FALSE(fs::exists("./cas_dynamic_disk_scratch"));
}

TEST_F(DiskObjectStorageTest, ServerConfiguredCasDiskDoesNotRequireOptIn)
{
    auto disk = getDiskObjectStorage("cas_object_storage_disk", /*custom_disk*/ false);
    EXPECT_TRUE(disk->isDisk());
    EXPECT_EQ(disk->getName(), "cas_object_storage_disk");
}
```

- [ ] **Step 3: Add a black-box test for the SQL `disk(...)` path**

Create an empty `tests/integration/test_cas_dynamic_disk_gate/__init__.py` and add
`tests/integration/test_cas_dynamic_disk_gate/test.py`:

```python
import pytest

from helpers.cluster import ClickHouseCluster


cluster = ClickHouseCluster(__file__)
node = cluster.add_instance("node")

POOL_PATH = "/var/lib/clickhouse/cas_dynamic_disk_gate_pool/"
SCRATCH_PATH = "/var/lib/clickhouse/cas_dynamic_disk_gate_scratch/"


@pytest.fixture(scope="module", autouse=True)
def started_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def path_exists(path):
    return (
        node.exec_in_container(
            ["bash", "-c", f"if test -e {path}; then echo 1; else echo 0; fi"]
        ).strip()
        == "1"
    )


def test_dynamic_cas_disk_is_disabled_without_server_opt_in():
    assert (
        node.query(
            "SELECT toBool(value) FROM system.server_settings "
            "WHERE name = 'cas_allow_unsafe_dynamic_disks'"
        ).strip()
        == "0"
    )

    assert not path_exists(POOL_PATH)
    assert not path_exists(SCRATCH_PATH)

    error = node.query_and_get_error(
        f"""
        CREATE TABLE dynamic_cas_gate (key UInt64)
        ENGINE = MergeTree
        ORDER BY key
        SETTINGS disk = disk(
            type = object_storage,
            object_storage_type = local,
            metadata_type = cas,
            name = 'dynamic_cas_gate',
            path = '{POOL_PATH}',
            cas_server_root_id = 'dynamic-cas-gate',
            cas_scratch_path = '{SCRATCH_PATH}')
        """
    )

    table_exists = node.query("EXISTS TABLE dynamic_cas_gate").strip()
    pool_exists = path_exists(POOL_PATH)
    scratch_exists = path_exists(SCRATCH_PATH)
    assert (
        "BAD_ARGUMENTS" in error
        and "cas_allow_unsafe_dynamic_disks" in error
        and table_exists == "0"
        and not pool_exists
        and not scratch_exists
    ), {
        "error": error,
        "table_exists": table_exists,
        "pool_exists": pool_exists,
        "scratch_exists": scratch_exists,
    }
```

The node intentionally has no config enabling the new setting. This test must remain black-box: do
not call `DiskFactory` or `DiskFromAST` from it.

- [ ] **Step 4: Declare the default-off server setting without adding the gate yet**

Add this entry directly after `cas_blob_upload_pool_size` in
`LIST_OF_SERVER_SETTINGS_WITHOUT_PATH`:

```cpp
DECLARE(Bool, cas_allow_unsafe_dynamic_disks, false, R"(
Allow defining content-addressed (`CAS`) disks through the SQL `disk(...)` function.
Disabled by default because a dynamic `CAS` disk joins a process-wide shared pool, may use server
credentials, and starts background work. Configure `CAS` disks in `storage_configuration` on
multi-user servers.
)", 0) \
```

- [ ] **Step 5: Build both binaries for the red phase**

```bash
ninja -C build unit_tests_dbms > build/build_dynamic_cas_disk_gate_red_unit.log 2>&1
ninja -C build clickhouse > build/build_dynamic_cas_disk_gate_red_server.log 2>&1
```

Dispatch one subagent per log and require both builds to succeed.

- [ ] **Step 6: Run both negative tests and verify the intended red failures**

```bash
build/src/unit_tests_dbms \
    --gtest_filter='DiskObjectStorageTest.DynamicCasDiskRequiresServerOptIn' \
    > build/test_dynamic_cas_disk_gate_red_unit.log 2>&1
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run integration --test test_cas_dynamic_disk_gate \
    > build/test_dynamic_cas_disk_gate_red_integration.log 2>&1
```

Dispatch one subagent per log. The unit test must fail because the valid dynamic `CAS` disk is
constructed. The integration pytest summary must report the black-box test failed because the SQL
creation did not return `BAD_ARGUMENTS` and produced construction side effects. Praktika's
integration wrapper can exit zero when pytest fails, so its pytest summary—not the process exit
code—is the verdict. If either test fails earlier because its fixture is invalid, fix the fixture
before proceeding.

- [ ] **Step 7: Reject a dynamic `CAS` disk before construction starts**

In `RegisterDiskObjectStorage.cpp`, include the two headers that own the setting and the complete
`Context` interface:

```cpp
#include <Core/ServerSettings.h>
#include <Interpreters/Context.h>
```

Add the generated setting declaration:

```cpp
namespace ServerSetting
{
    extern const ServerSettingsBool cas_allow_unsafe_dynamic_disks;
}
```

Name the creator's last two arguments and insert the gate as the first executable code in the
lambda:

```cpp
        bool /* attach */,
        bool custom_disk) -> DiskPtr
    {
        const auto configured_metadata_type = config.getString(config_prefix + ".metadata_type", "");
        if (custom_disk
            && configured_metadata_type == "cas"
            && !context->getServerSettings()[ServerSetting::cas_allow_unsafe_dynamic_disks])
        {
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "Dynamic `CAS` disks are disabled because they join a process-wide shared storage "
                "pool and may use server credentials. Configure the `CAS` disk in "
                "`storage_configuration`, or explicitly enable "
                "`cas_allow_unsafe_dynamic_disks` in the server configuration");
        }

        const bool skip_access_check = // existing code follows
```

Do not derive the type by constructing metadata storage. The early explicit string check matches
the exact key accepted by `MetadataStorageFactory` and is what keeps the rejection free of side
effects.

- [ ] **Step 8: Opt the shared stateless test configuration into dynamic `CAS` disks**

Create `tests/config/config.d/cas_dynamic_disks.xml`:

```xml
<clickhouse>
    <!-- Stateless CAS tests intentionally construct isolated dynamic local pools. -->
    <cas_allow_unsafe_dynamic_disks>true</cas_allow_unsafe_dynamic_disks>
</clickhouse>
```

Install it for test binaries that know the new server setting by adding this near the other
unconditional `config.d` links in `tests/config/install.sh`:

```bash
if check_clickhouse_version 26.6; then
    ln -sf $SRC_PATH/config.d/cas_dynamic_disks.xml $DEST_SERVER_PATH/config.d/
fi
```

The version check is required because the same installer can configure an earlier ClickHouse binary,
which rejects unknown server settings. Do not edit the 31 individual SQL tests that call `disk(...)`.

- [ ] **Step 9: Rebuild the unit-test and server binaries**

```bash
ninja -C build unit_tests_dbms > build/build_dynamic_cas_disk_gate_green.log 2>&1
ninja -C build clickhouse > build/build_dynamic_cas_disk_gate_green_server.log 2>&1
```

Dispatch one subagent per log and require both builds to succeed.

- [ ] **Step 10: Run the complete `DiskObjectStorageTest` suite**

```bash
build/src/unit_tests_dbms \
    --gtest_filter='DiskObjectStorageTest.*' \
    > build/test_dynamic_cas_disk_gate_green_unit.log 2>&1
```

Dispatch a subagent to summarize the log. Require the entire suite to pass. This covers both new
scope tests and the existing dynamic `metadata_type=local` path exercised by `CreateDisk` and the
other fixture tests.

- [ ] **Step 11: Run the black-box negative test green**

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run integration --test test_cas_dynamic_disk_gate \
    > build/test_dynamic_cas_disk_gate_green_integration.log 2>&1
```

Dispatch a subagent to inspect the pytest summary in the log. Require the test to pass with
`BAD_ARGUMENTS`, the setting name, no table, and no pool or scratch directory.

- [ ] **Step 12: Run a representative general-lane positive stateless test**

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run functional --test 04278 \
    > build/test_dynamic_cas_disk_gate_opt_in.log 2>&1
```

Dispatch a subagent to summarize the log. Require `04278` to pass; this verifies that the shared test
configuration's explicit opt-in retains the current inline `CAS` disk behavior outside a specialized
CAS lane.

- [ ] **Step 13: Commit the implementation and tests**

```bash
test "$(git rev-parse --abbrev-ref HEAD)" = cas-gc-rebuild
git add src/Core/ServerSettings.cpp \
    src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp \
    src/Disks/tests/gtest_disk_object_storage.cpp \
    tests/integration/test_cas_dynamic_disk_gate/__init__.py \
    tests/integration/test_cas_dynamic_disk_gate/test.py \
    tests/config/config.d/cas_dynamic_disks.xml \
    tests/config/install.sh
git diff --cached --check
git commit -m 'security: gate dynamic `CAS` disks behind a server setting'
test "$(git rev-parse --abbrev-ref HEAD)" = cas-gc-rebuild
git log --oneline -1
```

---

## Task 2: Document the operational boundary {#task-2-document-the-operational-boundary}

**Files:**

- Modify: `docs/en/antalya/cas/configuration.md`

- [ ] **Step 1: Add the setting to the existing server-level table**

Add this row after `cas_blob_upload_pool_size`:

```markdown
| `cas_allow_unsafe_dynamic_disks` | `false` | Allows `disk(..., metadata_type=cas)` in SQL. Keep it disabled on multi-user servers: a dynamic `CAS` disk becomes a process-wide pool member, may use server credentials, and starts background work. Configure `CAS` disks in `storage_configuration` instead |
```

Do not add a new heading. `ServerSettings.cpp` remains the source for the generated global setting
reference; do not edit `docs/en/operations/server-configuration-parameters/settings.md` manually.

- [ ] **Step 2: Review the complete change**

```bash
git diff --check
git diff -- src/Core/ServerSettings.cpp \
    src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp \
    src/Disks/tests/gtest_disk_object_storage.cpp \
    tests/integration/test_cas_dynamic_disk_gate/__init__.py \
    tests/integration/test_cas_dynamic_disk_gate/test.py \
    tests/config/config.d/cas_dynamic_disks.xml \
    tests/config/install.sh \
    docs/en/antalya/cas/configuration.md
```

Verify from the diff that:

- only explicit `metadata_type=cas` dynamic disks are gated;
- the check precedes every object-storage and metadata-storage constructor;
- server-configured disks remain unaffected;
- the default is `false` and the shared stateless server config opts in once;
- the exception and documentation name the same setting.

- [ ] **Step 3: Re-run the focused regression coverage**

```bash
build/src/unit_tests_dbms \
    --gtest_filter='DiskObjectStorageTest.*' \
    > build/test_dynamic_cas_disk_gate_final_unit.log 2>&1
python3 -m ci.praktika run integration --test test_cas_dynamic_disk_gate \
    > build/test_dynamic_cas_disk_gate_final_integration.log 2>&1
python3 -m ci.praktika run functional --test 04278 \
    > build/test_dynamic_cas_disk_gate_final_stateless.log 2>&1
```

Run the two Praktika commands sequentially. Dispatch one subagent per log and require the complete
unit suite, the integration test, and the stateless test to pass.

- [ ] **Step 4: Commit the documentation**

```bash
test "$(git rev-parse --abbrev-ref HEAD)" = cas-gc-rebuild
git add docs/en/antalya/cas/configuration.md
git diff --cached --check
git commit -m 'docs: explain the dynamic `CAS` disk safety gate'
test "$(git rev-parse --abbrev-ref HEAD)" = cas-gc-rebuild
git log --oneline -1
```
