# CAS File-Cache Disk Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a file-cache disk (`<type>cache</type>`) work on top of a content-addressed (CA) disk (currently fails at startup with `NOT_IMPLEMENTED`), and demonstrate the cache effect on repeated reads via metrics.

**Architecture:** In `DiskObjectStorage::wrapWithCache`, when the underlying disk is content-addressed, reuse the CA metadata storage directly as the cache disk's metadata storage (wrap ONLY the object storage with `CachedObjectStorage`). CA startup/shutdown are idempotent, so base disk + cache disk share one mount safely. Immutable content-hash blobs then cache through the object-storage wrapper; the control plane keeps using the CA metadata storage's own raw object-storage pointer and bypasses the cache. Full rationale: `docs/superpowers/specs/2026-07-08-cas-file-cache-disk-support-design.md`.

**Tech Stack:** ClickHouse C++ (Disks subsystem), Python integration tests (pytest + minio).

## Global Constraints

- Branch: `cas-gc-rebuild`. NEVER switch branch, NEVER commit to master, NEVER rebase/amend — add new commits.
- Commit trailers (every commit):
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`
- `git add` ONLY the specific files you changed — never `git add -A`/`.`.
- C++: Allman braces (opening brace on its own line). No `LOGICAL_ERROR` for runtime-refusal paths (not relevant here — this is startup wiring).
- Builds: use `build/` (RelWithDebInfo), run `ninja` in the FOREGROUND (blocking), NO `-j`/`nproc`, redirect output to `build/build_<task>.log`, then have a subagent summarize the log. Never background the build and return early.
- Integration tests: run from repo root. The prebuilt binary is at `build/programs/clickhouse` (symlink it where the harness expects, per `reference_praktika_local_runs`). Redirect test output to `build/test_<name>.log`; a subagent summarizes.

---

### Task 1: Failing reproduction integration test (RED)

**Files:**
- Create: `tests/integration/test_cas_file_cache/__init__.py` (empty)
- Create: `tests/integration/test_cas_file_cache/configs/storage_conf.xml`
- Create: `tests/integration/test_cas_file_cache/test.py`

**Interfaces:**
- Produces: storage policy name `cas_cache` (CA disk `disk_ca_s3` wrapped by cache disk `disk_ca_s3_cache`), used by later tasks.

**Config** `configs/storage_conf.xml` (CA-over-minio + cache wrapper, modeled on `test_content_addressed_s3` + the cache stanza from `test_backup_restore_s3/configs/disk_s3.xml`):

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <disk_ca_s3>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>content_addressed</metadata_type>
                <server_root_id>itest-cas-file-cache</server_root_id>
                <endpoint>http://minio1:9001/root/cas_cache_data/</endpoint>
                <access_key_id>minio</access_key_id>
                <secret_access_key>ClickHouse_Minio_P@ssw0rd</secret_access_key>
            </disk_ca_s3>
            <disk_ca_s3_cache>
                <type>cache</type>
                <disk>disk_ca_s3</disk>
                <path>/tmp/cas_file_cache/</path>
                <max_size>1000000000</max_size>
            </disk_ca_s3_cache>
        </disks>
        <policies>
            <cas_cache>
                <volumes>
                    <main>
                        <disk>disk_ca_s3_cache</disk>
                    </main>
                </volumes>
            </cas_cache>
        </policies>
    </storage_configuration>
</clickhouse>
```

**Test** `test.py`:

```python
import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_cache"
NUM_ROWS = 100000


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_minio=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def test_cache_over_ca_startup_and_roundtrip():
    # Before the fix the server fails to register the cache-over-CA disk (NOT_IMPLEMENTED at
    # checkAccess), so this whole module fails at cluster.start(). After the fix, startup + a
    # write/read round-trip succeed.
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_cache_test SYNC")
    node.query(
        """
        CREATE TABLE cas_cache_test (id Int64, data String)
        ENGINE = MergeTree() ORDER BY id
        SETTINGS storage_policy = '{}'
        """.format(STORAGE_POLICY)
    )
    node.query(
        "INSERT INTO cas_cache_test SELECT number, toString(number) FROM numbers({})".format(NUM_ROWS)
    )
    expected_sum = (NUM_ROWS - 1) * NUM_ROWS // 2
    assert int(node.query("SELECT count() FROM cas_cache_test")) == NUM_ROWS
    assert int(node.query("SELECT sum(id) FROM cas_cache_test")) == expected_sum

    node.query("DROP TABLE cas_cache_test SYNC")
```

- [ ] **Step 1: Create the three files** exactly as above.

- [ ] **Step 2: Ensure the prebuilt binary is wired for the harness.** Confirm `build/programs/clickhouse` exists; symlink it to `ci/tmp/clickhouse` if the local praktika/integration harness expects it there (see memory `reference_praktika_local_runs`). Do not rebuild — Task 1 must run against the CURRENT (buggy) binary.

- [ ] **Step 3: Run the test to verify it FAILS.**

Run (redirect to log): `cd tests/integration && ./runner --binary $(pwd)/../../build/programs/clickhouse 'test_cas_file_cache/test.py -x -vvv' > ../../build/test_cas_file_cache_red.log 2>&1` (adjust runner invocation to the local convention if different; the goal is to run this one module).
Expected: FAIL — the module errors at `cluster.start()` or the first insert with a `NOT_IMPLEMENTED` / "not implemented for a content-addressed disk" message. Have a subagent summarize `build/test_cas_file_cache_red.log` and confirm the failure signature is the CA cache-wrap NOT_IMPLEMENTED (not an unrelated infra error).

- [ ] **Step 4: Commit the failing test.**

```bash
git add tests/integration/test_cas_file_cache/__init__.py tests/integration/test_cas_file_cache/configs/storage_conf.xml tests/integration/test_cas_file_cache/test.py
git commit -m "test(cas): failing integration test for file-cache disk over a CA disk

<trailers>"
```

---

### Task 2: Fix `wrapWithCache` for CA disks (GREEN)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp` (`wrapWithCache`, ~L15-32)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.h` (add `isContentAddressed` override decl)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.cpp` (add `isContentAddressed` forward)

**Interfaces:**
- Consumes: the `cas_cache` policy / integration test from Task 1.
- Produces: a working cache-over-CA disk.

- [ ] **Step 1: Edit `wrapWithCache`.** Replace the unconditional `MetadataStorageFromCacheObjectStorage` construction with a CA-aware choice:

```cpp
DiskObjectStoragePtr DiskObjectStorage::wrapWithCache(FileCachePtr cache, const FileCacheSettings & cache_settings, const String & layer_name) const
{
    auto registry = object_storages->getRegistry();
    auto local_location = cluster->getLocalLocation();
    registry[local_location] = std::make_shared<CachedObjectStorage>(registry[local_location], cache, cache_settings, layer_name);

    /// A content-addressed disk cannot be fronted by the generic MetadataStorageFromCacheObjectStorage
    /// passthrough: that wrapper hides isContentAddressed and the concrete CA metadata/transaction
    /// types the CA read/write paths dynamic_cast to, so a cache-wrapped CA disk would take the generic
    /// write path and throw NOT_IMPLEMENTED at startup. Reuse the CA metadata storage directly; only the
    /// object storage is cached. Safe because ContentAddressedMetadataStorage::startup()/shutdown() are
    /// idempotent, so the base disk and this cache disk share one mount/lease with no conflict.
    /// Immutable content-hash blobs then cache through the CachedObjectStorage above; the control plane
    /// keeps using the CA metadata storage's own raw object-storage pointer and bypasses the cache.
    MetadataStoragePtr cache_metadata_storage = metadata_storage->isContentAddressed()
        ? metadata_storage
        : std::make_shared<MetadataStorageFromCacheObjectStorage>(metadata_storage);

    auto cache_disk = std::make_shared<DiskObjectStorage>(
        layer_name,
        std::make_shared<ClusterConfiguration>(layer_name, cluster->getConfiguration()),
        cache_metadata_storage,
        std::make_shared<ObjectStorageRouter>(std::move(registry)),
        std::dynamic_pointer_cast<const DiskObjectStorage>(shared_from_this()),
        Context::getGlobalContextInstance()->getConfigRef(),
        "storage_configuration.disks." + layer_name,
        use_fake_transaction);

    return cache_disk;
}
```

- [ ] **Step 2: Add the defensive `isContentAddressed` forward to the wrapper.** In the header (`MetadataStorageFromCacheObjectStorage.h`), add next to the other bool queries: `bool isContentAddressed() const override;`. In the `.cpp`, add:

```cpp
bool MetadataStorageFromCacheObjectStorage::isContentAddressed() const
{
    return underlying->isContentAddressed();
}
```

- [ ] **Step 3: Build clickhouse (FOREGROUND, blocking).**

Run: `ninja -C build clickhouse > build/build_task2.log 2>&1` (no `-j`/`nproc`; wait for completion). Then dispatch a subagent to summarize `build/build_task2.log` — report only success/failure + any errors.

- [ ] **Step 4: Re-run the Task 1 integration test → GREEN.**

Run: same runner command as Task 1 Step 3, redirecting to `build/test_cas_file_cache_green.log`. Have a subagent summarize. Expected: `test_cache_over_ca_startup_and_roundtrip` PASSES.

- [ ] **Step 5: Commit the fix.**

```bash
git add src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.h src/Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.cpp
git commit -m "fix(cas): support file-cache disk over a content-addressed disk

<trailers>"
```

---

### Task 3: Demonstrate cache effect on repeated reads (metrics)

**Files:**
- Modify: `tests/integration/test_cas_file_cache/test.py` (add a second test function)

**Interfaces:**
- Consumes: `cas_cache` policy, the working fix.

- [ ] **Step 1: Add the metrics test.** Append to `test.py`:

```python
def _profile_events(node, query_id, event):
    node.query("SYSTEM FLUSH LOGS")
    v = node.query(
        "SELECT sum(ProfileEvents['{}']) FROM system.query_log "
        "WHERE query_id = '{}' AND type = 'QueryFinish'".format(event, query_id)
    ).strip()
    return int(v) if v else 0


def test_cache_hits_on_repeated_reads():
    # The whole point of the feature: a second full scan of the same data is served from the local
    # file cache instead of re-fetching immutable content blobs from object storage.
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_cache_metrics SYNC")
    node.query(
        """
        CREATE TABLE cas_cache_metrics (id Int64, data String)
        ENGINE = MergeTree() ORDER BY id
        SETTINGS storage_policy = '{}'
        """.format(STORAGE_POLICY)
    )
    node.query(
        "INSERT INTO cas_cache_metrics SELECT number, toString(number % 1000) FROM numbers(1000000)"
    )
    node.query("OPTIMIZE TABLE cas_cache_metrics FINAL")

    # Start from a cold cache.
    node.query("SYSTEM DROP FILESYSTEM CACHE")

    q1 = "cas_cache_cold_scan"
    node.query(
        "SELECT sum(cityHash64(id, data)) FROM cas_cache_metrics",
        query_id=q1,
        settings={"enable_filesystem_cache": 1},
    )
    q2 = "cas_cache_warm_scan"
    node.query(
        "SELECT sum(cityHash64(id, data)) FROM cas_cache_metrics",
        query_id=q2,
        settings={"enable_filesystem_cache": 1},
    )

    cold_source = _profile_events(node, q1, "CachedReadBufferReadFromSourceBytes")
    warm_source = _profile_events(node, q2, "CachedReadBufferReadFromSourceBytes")
    warm_cache = _profile_events(node, q2, "CachedReadBufferReadFromCacheBytes")

    # Cold scan reads real bytes from the source (object storage); warm scan reads (near) nothing
    # from source and serves the data from cache.
    assert cold_source > 0, "cold scan should read from source"
    assert warm_source * 10 < cold_source, (
        "warm scan should read far fewer source bytes (cold={}, warm={})".format(cold_source, warm_source)
    )
    assert warm_cache > 0, "warm scan should read from the filesystem cache"

    # The cache holds populated segments for this cache disk.
    assert int(node.query("SELECT count() FROM system.filesystem_cache")) > 0

    node.query("DROP TABLE cas_cache_metrics SYNC")
```

- [ ] **Step 2: Run the test → GREEN.** Same runner command scoped to `test_cas_file_cache/test.py::test_cache_hits_on_repeated_reads`, redirect to `build/test_cas_file_cache_metrics.log`, subagent summarizes. If the `ProfileEvents` names differ in this build, the implementer must grep `src/Common/ProfileEvents.cpp` for the actual cached-read-buffer event names and adjust (do NOT invent names). Expected: PASS with a clear cold≫warm source-bytes gap.

- [ ] **Step 3: Commit.**

```bash
git add tests/integration/test_cas_file_cache/test.py
git commit -m "test(cas): assert file-cache hits on repeated reads over a CA disk

<trailers>"
```

---

### Task 4: Docs, memory, backlog, and config-comment updates

**Files:**
- Modify: `tmp/test_stand_ca_storage.xml` (the "NOT WIRED YET" comment block, ~L66-80)
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` (ROADMAP row / entry for cache-over-CA)
- Modify: `docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md` (log Task 1 done)

**Note:** the memory file `project-ca-cache-disk-unwired` is under `~/.claude/...` (out of repo) — the controller updates it directly, not a subagent.

- [ ] **Step 1: Update `tmp/test_stand_ca_storage.xml`.** Replace the "FILE CACHE OVER CA: NOT WIRED YET" comment with a note that cache-over-CA is now supported, and uncomment/enable the `<s3_cache>` stanza as a working example (keep it a valid example; do not point the default policy at it unless intended — leave the policy on the bare CA disk with a comment showing how to switch to the cache disk).

- [ ] **Step 2: Update `BACKLOG.md`.** Mark the "File-cache disk over a CA disk" item RESOLVED (2026-07-08), one line pointing at the spec + the integration test.

- [ ] **Step 3: Update the worklog** with Task 1 completion (commits, test result).

- [ ] **Step 4: Commit.**

```bash
git add tmp/test_stand_ca_storage.xml utils/ca-soak/scenarios/BACKLOG.md docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md
git commit -m "docs(cas): mark file-cache disk over CA disk supported

<trailers>"
```

---

## Self-Review notes

- Spec coverage: startup fix (Task 2), reproduction (Task 1), metrics demonstration (Task 3), docs (Task 4) — all covered.
- The ca-soak `storage_conf_s3cache_ch1.xml` scenario flip is deferred to Task 3-of-session (stabilization); noted in the spec, not duplicated here.
- Risk: integration-test runner invocation differs by local setup — implementers adjust the exact `runner`/`praktika` command to the working local convention (memory `reference_praktika_local_runs`), keeping the module scope and log redirection.
