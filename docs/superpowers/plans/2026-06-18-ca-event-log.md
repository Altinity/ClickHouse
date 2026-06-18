# `system.content_addressed_log` (B170) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Emit one structured row per content-addressed (CA) decision/event to a new optional `SystemLog` table `system.content_addressed_log`, off by default, no-op when disabled, enabled for soak/CI.

**Architecture:** Mirror the existing `system.content_addressed_garbage_collection_log` exactly. A decoupled Core POD `CasEvent` + a `std::function` sink held by `Store`; `Store`/`Build`/`Gc` call `store->emitEvent(CasEvent{…})` at hooks (the 4 existing audit lines `CAGCDEL`/`CAREUSE`/`CASTRIP`/`CAROOTREM` are converted to events, plus the rest of the taxonomy). `ContentAddressedMetadataStorage` builds the sink lambda (CasEvent → `ContentAddressedLogElement` → `log->add`) and injects it when the Context has the log; null sink ⇒ no-op.

**Tech Stack:** C++ (ClickHouse), `SystemLog`, gtest, ClickHouse server config. Build dir `build/`. Branch `cas-mergetree-poc`.

**Spec:** `docs/superpowers/specs/2026-06-18-ca-event-log-design.md`

**Conventions (CLAUDE.md):** Allman braces; "exception" not "crash"; no `-j`/`nproc`; redirect builds to a log in `build/`, subagent-summarize; commit trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

---

## File Structure

- Create `…/Core/CasEvent.h` — POD `CasEvent` + `EventType`/`ObjectKindTag` enums + `using CasEventSink = std::function<void(const CasEvent &)>` (Core-only; no ClickHouse types).
- Modify `…/Core/CasStore.h/.cpp` — `Store` holds `CasEventSink event_sink_`; `void emitEvent(const CasEvent &) const;` (no-op if unset); a setter `setEventSink`.
- Modify `…/Core/CasGc.cpp`, `…/Core/CasBuild.cpp` — convert the 4 existing audit `LOG_INFO` lines to `store->emitEvent(...)` and add the remaining hooks (Task 4 table).
- Create `src/Interpreters/ContentAddressedLog.h/.cpp` — `ContentAddressedLogElement` + `ContentAddressedLog : SystemLog<…>` (model on `ContentAddressedGarbageCollectionLog.{h,cpp}`).
- Modify `src/Interpreters/SystemLog.h` (the `M(...)` list), `src/Interpreters/SystemLog.cpp` (include), `src/Interpreters/Context.h/.cpp` (accessor).
- Modify `…/ContentAddressedMetadataStorage.h/.cpp` — `makeCasEventSink()` + inject into the Store.
- Create `utils/ca-soak/configs/ca_event_log.xml` (+ mount it) — enables the log for the soak.
- Tests: `src/Disks/tests/gtest_cas_event_log.cpp`; stateless `tests/queries/0_stateless/…`.

---

## Task 1: `CasEvent` POD + sink type (Core)

**Files:** Create `…/Core/CasEvent.h`; Test: `src/Disks/tests/gtest_cas_event_log.cpp`

- [ ] **Step 1: Write `CasEvent.h`**

```cpp
#pragma once
#include <base/types.h>
#include <base/extended_types.h>
#include <functional>
#include <map>

namespace DB::Cas
{

/// Decoupled, pure-data per-event record (mirrors GcRoundLogRecord's design: NO Interpreters
/// dependency). The metadata storage converts it into a ContentAddressedLogElement and forwards it
/// to the SystemLog. Keeping it a plain POD lets the Core (and its unit tests) stay free of the
/// system-log machinery.
enum class CasEventType
{
    BlobPut, BlobReuseAdopt, BlobReuseResurrect, BlobRetire, BlobDelete, BlobForget,
    TreePut, TreeExpand, TreeRetire, TreeDelete, TreeStrip,
    RefPublish, RefDrop, RootAdd, RootRemove, RootRepoint,
    GcFoldBegin, GcFoldEnd, GcRetireObserve, GcRecheck, GcFence,
    GcLeaseAcquire, GcLeaseSteal, GcLeaseHeartbeat, GcSnapPersist, GcCursorAdvance,
    BuildStart, BuildPublish, BuildAbort, GateRevalidate, WatermarkRenew, Heartbeat,
};

enum class CasEventObjectKind { None, Blob, Tree, Pack, Root, Snap };

struct CasEvent
{
    CasEventType type = CasEventType::Heartbeat;
    String namespace_;          /// roots/<ns> (empty if N/A)
    String ref_name;            /// part name / ref (empty if N/A)
    CasEventObjectKind object_kind = CasEventObjectKind::None;
    String object_hash;         /// lowercase hex (empty if N/A)
    String token;               /// incarnation token (empty if N/A)
    UInt64 round = 0;
    UInt64 gen = 0;
    UInt64 at_version = 0;
    String outcome;             /// e.g. "ok","adopt","deleted","zeroed" (empty if N/A)
    String reason;              /// free-text cause (empty if N/A)
    std::map<String, String> detail;
};

using CasEventSink = std::function<void(const CasEvent &)>;

}
```

- [ ] **Step 2: Write a compile/usage test** in `src/Disks/tests/gtest_cas_event_log.cpp`

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
using namespace DB::Cas;
TEST(CasEvent, ConstructAndCopy)
{
    CasEvent e;
    e.type = CasEventType::BlobDelete;
    e.object_kind = CasEventObjectKind::Blob;
    e.object_hash = "abcd";
    e.token = "tok";
    e.round = 7; e.gen = 3;
    e.detail["freed"] = "10";
    CasEvent c = e;
    EXPECT_EQ(c.type, CasEventType::BlobDelete);
    EXPECT_EQ(c.object_hash, "abcd");
    EXPECT_EQ(c.detail.at("freed"), "10");
}
```

- [ ] **Step 3: Build + run** `cd build && ninja unit_tests_dbms > build_b170_t1.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasEvent.*'` → PASS.
- [ ] **Step 4: Commit** `git add …/Core/CasEvent.h src/Disks/tests/gtest_cas_event_log.cpp && git commit -m "CA B170: CasEvent POD + CasEventSink (Core)"`

---

## Task 2: `Store::emitEvent` + sink plumbing (Core)

**Files:** Modify `…/Core/CasStore.h/.cpp`; Test: `src/Disks/tests/gtest_cas_event_log.cpp`

- [ ] **Step 1: Failing test** (capturing sink) — append to `gtest_cas_event_log.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <vector>
TEST(CasEvent, StoreEmitsToSink)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    std::vector<CasEvent> seen;
    s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });
    CasEvent e; e.type = CasEventType::BlobPut; e.object_hash = "h";
    s->emitEvent(e);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].type, CasEventType::BlobPut);
    // null sink => no-op (no crash)
    s->setEventSink(nullptr);
    s->emitEvent(e);
    EXPECT_EQ(seen.size(), 1u);
}
```

- [ ] **Step 2: Build → FAIL** (`setEventSink`/`emitEvent` undefined).
- [ ] **Step 3: Implement** — in `CasStore.h` (public): `#include ".../Core/CasEvent.h"`, add
```cpp
    void setEventSink(CasEventSink sink) { event_sink_ = std::move(sink); }
    void emitEvent(const CasEvent & e) const { if (event_sink_) event_sink_(e); }
```
and a private member `CasEventSink event_sink_;`. (Header-only; no .cpp change needed unless the class layout requires it.)
- [ ] **Step 4: Build + run** `--gtest_filter='CasEvent.*'` → PASS.
- [ ] **Step 5: Commit** `git commit -m "CA B170: Store::emitEvent + setEventSink (no-op when unset)"`

---

## Task 3: `ContentAddressedLog` SystemLog + Element + Context accessor

**Files:** Create `src/Interpreters/ContentAddressedLog.h/.cpp`; Modify `src/Interpreters/SystemLog.h`, `SystemLog.cpp`, `Context.h`, `Context.cpp`.

- [ ] **Step 1: Create `ContentAddressedLog.h`** — Element with the spec §3 columns:

```cpp
#pragma once
#include <Interpreters/SystemLog.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Storages/ColumnsDescription.h>

namespace DB
{
struct ContentAddressedLogElement
{
    time_t event_time = 0;
    Decimal64 event_time_microseconds = 0;
    String event_type;            /// CasEventType name (LowCardinality in the table)
    String disk_name;
    String namespace_;
    String ref_name;
    String object_kind;           /// none/blob/tree/pack/root/snap
    String object_hash;
    String token;
    UInt64 round = 0;
    UInt64 gen = 0;
    UInt64 at_version = 0;
    String outcome;
    String reason;
    UInt64 thread_id = 0;
    String query_id;
    std::map<String, String> detail;

    static std::string name() { return "ContentAddressedLog"; }
    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & columns) const;
};
class ContentAddressedLog : public SystemLog<ContentAddressedLogElement>
{
    using SystemLog<ContentAddressedLogElement>::SystemLog;
};
}
```

- [ ] **Step 2: Create `ContentAddressedLog.cpp`** — `getColumnsDescription()` (hostname LowCardinality, event_date, event_time, event_time_microseconds, event_type LowCardinality, disk_name LowCardinality, namespace String, ref_name String, object_kind LowCardinality, object_hash String, token String, round/gen/at_version UInt64, outcome LowCardinality, reason String, thread_id UInt64, query_id String, detail `Map(String,String)`) and `appendToBlock()` inserting in the same order — model byte-for-byte on `ContentAddressedGarbageCollectionLog.cpp` (use `getFQDNOrHostName()`, `DateLUT::instance().toDayNum(event_time)`, and a `Map` of `Tuple{k,v}` for `detail`).

- [ ] **Step 3: Register in `SystemLog.h`** — add to the `M(...)` list (next to the GC log):
```cpp
    M(ContentAddressedLog, content_addressed_log, "Per-event content-addressed (CA) MergeTree audit log: one row per blob/tree/ref/GC decision (put, reuse, retire, delete, strip, root add/remove, fence, lease, etc.). Optional, off by default; enabled for soak/CI forensics.") \
```
and `#include <Interpreters/ContentAddressedLog.h>` in `SystemLog.cpp`.

- [ ] **Step 4: Context accessor** — `Context.h`: forward-declare `class ContentAddressedLog;` and `std::shared_ptr<ContentAddressedLog> getContentAddressedLog() const;`. `Context.cpp`: implement returning `shared->system_logs->content_addressed_log` (mirror `getContentAddressedGarbageCollectionLog`).

- [ ] **Step 5: Build** `ninja clickhouse unit_tests_dbms > build_b170_t3.log 2>&1` → compiles. (No new test here; covered by Task 6's stateless test.)
- [ ] **Step 6: Commit** `git commit -m "CA B170: ContentAddressedLog SystemLog + Element + Context accessor"`

---

## Task 4: Emit hooks across the Core (consolidate the 4 audit lines + add the rest)

**Files:** Modify `…/Core/CasGc.cpp`, `…/Core/CasBuild.cpp`.

Replace each existing audit `LOG_INFO` with a `store->emitEvent(...)` (keep behavior identical; the row carries the same fields). Worked example — the `CASTRIP` line becomes:

```cpp
        store->emitEvent(CasEvent{
            .type = CasEventType::TreeStrip,
            .object_kind = CasEventObjectKind::Tree,
            .object_hash = u128ToHex(tree),
            .round = round, .gen = state.snap_generation,
            .outcome = "stripped",
            .detail = {{"freed", std::to_string(freed.size())}}});
```
`CAGCDEL` → `BlobDelete`/`TreeDelete` (by `entry.kind`) with `object_hash`, `token`, `round`, `gen`, `outcome` = the delete outcome string. `CAROOTREM` → `RootRemove` (`ref_name`, `at_version`, `object_hash`=tree, `round`, `gen`, `outcome` = `cands.empty() ? "ok" : "zeroed"`). `CAROOTREPOINT` → `RootRepoint`. `CAREUSE adopt`/`resurrect` (in `CasBuild.cpp`) → `BlobReuseAdopt`/`BlobReuseResurrect` (`object_hash`, `token`, `round`). (Gc reaches the Store via its `store` member; Build via its owning Store.)

Then add the remaining hooks (one `emitEvent` each), at these sites:

| event | site | key fields |
|---|---|---|
| `BlobPut` | `CasBuild.cpp` putBlob (after successful PUT) | object_hash, token |
| `TreePut` | `CasBuild.cpp` putTree | object_hash |
| `TreeExpand` | `CasGc.cpp` foldShardRecords (after `markExpanded`) | object_hash=tree, round, gen, detail{out_edges} |
| `RefPublish` | `CasBuild.cpp` publish | namespace_, ref_name, object_hash=tree, at_version |
| `RefDrop` | `CasStore.cpp` dropRef/republishRef | namespace_, ref_name |
| `RootAdd` | `CasGc.cpp` foldShardRecords Add branch | ref_name, object_hash=tree, at_version, round, gen |
| `BlobRetire`/`TreeRetire` | `CasGc.cpp` retire (per RetiredEntry) | object_hash, token, round |
| `GcRecheck` | `CasGc.cpp` recheck (per outcome) | object_hash, token, round, outcome |
| `GcFence` | `CasGc.cpp` fence | round, detail{fence_seq} |
| `GcLeaseAcquire/Steal/Heartbeat` | `CasGc.cpp` acquireOrRenewLease / pulseHeartbeat | round, detail{owner,seq} |
| `GcSnapPersist`/`GcCursorAdvance` | `CasGc.cpp` cascade/fold persist | gen, detail{cursor} |
| `GcFoldBegin/End` | `CasGc.cpp` fold entry/exit | round, gen |
| `BlobForget` | `CasGc.cpp` forget sites | object_hash, round, gen |
| `BuildStart/Publish/Abort`,`GateRevalidate`,`WatermarkRenew`,`Heartbeat` | `CasBuild.cpp`/`CasHeartbeat.cpp`/`CasWatermark.cpp` | as available |

- [ ] **Step 1:** Convert the 4 existing audit lines to `emitEvent` (delete the `LOG_INFO` lines).
- [ ] **Step 2:** Add the remaining hooks per the table (each a single `store->emitEvent(CasEvent{...})`).
- [ ] **Step 3: Failing-then-passing test** — extend `gtest_cas_event_log.cpp`: open a Store with a capturing sink, `publishPart` + `dropRef` + a `Gc` round, assert events of types `RefPublish`, `RootAdd`/`TreeExpand`, `RefDrop`, and at least one `Gc*` appear.
- [ ] **Step 4: Build + run** `--gtest_filter='CasEvent.*:CasGc*.*'` → PASS (and existing `Cas*` GC tests still green).
- [ ] **Step 5: Commit** `git commit -m "CA B170: emit CasEvents at Core hooks; consolidate CAGCDEL/CAREUSE/CASTRIP/CAROOTREM"`

---

## Task 5: Bridge sink + config wiring

**Files:** Modify `…/ContentAddressedMetadataStorage.h/.cpp`; Create `utils/ca-soak/configs/ca_event_log.xml`; Modify `utils/ca-soak/docker-compose.yml`.

- [ ] **Step 1: `makeCasEventSink()`** in `ContentAddressedMetadataStorage.cpp`, modeled on `makeGcRoundLogger()`:
```cpp
Cas::CasEventSink ContentAddressedMetadataStorage::makeCasEventSink() const
{
    if (!context)
        return {};
    auto ctx = context;
    const String disk = disk_name;
    return [ctx, disk](const Cas::CasEvent & ev)
    {
        auto log = ctx->getContentAddressedLog();
        if (!log)
            return;
        ContentAddressedLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        e.event_type = toString(ev.type);          // a free function name-mapper in CasEvent.h
        e.disk_name = disk;
        e.namespace_ = ev.namespace_;
        e.ref_name = ev.ref_name;
        e.object_kind = toString(ev.object_kind);
        e.object_hash = ev.object_hash;
        e.token = ev.token;
        e.round = ev.round; e.gen = ev.gen; e.at_version = ev.at_version;
        e.outcome = ev.outcome; e.reason = ev.reason;
        e.thread_id = getThreadId();
        e.query_id = CurrentThread::getQueryId();
        e.detail = ev.detail;
        log->add(std::move(e));
    };
}
```
Add `String toString(CasEventType)` / `toString(CasEventObjectKind)` in `CasEvent.h` (a `switch`). Inject: where the Store is created in the metadata storage, call `store->setEventSink(makeCasEventSink());`. Declare `makeCasEventSink` in the `.h`.

- [ ] **Step 2: Build** `ninja clickhouse > build_b170_t5.log 2>&1` → compiles.
- [ ] **Step 3: Soak config** — `utils/ca-soak/configs/ca_event_log.xml`:
```xml
<clickhouse>
    <content_addressed_log>
        <database>system</database>
        <table>content_addressed_log</table>
        <flush_interval_milliseconds>2000</flush_interval_milliseconds>
        <ttl>event_date + INTERVAL 3 DAY DELETE</ttl>
    </content_addressed_log>
</clickhouse>
```
Mount it in `docker-compose.yml` for ch1 AND ch2 (add a `volumes:` line `- ./configs/ca_event_log.xml:/etc/clickhouse-server/config.d/ca_event_log.xml:ro`, both nodes).
- [ ] **Step 4: Commit** `git commit -m "CA B170: bridge CasEvent->SystemLog; soak config.d enables the log"`

---

## Task 6: Stateless test (default-off + on-when-configured)

**Files:** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_event_log.sql` (use `./tests/queries/0_stateless/add-test`).

- [ ] **Step 1:** Use the inline-CA-disk pattern from an existing `0*_content_addressed_*` test. With the log NOT configured by default, assert `system.content_addressed_log` is empty/absent after a CA INSERT (default off). (If the suite has the log enabled globally, instead assert it's queryable.)
- [ ] **Step 2:** (Enabled path is covered by the soak config; a stateless variant that enables it via the test's own config is optional.) Assert after INSERT+OPTIMIZE the table (when enabled) has `event_type IN ('blob_put','ref_publish')` rows.
- [ ] **Step 3:** Run via `python3 -m ci.praktika run` per the repo test harness; confirm green.
- [ ] **Step 4: Commit.**

---

## After the plan
Rebuild `clickhouse`; restart the soak (12h, chaos, `WORKERS=2`, the `ca_event_log.xml` config.d enabling the log, disk watchdog, hourly status reports). On a `dangling` checkpoint, pin the B140-dangle trigger by `SELECT … FROM system.content_addressed_log WHERE ref_name LIKE '%<part>%' ORDER BY event_time` (root_remove/tree_strip/blob_delete chain) — no log-greps.
