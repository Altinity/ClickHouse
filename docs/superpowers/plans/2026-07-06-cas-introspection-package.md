# CAS introspection package — Implementation Plan (fix-plan Phase 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give CAS its first live introspection: a `system.content_addressed_mounts` table (I2/I3), mount-slot writer audit events in `system.content_addressed_log` (the P1 "who is the foreign writer" instrument), the first CAS gauges in `CurrentMetrics` (I1), and a scoped/partial-capable `fsck` (I4).

**Architecture:** A read-only `Cas::listMounts` Core function (sibling of `computeHeartbeatFloor`, but with ZERO write side effects) feeds a `StorageSystemDisks`-style system table. Audit events ride the existing `CasEventSink` plumbing (`EventEmitter`/`Store::emitEvent` → `makeCasEventSink` → `system.content_addressed_log`), threaded into the mount claim/renew/release paths as an optional sink parameter. Gauges are one-line `CurrentMetrics` additions set at the GC scheduler's round-end hook. `fsck` gains a `namespace_prefix` (scoped dangling-only mode) and a partial-on-deadline mode returning accumulated counts instead of throwing with nothing.

**Tech Stack:** ClickHouse C++ (Allman braces), gtest (`src/Disks/tests/gtest_cas_*`, helpers in `cas_test_helpers.h`), `build/` ninja builds.

## Global Constraints

- Work on the current branch (`cas-gc-rebuild`); NEVER commit to master; new commits only, no rebase/amend.
- Allman braces everywhere (style check enforces).
- CAS is pre-release: NO compat scaffolding, no schema-evolution shims.
- Introspection must be READ-ONLY and fail-open per row: a corrupt/racing mount body yields a `state='corrupt'` row (or a skipped row), NEVER an exception into the query; contrast `computeHeartbeatFloor`, which deliberately WRITES fence-outs — the new `listMounts` must not.
- Builds: from `build/`, ALWAYS redirect to a unique log (`ninja clickhouse unit_tests_dbms > build_phase2_task<N>.log 2>&1`), NEVER `-j`; use a subagent to summarize the log.
- Unit gate for every task: `build/src/unit_tests_dbms --gtest_filter='Cas*'` fully green.
- When writing text, wrap literal names in backticks; say "exception", not "crash", for `LOGICAL_ERROR`s.
- Base paths: `CA=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed` (used below for brevity — expand in real edits).

---

### Task 1: `Cas::listMounts` — read-only mount enumeration

**Files:**
- Modify: `$CA/Core/CasServerRoot.h` (after `computeHeartbeatFloor`, ~line 240)
- Modify: `$CA/Core/CasServerRoot.cpp` (after `computeHeartbeatFloor`'s body, ~line 497)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (append)

**Interfaces:**
- Consumes: `MountLease` + `decodeMountLease` (`CasServerRoot.h:85-99` / `.cpp:134`), `Layout::serverRootsPrefix()`, `Backend::list/get`.
- Produces: `struct MountInfo { String srid; MountLease lease; String state; }` and `std::vector<MountInfo> listMounts(Backend &, const Layout &, uint64_t now_ms, uint64_t skew_margin_ms)`; `state` ∈ `live | expired | terminated | fenced | corrupt`. Task 2 consumes both.

- [ ] **Step 1: Write the failing test (append to `gtest_cas_mount.cpp`; reuse its existing includes/namespaces — check the file header first and mirror the existing test style)**

```cpp
TEST(CasListMounts, ClassifiesEveryStateReadOnly)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const uint64_t now_ms = 1'000'000;
    const uint64_t ttl_ms = 10'000;

    /// live: fresh claim for srid "a"
    ASSERT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, /*writer_epoch=*/1, ttl_ms, now_ms),
              ClaimMountOutcome::Claimed);
    /// expired: claim for "b" whose lease ran out long before now_ms
    ASSERT_EQ(claimMount(*backend, layout, "b", DB::UInt128{2}, 1, ttl_ms, now_ms - 100'000),
              ClaimMountOutcome::Claimed);
    /// corrupt: garbage bytes in "c"'s mount slot
    backend->putIfAbsent(layout.mountKey("c"), "garbage-not-a-proto", {});

    auto mounts = listMounts(*backend, layout, now_ms, /*skew_margin_ms=*/ttl_ms / 2);
    ASSERT_EQ(mounts.size(), 3u);
    std::map<DB::String, DB::String> by_srid;
    for (const auto & m : mounts)
        by_srid[m.srid] = m.state;
    EXPECT_EQ(by_srid["a"], "live");
    EXPECT_EQ(by_srid["b"], "expired");
    EXPECT_EQ(by_srid["c"], "corrupt");

    /// READ-ONLY guarantee: "b" is expired but must NOT be fenced by listMounts
    /// (computeHeartbeatFloor would stamp gc_fenced=true; the introspection view must not).
    auto again = listMounts(*backend, layout, now_ms, ttl_ms / 2);
    for (const auto & m : again)
        if (m.srid == "b")
        {
            EXPECT_FALSE(m.lease.gc_fenced);
            EXPECT_EQ(m.state, "expired");
        }
}
```

NOTE: `claimMount`'s exact signature/outcome enum — read it at `$CA/Core/CasServerRoot.h` (the mapper places the implementation at `.cpp:300`; `gtest_cas_heartbeat.cpp:24-28` has a `seedOwnClaim` helper calling it) and adjust the two `claimMount(...)` calls to the real signature verbatim. `mountKey(srid)` exists on `Layout` (used throughout `CasServerRoot.cpp`). Do NOT change the assertions.

- [ ] **Step 2: Build + run to verify it fails**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build
ninja unit_tests_dbms > build_phase2_task1_red.log 2>&1 ; tail -3 build_phase2_task1_red.log
```
Expected: compile FAILURE — `listMounts`/`MountInfo` undeclared.

- [ ] **Step 3: Implement in `CasServerRoot.h` (declaration, after `computeHeartbeatFloor`)**

```cpp
/// A read-only snapshot of one server's mount slot, for introspection (`system.content_addressed_mounts`).
/// state: `live` (lease within TTL+skew), `expired` (lease ran out; the next GC round's heartbeat floor
/// will fence it), `terminated` (clean farewell: `min_active == UINT64_MAX`), `fenced` (`gc_fenced`),
/// `corrupt` (body failed to decode — surfaced as a row, never an exception).
struct MountInfo
{
    String srid;
    MountLease lease;
    String state;
};

/// Enumerate every mount slot under `gc/server-roots/`, decoded and classified — the read-only sibling
/// of `computeHeartbeatFloor`: ZERO writes (no fence-out), per-row fail-open. One LIST + one GET per slot.
std::vector<MountInfo> listMounts(Backend & backend, const Layout & layout, uint64_t now_ms, uint64_t skew_margin_ms);
```

- [ ] **Step 4: Implement in `CasServerRoot.cpp`**

```cpp
std::vector<MountInfo> listMounts(Backend & backend, const Layout & layout, uint64_t now_ms, uint64_t skew_margin_ms)
{
    std::vector<MountInfo> out;
    String cursor;
    while (true)
    {
        const ListPage page = backend.list(layout.serverRootsPrefix(), cursor, 1000);
        for (const auto & k : page.keys)
        {
            static constexpr std::string_view suffix = "/mount";
            if (!k.key.ends_with(suffix))
                continue;
            const auto got = backend.get(k.key);
            if (!got)
                continue;   /// raced a delete — read-only view, skip the row
            MountInfo info;
            const size_t end = k.key.size() - suffix.size();
            const size_t start = k.key.rfind('/', end - 1);
            info.srid = k.key.substr(start + 1, end - start - 1);
            try
            {
                info.lease = decodeMountLease(got->bytes);
            }
            catch (...)
            {
                info.state = "corrupt";
                out.push_back(std::move(info));
                continue;
            }
            if (info.lease.gc_fenced)
                info.state = "fenced";
            else if (info.lease.min_active == std::numeric_limits<uint64_t>::max())
                info.state = "terminated";
            else if (now_ms <= info.lease.expires_at_ms + skew_margin_ms)
                info.state = "live";
            else
                info.state = "expired";
            out.push_back(std::move(info));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
    return out;
}
```

- [ ] **Step 5: Build + full Cas gtest sweep green**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build
ninja unit_tests_dbms > build_phase2_task1.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -3
```
Expected: `PASSED` (all Cas tests, including the new `CasListMounts.ClassifiesEveryStateReadOnly`).

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_mount.cpp
git commit -m "CAS: \`listMounts\` — read-only mount-slot enumeration for introspection

Sibling of \`computeHeartbeatFloor\` with ZERO write side effects (no fence-out);
per-row fail-open (\`corrupt\` state instead of an exception)."
```

---

### Task 2: `system.content_addressed_mounts`

The S13 wedge (mount-lease self-adoption fail-closed) was diagnosable only from raw `err.log` lines — mount/lease state had NO live view. One row per mount slot per CA disk.

**Files:**
- Create: `src/Storages/System/StorageSystemContentAddressedMounts.h`
- Create: `src/Storages/System/StorageSystemContentAddressedMounts.cpp`
- Modify: `src/Storages/System/attachSystemTables.cpp` (include block ~line 77; attach block ~line 233)

**Interfaces:**
- Consumes: `Cas::listMounts` (Task 1); `ContentAddressedMetadataStorage::store()` / `isContentAddressed()` (`$CA/ContentAddressedMetadataStorage.h:116/:84`); `Store::layout()/backend()/poolConfig()` (`CasStore.h:362-363`); the `content_addressed_storage_of` disk-iteration idiom from `InterpreterSystemQuery.cpp:2177-2193`.
- Produces: table `system.content_addressed_mounts` with columns `disk String, srid String, server_uuid String, hostname String, pid UInt64, writer_epoch UInt64, seq UInt64, started_at_ms UInt64, expires_at_ms UInt64, min_active UInt64, observed_gc_round UInt64, gc_fenced UInt8, state String`.

- [ ] **Step 1: Header (`StorageSystemContentAddressedMounts.h`)** — mirror `StorageSystemDisks.h`'s shape exactly (read it first; it is ~30 lines):

```cpp
#pragma once

#include <Storages/IStorage.h>

namespace DB
{

class Context;

/// system.content_addressed_mounts: one row per CAS mount slot (`gc/server-roots/<srid>/mount`)
/// visible from every content-addressed disk — live lease state for operators (who holds which
/// slot, renewal health, fenced/terminated). Read-only, one LIST + one GET per slot per disk.
class StorageSystemContentAddressedMounts final : public IStorage
{
public:
    explicit StorageSystemContentAddressedMounts(const StorageID & table_id_);

    std::string getName() const override { return "SystemContentAddressedMounts"; }

    Pipe read(
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    bool isSystemStorage() const override { return true; }
};

}
```

(If `StorageSystemDisks.h` uses extra base helpers — e.g. a protected `const StorageID`, `IStorage(table_id_)` ctor call — copy its exact skeleton.)

- [ ] **Step 2: Implementation (`StorageSystemContentAddressedMounts.cpp`)**

```cpp
#include <Storages/System/StorageSystemContentAddressedMounts.h>

#include <Columns/ColumnString.h>
#include <Columns/ColumnsNumber.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Interpreters/Context.h>
#include <Processors/Sources/SourceFromSingleChunk.h>
#include <QueryPipeline/Pipe.h>
#include <base/getFQDNOrHostName.h>
#include <base/hex.h>

#include <chrono>

namespace DB
{
namespace ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
}

StorageSystemContentAddressedMounts::StorageSystemContentAddressedMounts(const StorageID & table_id_)
    : IStorage(table_id_)
{
    StorageInMemoryMetadata storage_metadata;
    storage_metadata.setColumns(ColumnsDescription(
    {
        {"disk", std::make_shared<DataTypeString>(), "Name of the content-addressed disk."},
        {"srid", std::make_shared<DataTypeString>(), "Server root id owning the mount slot."},
        {"server_uuid", std::make_shared<DataTypeString>(), "UUID of the server incarnation holding the lease (hex)."},
        {"hostname", std::make_shared<DataTypeString>(), "Hostname recorded in the lease body."},
        {"pid", std::make_shared<DataTypeUInt64>(), "Process id recorded in the lease body."},
        {"writer_epoch", std::make_shared<DataTypeUInt64>(), "Fenced writer epoch of the incarnation."},
        {"seq", std::make_shared<DataTypeUInt64>(), "Lease renewal sequence number."},
        {"started_at_ms", std::make_shared<DataTypeUInt64>(), "Lease start, unix ms."},
        {"expires_at_ms", std::make_shared<DataTypeUInt64>(), "Lease expiry, unix ms."},
        {"min_active", std::make_shared<DataTypeUInt64>(), "Oldest in-flight build sequence (UINT64_MAX = farewell)."},
        {"observed_gc_round", std::make_shared<DataTypeUInt64>(), "Newest GC round this server has acked."},
        {"gc_fenced", std::make_shared<DataTypeUInt8>(), "1 if GC fenced this slot out (terminal)."},
        {"state", std::make_shared<DataTypeString>(), "live | expired | terminated | fenced | corrupt."},
    }));
    setInMemoryMetadata(storage_metadata);
}

Pipe StorageSystemContentAddressedMounts::read(
    const Names & column_names,
    const StorageSnapshotPtr & storage_snapshot,
    SelectQueryInfo & /*query_info*/,
    ContextPtr context,
    QueryProcessingStage::Enum /*processed_stage*/,
    size_t /*max_block_size*/,
    size_t /*num_streams*/)
{
    storage_snapshot->check(column_names);

    MutableColumnPtr col_disk = ColumnString::create();
    MutableColumnPtr col_srid = ColumnString::create();
    MutableColumnPtr col_uuid = ColumnString::create();
    MutableColumnPtr col_host = ColumnString::create();
    MutableColumnPtr col_pid = ColumnUInt64::create();
    MutableColumnPtr col_epoch = ColumnUInt64::create();
    MutableColumnPtr col_seq = ColumnUInt64::create();
    MutableColumnPtr col_started = ColumnUInt64::create();
    MutableColumnPtr col_expires = ColumnUInt64::create();
    MutableColumnPtr col_min_active = ColumnUInt64::create();
    MutableColumnPtr col_round = ColumnUInt64::create();
    MutableColumnPtr col_fenced = ColumnUInt8::create();
    MutableColumnPtr col_state = ColumnString::create();

    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto & [disk_name, disk] : context->getDisksMap())
    {
        MetadataStoragePtr md;
        try
        {
            md = disk->getMetadataStorage();
        }
        catch (const Exception & e)
        {
            if (e.code() == ErrorCodes::NOT_IMPLEMENTED)
                continue;
            throw;
        }
        if (!md || !md->isContentAddressed())
            continue;
        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(md.get());
        if (!ca)
            continue;

        Cas::StorePtr store;
        try
        {
            store = ca->store();
        }
        catch (...)
        {
            continue;   /// disk not started yet — no rows, not an error
        }

        const uint64_t skew_margin_ms =
            static_cast<uint64_t>(store->poolConfig().mount_lease_ttl_ms.count()) / 2;
        for (const auto & m : Cas::listMounts(store->backend(), store->layout(), now_ms, skew_margin_ms))
        {
            col_disk->insert(disk_name);
            col_srid->insert(m.srid);
            col_uuid->insert(getHexUIntLowercase(m.lease.server_uuid));
            col_host->insert(m.lease.hostname);
            col_pid->insert(m.lease.pid);
            col_epoch->insert(m.lease.writer_epoch);
            col_seq->insert(m.lease.seq);
            col_started->insert(m.lease.started_at_ms);
            col_expires->insert(m.lease.expires_at_ms);
            col_min_active->insert(m.lease.min_active);
            col_round->insert(m.lease.observed_gc_round);
            col_fenced->insert(static_cast<UInt8>(m.lease.gc_fenced));
            col_state->insert(m.state);
        }
    }

    Block block = storage_snapshot->metadata->getSampleBlock().cloneEmpty();
    MutableColumns res_columns;
    res_columns.emplace_back(std::move(col_disk));
    res_columns.emplace_back(std::move(col_srid));
    res_columns.emplace_back(std::move(col_uuid));
    res_columns.emplace_back(std::move(col_host));
    res_columns.emplace_back(std::move(col_pid));
    res_columns.emplace_back(std::move(col_epoch));
    res_columns.emplace_back(std::move(col_seq));
    res_columns.emplace_back(std::move(col_started));
    res_columns.emplace_back(std::move(col_expires));
    res_columns.emplace_back(std::move(col_min_active));
    res_columns.emplace_back(std::move(col_round));
    res_columns.emplace_back(std::move(col_fenced));
    res_columns.emplace_back(std::move(col_state));
    UInt64 num_rows = res_columns.at(0)->size();
    Chunk chunk(std::move(res_columns), num_rows);
    return Pipe(std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(block), std::move(chunk)));
}

}
```

IMPORTANT: before finalizing, open `src/Storages/System/StorageSystemDisks.cpp` and mirror its EXACT end-of-read block-assembly and `SourceFromSingleChunk` construction (the API shape changes between versions — copy from the sibling, keep column order matching the `ColumnsDescription`). Also verify: `getHexUIntLowercase(UInt128)` exists in `base/hex.h` (grep; if the overload differs, format via `fmt::format("{:032x}", ...)`-equivalent used elsewhere for `UInt128` — grep `getHexUIntLowercase(` for a `UInt128` call site and copy it). If a selected-columns projection is expected (some system storages filter by `column_names`), mirror `StorageSystemDisks` exactly.

- [ ] **Step 3: Register in `attachSystemTables.cpp`**

Include (next to `#include <Storages/System/StorageSystemDisks.h>`):

```cpp
#include <Storages/System/StorageSystemContentAddressedMounts.h>
```

Attach (next to the `"disks"` line at ~:233):

```cpp
    attachNoDescription<StorageSystemContentAddressedMounts>(context, system_database, "content_addressed_mounts",
        "One row per content-addressed (CAS) mount slot: live lease state per server root id — who holds "
        "which slot, renewal health, fenced/terminated state.");
```

(Mirror the exact `attach`/`attachNoDescription` helper the `"disks"` row uses.)

- [ ] **Step 4: Build `clickhouse` + gtest sweep**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build
ninja clickhouse unit_tests_dbms > build_phase2_task2.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -2
```
Expected: build OK, tests green. Have a subagent summarize `build_phase2_task2.log` if it fails.

- [ ] **Step 5: Live check on the ca-soak stand (if up; else defer to Task 6)**

The stand must run THIS build (remount per `reference_ca_soak_fresh_restart`: from `utils/ca-soak`, `docker compose down && docker compose up -d`, wait for `curl -s localhost:8123/ping`).

```bash
docker exec ca-soak-ch1-1 clickhouse-client -q \
  "SELECT disk, srid, state, observed_gc_round, gc_fenced FROM system.content_addressed_mounts FORMAT PrettyCompact"
```
Expected: two rows (`ca_soak_ch1`, `ca_soak_ch2`), both `state='live'`, on both nodes.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add src/Storages/System/StorageSystemContentAddressedMounts.h \
        src/Storages/System/StorageSystemContentAddressedMounts.cpp \
        src/Storages/System/attachSystemTables.cpp
git commit -m "CAS: \`system.content_addressed_mounts\` — live mount/lease state view

The S13 mount-lease wedge was diagnosable only from raw err.log lines; this
table shows who holds which slot, renewal health, and fenced/terminated state."
```

---

### Task 3: mount-slot writer audit events (the P1 "foreign writer" instrument)

Every mount-slot WRITE and every observed foreign/conflicting body becomes a `system.content_addressed_log` row. The chronic all-night `touched by a foreign writer` collisions (26×) are undiagnosable without knowing WHO touched the slot — the conflicting body's identity fields are the payload.

**Files:**
- Modify: `$CA/Core/CasEvent.h` (enum + toString decl), `$CA/Core/CasEvent.cpp` (toString mapping)
- Modify: `$CA/Core/CasServerRoot.h` / `.cpp` (optional sink param on `claimMount` + `MountLeaseKeeper`)
- Modify: `$CA/Core/CasStore.cpp:235` and `:447` (wire the store's sink into the keeper)
- Modify: `src/Interpreters/ContentAddressedLog.cpp` (event-type value comment/list, if one enumerates types — grep first)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (append)

**Interfaces:**
- Consumes: `CasEvent`/`CasEventSink` (`CasEvent.h:43-58`), `Store::emitEvent` (`CasStore.h:404-410`).
- Produces: new `CasEventType` values `MountClaim`, `MountRelease`, `MountConflict` (strings `mount_claim`, `mount_release`, `mount_conflict`); `claimMount(..., const CasEventSink & sink = {})`; `MountLeaseKeeper(..., CasEventSink event_sink = {})`.

- [ ] **Step 1: Write the failing test (append to `gtest_cas_heartbeat.cpp`, mirroring its fixture style — direct `Backend`+`Layout`, injected clock lambdas, no `Store`)**

```cpp
TEST(CasMountAudit, ClaimReleaseAndForeignConflictEmitEvents)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    std::vector<CasEvent> seen;
    CasEventSink sink = [&](const CasEvent & e) { seen.push_back(e); };

    const uint64_t now_ms = 1'000'000;
    /// mint for uuid 1 -> one mount_claim
    ASSERT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, 1, 10'000, now_ms, sink),
              ClaimMountOutcome::Claimed);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::MountClaim);
    EXPECT_EQ(seen[0].detail.at("srid"), "a");
    EXPECT_EQ(seen[0].detail.at("branch"), "mint");

    /// a FOREIGN uuid claiming a live slot -> mount_conflict carrying the current holder's identity
    seen.clear();
    (void)claimMount(*backend, layout, "a", DB::UInt128{2}, 1, 10'000, now_ms, sink);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().type, CasEventType::MountConflict);
    EXPECT_EQ(seen.back().detail.at("srid"), "a");
    EXPECT_FALSE(seen.back().detail.at("holder_uuid").empty());
}
```

NOTE: adjust the `claimMount` calls to its real signature (Task 1 note) with `sink` appended as the LAST defaulted parameter; if the foreign-claim branch returns an outcome instead of throwing, keep `(void)` — the assertion is about the emitted event, not the outcome. Do NOT weaken the `detail` assertions.

- [ ] **Step 2: Build → RED** (`ninja unit_tests_dbms > build_phase2_task3_red.log 2>&1` — compile failure on the new enum/param).

- [ ] **Step 3: Add the enum values + strings**

`CasEvent.h` enum (append before `RefResolve` group, keeping related groups together — place after `MountBeat, MountRemount`):

```cpp
    MountClaim, MountRelease, MountConflict,
```

`CasEvent.cpp` `toString(CasEventType)` switch — add (Allman, mirroring neighbors):

```cpp
        case CasEventType::MountClaim: return "mount_claim";
        case CasEventType::MountRelease: return "mount_release";
        case CasEventType::MountConflict: return "mount_conflict";
```

Then `grep -n "mount_beat\|mount_remount" src/Interpreters/ContentAddressedLog.cpp src/Interpreters/ContentAddressedLog.h` — if a comment or values list enumerates event types, extend it with the three new values the same way.

- [ ] **Step 4: Thread the sink through `claimMount`**

In `CasServerRoot.h`, append `const CasEventSink & sink = {}` as the last parameter of `claimMount` (and of `claimMountAwaitingExpiry`, which must forward it). Include `CasEvent.h` from `CasServerRoot.h`.

In `CasServerRoot.cpp` `claimMount` (~:300): each WRITE branch emits one event after its successful write, and the foreign/conflict observation emits before returning/throwing. Use this helper (file-local, above `claimMount`):

```cpp
namespace
{
void emitMountEvent(const CasEventSink & sink, CasEventType type, const String & srid,
                    const String & branch, const MountLease * observed, const String & reason)
{
    if (!sink)
        return;
    CasEvent e;
    e.type = type;
    e.object_kind = CasEventObjectKind::None;
    e.outcome = branch;
    e.reason = reason;
    e.detail["srid"] = srid;
    e.detail["branch"] = branch;
    if (observed)
    {
        e.detail["holder_uuid"] = getHexUIntLowercase(observed->server_uuid);
        e.detail["holder_hostname"] = observed->hostname;
        e.detail["holder_pid"] = std::to_string(observed->pid);
        e.detail["holder_epoch"] = std::to_string(observed->writer_epoch);
        e.detail["holder_seq"] = std::to_string(observed->seq);
        e.detail["holder_expires_at_ms"] = std::to_string(observed->expires_at_ms);
    }
    sink(e);
}
}
```

Emission points (anchor by the branch structure the mapper located):
- absent → `putIfAbsent` success (`:311` area): `emitMountEvent(sink, CasEventType::MountClaim, srid, "mint", nullptr, "fresh mount slot minted")`.
- same uuid + epoch refresh (`:330` area): branch `"refresh"`, pass the observed body.
- same uuid, different epoch, expired/fenced reclaim (`:347` area): branch `"reclaim"`, pass the observed body.
- any branch that REFUSES because the observed body belongs to a FOREIGN uuid (or a same-uuid body it cannot adopt): `emitMountEvent(sink, CasEventType::MountConflict, srid, "<branch-name>", &observed_lease, "<the exact refusal reason used in the thrown/returned message>")` — place it immediately before the throw/return in EVERY refusal branch, including the `touched while adopting our own mount slot` and `touched by a foreign writer` paths (grep those two literals in `CasServerRoot.cpp`; both get a `MountConflict` emission with the CURRENT decoded body — this is the P1 payload).

- [ ] **Step 5: Thread the sink through `MountLeaseKeeper`**

Ctor (`CasServerRoot.h:266`): append `CasEventSink event_sink_ = {}`; store as a member `CasEventSink event_sink;`. Emit:
- in `claim` (`:540`): after the adopt/mint write succeeds → `MountClaim` with branch `"adopt"` / `"mint"`; in its refusal paths → `MountConflict` (same rule as Step 4).
- in `terminate` (`:602`): after the farewell write → `MountRelease`, branch `"farewell"`.
- in `onRenewFailed` (`:~595`): `MountConflict`, branch `"renew_failed"`, with whatever body/mismatch info the failure path already has in scope (pass `nullptr` if none — the srid+timestamp alone is still the timeline signal).

- [ ] **Step 6: Wire from `Store`**

`CasStore.cpp:235` and `:447` construct the keeper — append the sink argument. At `:235` (inside `Store::open`, `store` is the fresh object): `[s = store.get()](const CasEvent & e) { s->emitEvent(e); }`; at `:447` (member context): `[this](const CasEvent & e) { emitEvent(e); }`. If `claimMount` is invoked directly nearby (grep `claimMount(` in `CasStore.cpp`), pass the same lambda there. Lifetime note for the reviewer: the keeper is a member of `Store` (`CasStore.h:607`) destroyed before the `Store`, so the raw-pointer capture cannot dangle; `emitEvent` is a no-op when no sink is installed.

- [ ] **Step 7: Build + green + full sweep** (`ninja clickhouse unit_tests_dbms > build_phase2_task3.log 2>&1`; `--gtest_filter='Cas*'` green, including `CasMountAudit.*` and the existing `gtest_cas_event_log.cpp` tests).

- [ ] **Step 8: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Interpreters/ContentAddressedLog.cpp src/Disks/tests/gtest_cas_heartbeat.cpp
git commit -m "CAS: mount-slot writer audit events (\`mount_claim\`/\`mount_release\`/\`mount_conflict\`)

Every mount-slot write and every observed foreign/conflicting body now lands in
\`system.content_addressed_log\` with the holder's identity — the instrument for
the S13 'touched by a foreign writer' investigation."
```

---

### Task 4: first CAS gauges — `CasGcIsLeader`, `CasGcPendingReclaimEntries`

**Files:**
- Modify: `src/Common/CurrentMetrics.cpp` (the `APPLY_FOR_BUILTIN_METRICS(M)` block)
- Modify: `$CA/CasGcScheduler.cpp` (round-end hook, `:127-141`)

**Interfaces:**
- Consumes: `Cas::RoundReport` fields `acquired_lease, condemned, redeleted` (`Core/CasGc.h:53-83`).
- Produces: two gauges in `system.metrics`.

- [ ] **Step 1: Declare the metrics** — add to the `M(...)` list in `CurrentMetrics.cpp` (alphabetically near other `Cas`-prefixed... none exist; place near `S3Requests`-style disk metrics or at a sensible block with a comment):

```cpp
    M(CasGcIsLeader, "1 while this server holds the content-addressed GC leadership lease (set at each round end; 0 after a round where the lease was not acquired).") \
    M(CasGcPendingReclaimEntries, "Content-addressed GC two-phase deletion backlog observed by this process: cumulative condemned entries minus executed exact-token deletes. Process-local (resets on restart).") \
```

- [ ] **Step 2: Hook the scheduler round end** — in `CasGcScheduler.cpp` right where the `Cas::RoundReport` is captured after `gc.runRegularRound()` (`:127-141`; read the surrounding code and place after the existing report handling, before/next to the log-row emission):

```cpp
    CurrentMetrics::set(CurrentMetrics::CasGcIsLeader, report.acquired_lease ? 1 : 0);
    if (report.acquired_lease)
        CurrentMetrics::add(CurrentMetrics::CasGcPendingReclaimEntries,
                            static_cast<Int64>(report.condemned) - static_cast<Int64>(report.redeleted));
```

Add at the top of the file (mirroring how other files declare used metrics):

```cpp
#include <Common/CurrentMetrics.h>

namespace CurrentMetrics
{
    extern const Metric CasGcIsLeader;
    extern const Metric CasGcPendingReclaimEntries;
}
```

- [ ] **Step 3: Build + gtest sweep + live check**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build
ninja clickhouse unit_tests_dbms > build_phase2_task4.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -2
```
Live (stand remounted on this build, after a few GC rounds):

```bash
docker exec ca-soak-ch1-1 clickhouse-client -q \
  "SELECT metric, value FROM system.metrics WHERE metric LIKE 'CasGc%'"
```
Expected: `CasGcIsLeader` is `1` on exactly one node (and `0` on the other); `CasGcPendingReclaimEntries` ≥ 0 and drops toward 0 after a `DROP TABLE` + a few rounds.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add src/Common/CurrentMetrics.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp
git commit -m "CAS: first gauges — \`CasGcIsLeader\` + \`CasGcPendingReclaimEntries\`

CAS previously had ZERO CurrentMetrics: leader identity and the reclaim backlog
were invisible live (S11/S13 findings, 2026-07-06 re-audit)."
```

---

### Task 5: fsck — namespace scoping + partial results on deadline

The campaign lost 4 S08 verdicts and one S13 verdict to `fsck` timing out and returning NOTHING (`TIMEOUT_EXCEEDED` from `checkDeadline`, `CasFsck.cpp:33-38`).

**Files:**
- Modify: `$CA/Core/CasFsck.h` (report fields + signature), `$CA/Core/CasFsck.cpp` (impl split + scoped mode)
- Modify: `programs/disks/CommandFsck.cpp` (options + partial printing)
- Test: `src/Disks/tests/gtest_cas_fsck.cpp` (append; read its fixture first and reuse its store/publish helpers)

**Interfaces:**
- Produces: `FsckReport` gains `bool partial = false; String partial_reason;`; `runFsck(Store &, bool detail, FsckProgress on_progress = {}, std::optional<...> deadline = {}, bool partial_on_deadline = false, const String & namespace_prefix = {})`. Existing callers compile unchanged (defaulted params).

- [ ] **Step 1: Write the failing tests (append to `gtest_cas_fsck.cpp`; reuse its existing helpers for opening a store and publishing a ref — read the file first, mirror an existing test's setup verbatim)**

```cpp
TEST(CasFsckPartial, DeadlineReturnsAccumulatedCountsInsteadOfThrowing)
{
    /// <setup copied from an existing test: store + at least one published ref>
    const auto past = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    /// partial_on_deadline=false keeps the old contract:
    EXPECT_THROW(DB::Cas::runFsck(*store, /*detail=*/false, {}, past), DB::Exception);
    /// partial_on_deadline=true returns a flagged report:
    const auto report = DB::Cas::runFsck(*store, false, {}, past, /*partial_on_deadline=*/true);
    EXPECT_TRUE(report.partial);
    EXPECT_FALSE(report.partial_reason.empty());
}

TEST(CasFsckScoped, NamespacePrefixChecksOnlyMatchingRefsDanglingOnly)
{
    /// <setup: two namespaces "nsa...", "nsb...", one ref+blob each — copy the publish helper twice>
    const auto scoped = DB::Cas::runFsck(*store, false, {}, {}, false, /*namespace_prefix=*/"nsa");
    EXPECT_EQ(scoped.dangling, 0u);
    EXPECT_GT(scoped.reachable, 0u);
    /// scoped mode does not classify the rest of the pool:
    EXPECT_EQ(scoped.unreachable, 0u);
    EXPECT_EQ(scoped.pending_gc + scoped.awaiting_gc + scoped.unaccounted, 0u);
}
```

- [ ] **Step 2: Build → RED.**

- [ ] **Step 3: `CasFsck.h` — extend `FsckReport` + signature**

Into `FsckReport` (after the counters, before `objects`):

```cpp
    /// Set when the scan hit its deadline in partial mode: counts cover only what was walked
    /// before the deadline — a lower bound, not the pool truth.
    bool partial = false;
    String partial_reason;
```

New signature:

```cpp
FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress = {},
                   std::optional<std::chrono::steady_clock::time_point> deadline = {},
                   bool partial_on_deadline = false, const String & namespace_prefix = {});
```

- [ ] **Step 4: `CasFsck.cpp` — mechanical split + scoped mode**

1. Rename the current `runFsck` body into a file-local `void runFsckImpl(Store & store, bool detail, const FsckProgress & on_progress, const Deadline & deadline, const String & namespace_prefix, FsckReport & report)` — delete the local `FsckReport report;` declaration and the final `return report;` (the caller owns it).
2. New public wrapper:

```cpp
FsckReport runFsck(Store & store, bool detail, FsckProgress on_progress,
                   std::optional<std::chrono::steady_clock::time_point> deadline,
                   bool partial_on_deadline, const String & namespace_prefix)
{
    FsckReport report;
    try
    {
        runFsckImpl(store, detail, on_progress, deadline, namespace_prefix, report);
    }
    catch (const Exception & e)
    {
        if (!partial_on_deadline || e.code() != ErrorCodes::TIMEOUT_EXCEEDED)
            throw;
        report.partial = true;
        report.partial_reason = e.message();
    }
    return report;
}
```

3. Scoping inside `runFsckImpl`: both `store.listNamespaces("")` loops (`CasFsck.cpp:120` and `:341`) become `store.listNamespaces(namespace_prefix)`.
4. Scoped mode skips the GLOBAL physical classification (which is meaningless under a filter — blobs owned by other namespaces would read as unreachable). Wrap the block from `/// Physical listing: blobs + manifest bodies.` (`:184`) through the end of the present-blobs classification loop (`:336`) in `if (namespace_prefix.empty()) { ... }`, and add the scoped branch:

```cpp
    else
    {
        /// Scoped mode: dangling-only for the selected namespaces. Each blob named by a scoped ref
        /// is HEAD-verified (O(scoped refs), no pool-wide LIST); the unreachable/pending pipeline
        /// classification needs the whole pool and is intentionally skipped.
        for (const String & bkey : reachable_blobs)
        {
            checkDeadline(deadline, "head-checking scoped blobs");
            const HeadResult h = backend.head(bkey);
            if (h.exists)
            {
                ++report.reachable;
                report.physical_bytes += h.size;
            }
            else
                ++report.dangling;
            if (detail || !h.exists)
            {
                FsckObject o;
                o.key = bkey;
                o.kind = ObjectKind::Blob;
                o.size = h.exists ? h.size : 0;
                o.cls = h.exists ? FsckClass::Reachable : FsckClass::Dangling;
                if (detail)
                    if (const auto lit = blob_labels.find(bkey); lit != blob_labels.end())
                        o.reachable_from = lit->second;
                report.objects.push_back(std::move(o));
            }
        }
    }
```

The manifest-debris pass (`:341-368`) stays active in both modes (it is already per-namespace via `listNamespaces(namespace_prefix)`).

- [ ] **Step 5: `CommandFsck.cpp` — CLI options**

Add to `options_description` (`:27-28` block):

```cpp
    ("namespace", po::value<String>(), "scope the scan to namespaces with this prefix (dangling-only mode: "
                                       "the pool-wide unreachable classification is skipped)")
    ("partial", "on --timeout, print the counts accumulated so far flagged partial=1 instead of aborting empty-handed")
```

In `executeImpl`: read them (`String namespace_prefix = options.count("namespace") ? options["namespace"].as<String>() : "";`, `bool partial = options.contains("partial");` — mirror the exact option-reading idiom already used for `detail`/`timeout` in this file) and pass both to `Cas::runFsck(*ca->store(), detail, on_progress, deadline_opt, partial, namespace_prefix)`. In the summary print (`:63-68`), append ` partial=1 reason='<report.partial_reason>'` ONLY when `report.partial` (existing single-line format otherwise byte-identical — the soak harness parses it; verify with `grep -n "reachable" /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak/soak/fsck.py` that the parser keys on `k=v` tokens and tolerates extra tokens; if it is strict, extend the parser in the same commit). Keep the `dangling > 0` throw — partial results with dangling still exit nonzero.

- [ ] **Step 6: Build + green + sweep** (`ninja clickhouse unit_tests_dbms > build_phase2_task5.log 2>&1`; `--gtest_filter='Cas*'` green).

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        programs/disks/CommandFsck.cpp src/Disks/tests/gtest_cas_fsck.cpp
git commit -m "CAS: fsck \`--namespace\` scoping + \`--partial\` deadline mode

The 2026-07-05 campaign lost 5 verdicts to fsck timing out at 100k objects and
returning NOTHING; partial mode returns the accumulated lower-bound counts, and
namespace scoping bounds the walk to O(scoped refs)."
```

---

### Task 6: live end-to-end validation on the RustFS stand

**Files:** none (validation; fix-forward small issues).

- [ ] **Step 1: Remount the stand on this build** — from `utils/ca-soak`: `docker compose down && docker compose up -d`; wait `curl -s localhost:8123/ping` = `Ok.` on 8123 and 8124.

- [ ] **Step 2: Mounts table + gauges (both nodes)**

```bash
for p in 8123 8124; do curl -s "localhost:$p/?query=SELECT+hostName(),+srid,+state,+observed_gc_round+FROM+system.content_addressed_mounts+FORMAT+TSV"; done
for p in 8123 8124; do curl -s "localhost:$p/?query=SELECT+hostName(),+metric,+value+FROM+system.metrics+WHERE+metric+LIKE+'CasGc%25'+FORMAT+TSV"; done
```
Expected: 2 mount rows visible from EACH node, both `live`; `CasGcIsLeader=1` on exactly one node.

- [ ] **Step 3: Audit events across a kill-restart cycle (the S13 shape)**

```bash
docker kill -s KILL ca-soak-ch1-1 && sleep 3 && docker start ca-soak-ch1-1
sleep 30
docker exec ca-soak-ch1-1 clickhouse-client -q "SYSTEM FLUSH LOGS" 
docker exec ca-soak-ch1-1 clickhouse-client -q \
  "SELECT event_time, event_type, outcome, detail['srid'] AS srid, detail['branch'] AS branch, detail['holder_uuid'] AS holder \
   FROM system.content_addressed_log WHERE event_type LIKE 'mount%' ORDER BY event_time DESC LIMIT 10 FORMAT PrettyCompact"
```
Expected: `mount_claim` rows (branch `adopt` or `reclaim`) from the restart; any collision shows `mount_conflict` WITH the holder identity — exactly the P1 instrument working.

- [ ] **Step 4: Scoped + partial fsck** — create a small CA table, then:

```bash
docker exec ca-soak-ch1-1 clickhouse-disks --config /etc/clickhouse-server/config.xml \
  --disk ca_ro --query "fsck --namespace <the table's namespace prefix> --detail" 
```
(Adapt the invocation to how the harness calls fsck — see `utils/ca-soak/soak/fsck.py` for the exact working command line and read-only disk name.) Expected: summary with `dangling=0`, no pool-wide unreachable churn; a `--timeout 1 --partial` run on a bigger pool prints `partial=1`.

- [ ] **Step 5: Record + commit** — append a row to `docs/superpowers/cas/ROADMAP.md` under "DESIRABLE before release" (`B15/B99/B169` bullet): note that the mounts table, mount audit events, first gauges, and scoped/partial fsck landed (one line, cite this plan). Commit the doc edit.

---

## Self-review notes

- Spec coverage: I2/I3 → Tasks 1-2; P1 instrument → Task 3; I1 gauges → Task 4; I4 fsck → Task 5; live acceptance → Task 6. GC-log widening deliberately NOT done (design: YAGNI, mounts table carries point-in-time state).
- Read-only invariant: `listMounts` never writes (Task 1 test pins it); the system table only LIST+GETs.
- Signature-uncertainty is confined to two anchors flagged in-place (`claimMount`'s exact parameter list, `StorageSystemDisks`' exact block-assembly idiom) with explicit read-and-mirror instructions — assertions and semantics are fixed and must not be weakened.
- Type consistency: `MountInfo`/`listMounts` (Task 1) are consumed with the same names/fields in Task 2; `CasEventSink` param names match between `claimMount` and `MountLeaseKeeper` (Task 3); `partial/partial_reason/namespace_prefix` names match between `CasFsck.h`, `CasFsck.cpp`, and `CommandFsck.cpp` (Task 5).
