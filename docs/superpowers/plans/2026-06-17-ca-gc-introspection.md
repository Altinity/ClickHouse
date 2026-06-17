# CA GC introspection (`content_addressed_garbage_collection_log` + SYSTEM trigger) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `system.content_addressed_garbage_collection_log` table (a Start + Finish row per CA GC round, like `part_log`) and a `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION [<disk>]` command that runs one GC round synchronously.

**Architecture:** A new `SystemLog` (cloned from `PartLog`) registered in `LIST_OF_ALL_SYSTEM_LOGS`. The CA GC scheduler (`CasGcScheduler`, Disks layer) emits records through an injected `std::function` sink (kept decoupled from `Interpreters`), wrapping each `runRegularRound()` in a `ProfileEventsScope` for the per-round `ProfileEvents` delta. The SYSTEM command adds an `ASTSystemQuery` type + parser + interpreter handler that calls a new synchronous `ContentAddressedMetadataStorage::runGarbageCollectionRoundNow()`.

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/gtest_cas_*`), stateless SQL tests (`tests/queries/0_stateless`), ninja to a log analyzed by a subagent.

**Spec:** `docs/superpowers/specs/2026-06-17-ca-gc-introspection-design.md` · **Branch:** `cas-mergetree-poc` (add commits; do NOT branch off).

---

## Conventions (every task)
- Build: `ninja -C build clickhouse unit_tests_dbms > build/build_cagclog_<task>.log 2>&1` (no `-j`/`nproc`); a subagent summarizes the log tail.
- Allman braces; "exception" not "crash"; commit trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. `tmp/` not `/tmp`. Verify `git branch --show-current` = `cas-mergetree-poc`.
- Names fixed up-front (use verbatim everywhere): log class `ContentAddressedGarbageCollectionLog`, element `ContentAddressedGarbageCollectionLogElement`, member/table `content_addressed_garbage_collection_log`, Context accessor `getContentAddressedGarbageCollectionLog()`, AST type `CONTENT_ADDRESSED_GARBAGE_COLLECTION`, access type `SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION`, scheduler trigger method `runGarbageCollectionRoundNow()`, the Disks-layer POD `DB::ContentAddressed::GcRoundLogRecord`, sink type `GcRoundLogger = std::function<void(const GcRoundLogRecord &)>`.

## File structure
- Create `src/Interpreters/ContentAddressedGarbageCollectionLog.h/.cpp` — the element + log class (clone of `PartLog`).
- Modify `src/Interpreters/SystemLog.h` — register in `LIST_OF_ALL_SYSTEM_LOGS`.
- Modify `src/Interpreters/Context.h/.cpp` — the accessor.
- Modify `programs/server/config.xml` (+ `programs/server/config.d` defaults if present) — the `<content_addressed_garbage_collection_log>` block.
- Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h` — the decoupled POD + `GcRoundLogger` typedef + enums.
- Modify `CasGcScheduler.h/.cpp` — accept the sink + disk name; instrument `loop()` and `runOneRoundNow(trigger)`.
- Modify `ContentAddressedMetadataStorage.h/.cpp` — build the sink, pass disk name, add `runGarbageCollectionRoundNow()`.
- Modify `src/Parsers/ASTSystemQuery.h`, `src/Parsers/ParserSystemQuery.cpp` — the new type (disk-targeted).
- Modify `src/Access/Common/AccessType.h` — the access type.
- Modify `src/Interpreters/InterpreterSystemQuery.cpp` — the handler + access mapping + disk enumeration.
- Tests: `src/Disks/tests/gtest_cas_gc_log.cpp` (new), `tests/queries/0_stateless/<NNNNN>_ca_gc_introspection.sql` (via `add-test`).

---

## Phase 1 — the SystemLog table

### Task 1: `ContentAddressedGarbageCollectionLogElement` + log class
**Files:** Create `src/Interpreters/ContentAddressedGarbageCollectionLog.h`, `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`.

- [ ] **Step 1: Write the header.** `src/Interpreters/ContentAddressedGarbageCollectionLog.h`:
```cpp
#pragma once
#include <Interpreters/SystemLog.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Storages/ColumnsDescription.h>

namespace DB
{

struct ContentAddressedGarbageCollectionLogElement
{
    enum EventType : Int8 { START = 1, FINISH = 2 };
    enum Outcome   : Int8 { LED = 1, BACKED_OFF = 2, ABORTED = 3 };
    enum Trigger   : Int8 { SCHEDULED = 1, MANUAL = 2 };

    time_t event_time = 0;
    Decimal64 event_time_microseconds = 0;

    EventType event_type = START;
    String disk_name;
    String gc_id;
    Trigger trigger = SCHEDULED;

    UInt64 round = 0;
    Outcome outcome = LED;          /// meaningful on FINISH
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 children_cascaded = 0;
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;   /// per-round delta (FINISH)

    static std::string name() { return "ContentAddressedGarbageCollectionLog"; }
    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & columns) const;
};

class ContentAddressedGarbageCollectionLog : public SystemLog<ContentAddressedGarbageCollectionLogElement>
{
    using SystemLog<ContentAddressedGarbageCollectionLogElement>::SystemLog;
};

}
```

- [ ] **Step 2: Write the .cpp.** `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`:
```cpp
#include <Interpreters/ContentAddressedGarbageCollectionLog.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Common/DateLUTImpl.h>

namespace DB
{

ColumnsDescription ContentAddressedGarbageCollectionLogElement::getColumnsDescription()
{
    auto type_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Start", static_cast<Int8>(START)}, {"Finish", static_cast<Int8>(FINISH)}});
    auto outcome_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Led", static_cast<Int8>(LED)}, {"BackedOff", static_cast<Int8>(BACKED_OFF)}, {"Aborted", static_cast<Int8>(ABORTED)}});
    auto trigger_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Scheduled", static_cast<Int8>(SCHEDULED)}, {"Manual", static_cast<Int8>(MANUAL)}});
    auto lc_string = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());

    return ColumnsDescription
    {
        {"hostname", lc_string, "Host name of the server executing the round."},
        {"event_date", std::make_shared<DataTypeDate>(), "Event date."},
        {"event_time", std::make_shared<DataTypeDateTime>(), "Event time."},
        {"event_time_microseconds", std::make_shared<DataTypeDateTime64>(6), "Event time with microseconds."},
        {"event_type", type_enum, "Start or Finish of a GC round."},
        {"disk_name", lc_string, "Content-addressed disk the round ran on."},
        {"gc_id", std::make_shared<DataTypeString>(), "GC scheduler instance id (which mounter)."},
        {"trigger", trigger_enum, "Scheduled (background tick) or Manual (SYSTEM command)."},
        {"round", std::make_shared<DataTypeUInt64>(), "GC round number (0 on Start)."},
        {"outcome", outcome_enum, "Led / BackedOff / Aborted (Finish only)."},
        {"candidates_marked", std::make_shared<DataTypeUInt64>(), "Objects retired (marked) this round."},
        {"objects_deleted", std::make_shared<DataTypeUInt64>(), "Objects physically deleted this round."},
        {"objects_absent", std::make_shared<DataTypeUInt64>(), "Retire candidates found already absent."},
        {"objects_replaced", std::make_shared<DataTypeUInt64>(), "412-saves (a resurrection won the race)."},
        {"objects_spared", std::make_shared<DataTypeUInt64>(), "Candidates spared (in-degree > 0 at recheck)."},
        {"children_cascaded", std::make_shared<DataTypeUInt64>(), "Child edges freed by the cascade."},
        {"duration_ms", std::make_shared<DataTypeUInt64>(), "Round wall-clock duration (Finish)."},
        {"error", std::make_shared<DataTypeString>(), "Exception text when outcome = Aborted."},
        {"ProfileEvents", std::make_shared<DataTypeMap>(lc_string, std::make_shared<DataTypeUInt64>()),
            "Per-round ProfileEvents delta (the Cas* counters and S3 events for this round)."},
    };
}

void ContentAddressedGarbageCollectionLogElement::appendToBlock(MutableColumns & columns) const
{
    size_t i = 0;
    columns[i++]->insert(getFQDNOrHostName());
    columns[i++]->insert(DateLUT::instance().toDayNum(event_time).toUnderType());
    columns[i++]->insert(event_time);
    columns[i++]->insert(event_time_microseconds);
    columns[i++]->insert(static_cast<Int8>(event_type));
    columns[i++]->insert(disk_name);
    columns[i++]->insert(gc_id);
    columns[i++]->insert(static_cast<Int8>(trigger));
    columns[i++]->insert(round);
    columns[i++]->insert(static_cast<Int8>(outcome));
    columns[i++]->insert(candidates_marked);
    columns[i++]->insert(objects_deleted);
    columns[i++]->insert(objects_absent);
    columns[i++]->insert(objects_replaced);
    columns[i++]->insert(objects_spared);
    columns[i++]->insert(children_cascaded);
    columns[i++]->insert(duration_ms);
    columns[i++]->insert(error);
    {
        Map map;
        map.reserve(profile_events.size());
        for (const auto & [k, v] : profile_events)
            map.push_back(Tuple{k, v});
        columns[i++]->insert(map);
    }
}

}
```
(`getFQDNOrHostName`, `Map`, `Tuple`, `DateLUT` are the same helpers `PartLog.cpp` uses — confirm the include for `getFQDNOrHostName` matches `PartLog.cpp`, e.g. `<Common/getFQDNOrHostName.h>`, and add it.)

- [ ] **Step 3: Build** `ninja -C build clickhouse` → expect a link error that nothing registers/uses the class yet (or clean compile of the TU). Subagent summarizes. **No commit yet** (registration in Task 2 makes it usable).

### Task 2: Register the log + Context accessor + config
**Files:** Modify `src/Interpreters/SystemLog.h`, `src/Interpreters/Context.h`, `src/Interpreters/Context.cpp`, `programs/server/config.xml`.

- [ ] **Step 1: Forward-decl include + macro entry.** In `src/Interpreters/SystemLog.h`, add to the `LIST_OF_ALL_SYSTEM_LOGS(M)` macro (the block at ~lines 16-47, alphabetical-ish, next to `M(PartLog, part_log, ...)`):
```cpp
    M(ContentAddressedGarbageCollectionLog, content_addressed_garbage_collection_log, "Per-round records of the content-addressed (CA) MergeTree garbage collector: a Start and a Finish row per GC round, with counts of objects marked/deleted, duration, outcome, and per-round ProfileEvents.") \
```
The macro auto-generates the forward declaration (`FORWARD_DECLARATION`) and the `SystemLogs::content_addressed_garbage_collection_log` member (`DECLARE_PUBLIC_MEMBERS`). Add `#include <Interpreters/ContentAddressedGarbageCollectionLog.h>` where `SystemLog.cpp` includes the concrete logs (grep `#include <Interpreters/PartLog.h>` in `src/Interpreters/SystemLog.cpp` and add ours beside it).

- [ ] **Step 2: Context accessor.** In `src/Interpreters/Context.h` near `std::shared_ptr<PartLog> getPartLog() const;` (~line 1597) add:
```cpp
    std::shared_ptr<ContentAddressedGarbageCollectionLog> getContentAddressedGarbageCollectionLog() const;
```
Forward-declare `class ContentAddressedGarbageCollectionLog;` alongside the other `class PartLog;` forward decls in Context.h. In `src/Interpreters/Context.cpp` near `getPartLog()` (~6081) add:
```cpp
std::shared_ptr<ContentAddressedGarbageCollectionLog> Context::getContentAddressedGarbageCollectionLog() const
{
    SharedLockGuard lock(shared->mutex);
    if (!shared->system_logs)
        return {};
    return shared->system_logs->content_addressed_garbage_collection_log;
}
```
Add `#include <Interpreters/ContentAddressedGarbageCollectionLog.h>` to `Context.cpp` if not pulled transitively.

- [ ] **Step 3: Config block.** In `programs/server/config.xml`, after the `<part_log>...</part_log>` block (~1255-1265) add:
```xml
    <content_addressed_garbage_collection_log>
        <database>system</database>
        <table>content_addressed_garbage_collection_log</table>
        <partition_by>toYYYYMM(event_date)</partition_by>
        <flush_interval_milliseconds>7500</flush_interval_milliseconds>
        <max_size_rows>1048576</max_size_rows>
        <reserved_size_rows>8192</reserved_size_rows>
        <buffer_size_rows_flush_threshold>524288</buffer_size_rows_flush_threshold>
        <flush_on_crash>false</flush_on_crash>
    </content_addressed_garbage_collection_log>
```
Also add the same block to the soak config `utils/ca-soak/configs/` server config if it overrides the logs section (grep `part_log` there; if absent, the default config.xml applies).

- [ ] **Step 4: Build** `ninja -C build clickhouse` → expect clean. Subagent summarizes.

- [ ] **Step 5: Manual smoke (documented, run in Phase 4 test).** The table should appear empty on startup: `SELECT * FROM system.content_addressed_garbage_collection_log` returns 0 rows without error. (Asserted in the Phase-4 functional test.)

- [ ] **Step 6: Commit** (Task 1 + Task 2 together — the log is now usable):
```bash
git add src/Interpreters/ContentAddressedGarbageCollectionLog.h src/Interpreters/ContentAddressedGarbageCollectionLog.cpp \
        src/Interpreters/SystemLog.h src/Interpreters/SystemLog.cpp src/Interpreters/Context.h src/Interpreters/Context.cpp \
        programs/server/config.xml
git commit -m "CA GC log: add system.content_addressed_garbage_collection_log SystemLog + Context accessor + config"
```

---

## Phase 2 — the scheduler emits Start/Finish records

### Task 3: the decoupled record POD + sink typedef
**Files:** Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h`.

- [ ] **Step 1: Write the POD** (no `Interpreters` dependency — pure data the Disks layer owns):
```cpp
#pragma once
#include <base/types.h>
#include <functional>
#include <map>

namespace DB::ContentAddressed
{

struct GcRoundLogRecord
{
    enum class EventType { Start, Finish };
    enum class Outcome { Led, BackedOff, Aborted };
    enum class Trigger { Scheduled, Manual };

    EventType event_type = EventType::Start;
    Outcome outcome = Outcome::Led;
    Trigger trigger = Trigger::Scheduled;
    String disk_name;
    String gc_id;        /// hex of the scheduler's gc_id
    UInt64 round = 0;
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 children_cascaded = 0;
    UInt64 duration_ms = 0;
    String error;
    std::map<String, UInt64> profile_events;
};

using GcRoundLogger = std::function<void(const GcRoundLogRecord &)>;

}
```

- [ ] **Step 2: Build** `ninja -C build clickhouse` (header-only; compiles when first included in Task 4). No commit yet (used in Task 4).

### Task 4: scheduler accepts the sink + disk name and instruments rounds
**Files:** Modify `CasGcScheduler.h`, `CasGcScheduler.cpp`.

- [ ] **Step 1: Extend the ctor + members.** In `CasGcScheduler.h`:
  - `#include ".../CasGcRoundLogRecord.h"`.
  - Change the ctor to `CasGcScheduler(Cas::StorePtr store_, std::chrono::seconds interval_, const String & log_name, String disk_name_, GcRoundLogger logger_ = {});`
  - Add members `const String disk_name;` and `const GcRoundLogger logger;`.
  - Change `void runOneRoundNow();` to `Cas::RoundReport runOneRoundNow(GcRoundLogRecord::Trigger trigger = GcRoundLogRecord::Trigger::Manual);` (return the report so the SYSTEM command/tests can use it).

- [ ] **Step 2: Implement the instrumentation helper + ctor init.** In `CasGcScheduler.cpp`:
  - ctor (`:11`): init `disk_name(std::move(disk_name_))`, `logger(std::move(logger_))`. `gc_id` already computed there — store its hex for records (add `const String gc_id_hex = u128ToHex(gc_id);` member or compute on the fly via the existing `u128ToHex`).
  - Add a private helper that runs one round with full logging (used by BOTH `loop()` and `runOneRoundNow`):
```cpp
Cas::RoundReport CasGcScheduler::runRoundLogged(Cas::Gc & gc, GcRoundLogRecord::Trigger trigger)
{
    using Rec = GcRoundLogRecord;
    auto emit = [&](const Rec & r) { if (logger) try { logger(r); } catch (...) {} };  // best-effort

    Rec start;
    start.event_type = Rec::EventType::Start;
    start.trigger = trigger; start.disk_name = disk_name; start.gc_id = u128ToHex(gc_id);
    emit(start);

    const auto t0 = std::chrono::steady_clock::now();
    ProfileEventsScope profile_scope;     // captures this round's ProfileEvents delta
    Rec fin = start;
    fin.event_type = Rec::EventType::Finish;
    try
    {
        const Cas::RoundReport rep = gc.runRegularRound();
        fin.outcome = rep.acquired_lease ? Rec::Outcome::Led : Rec::Outcome::BackedOff;
        fin.round = rep.round;
        fin.candidates_marked = rep.candidates; fin.objects_deleted = rep.deleted;
        fin.objects_absent = rep.absent; fin.objects_replaced = rep.replaced;
        fin.objects_spared = rep.spared; fin.children_cascaded = rep.cascaded;
        fin.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        fin.profile_events = snapshotToMap(profile_scope.getSnapshot());
        emit(fin);
        return rep;
    }
    catch (...)
    {
        fin.outcome = Rec::Outcome::Aborted;
        fin.error = getCurrentExceptionMessage(false);
        fin.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        fin.profile_events = snapshotToMap(profile_scope.getSnapshot());
        emit(fin);
        throw;
    }
}
```
  - `snapshotToMap`: a small local helper converting a `ProfileEvents::Counters::Snapshot` to `std::map<String,UInt64>` (iterate events, skip zero deltas: `for (Event e = 0; e < Counters::num_counters; ++e) if (snap[e]) out[ProfileEvents::getName(e)] = snap[e];`). Check the exact `ProfileEventsScope`/`Snapshot` API in `src/Common/ProfileEventsScope.h` and `src/Common/ProfileEvents.h` and match it (the snapshot indexing + `getName`).
  - `loop()` (`:62`): replace the inline `gc.runRegularRound()` + per-branch logging with `const auto rep = runRoundLogged(gc, Rec::Trigger::Scheduled);` then keep the existing `i_am_leader.store(rep.acquired_lease, ...)` and the existing LOG_DEBUG/backoff logic (those text logs stay; the table row is additional). Catch is now inside `runRoundLogged` (it rethrows) — keep the outer try/catch in `loop()` so a throwing round still sets `i_am_leader=false` and continues; `runRoundLogged` will already have emitted the Aborted Finish.
  - `runOneRoundNow(trigger)` (`:53`): `Cas::Gc gc(store, gc_id); return runRoundLogged(gc, trigger);` (drop the old standalone LOG_DEBUG or keep it — the row supersedes it).

- [ ] **Step 3: ⚠ Verify ProfileEvents capture on the background thread.** The scheduler's `loop()` runs on a raw `std::thread` (`CasGcScheduler.h`). `ProfileEventsScope` redirects the thread-local ProfileEvents pointer; confirm it captures the P0 `Cas*` increments on this thread. Build, then in the Phase-4 functional test assert the `ProfileEvents` column is **non-empty** for a Scheduled Finish row that did work. **If it is empty**, the background thread lacks a ProfileEvents context: convert `std::thread thread;` to `ThreadFromGlobalPool thread;` (which attaches a `ThreadStatus`; `CasHeartbeat` uses `ThreadFromGlobalPool`) and adjust start/join accordingly. Document which path was needed. (The `Manual` path runs on the query thread, which is always attached, so it captures regardless.)

- [ ] **Step 4: Build** `ninja -C build clickhouse unit_tests_dbms` → clean. Subagent summarizes.

### Task 5: metadata storage builds the sink, passes disk name, exposes the trigger
**Files:** Modify `ContentAddressedMetadataStorage.h/.cpp`.

- [ ] **Step 1: Disk name.** Confirm whether `ContentAddressedMetadataStorage` already has the configured disk name. Run `grep -nE "disk_name|getName|storage_path_prefix|server_id" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`. If there's no disk-name field, add a `const String disk_name;` member set from the ctor (the metadata-storage factory has the config disk name in scope — grep the factory that constructs `ContentAddressedMetadataStorage` and pass the name through). If threading the name is disproportionate, use `storage_path_prefix` as the `disk_name` value and note it. Decision recorded in the commit.

- [ ] **Step 2: Build the sink + pass to both scheduler sites.** In `ContentAddressedMetadataStorage.cpp`, add a private helper:
```cpp
ContentAddressed::GcRoundLogger ContentAddressedMetadataStorage::makeGcRoundLogger() const
{
    if (!context)
        return {};
    auto ctx = context;
    String disk = disk_name;   // or storage_path_prefix per Step 1
    return [ctx, disk](const ContentAddressed::GcRoundLogRecord & r)
    {
        auto log = ctx->getContentAddressedGarbageCollectionLog();
        if (!log)
            return;
        ContentAddressedGarbageCollectionLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        e.event_type = r.event_type == ContentAddressed::GcRoundLogRecord::EventType::Start
            ? ContentAddressedGarbageCollectionLogElement::START : ContentAddressedGarbageCollectionLogElement::FINISH;
        e.disk_name = r.disk_name.empty() ? disk : r.disk_name;
        e.gc_id = r.gc_id;
        e.trigger = r.trigger == ContentAddressed::GcRoundLogRecord::Trigger::Manual
            ? ContentAddressedGarbageCollectionLogElement::MANUAL : ContentAddressedGarbageCollectionLogElement::SCHEDULED;
        switch (r.outcome)
        {
            case ContentAddressed::GcRoundLogRecord::Outcome::Led: e.outcome = ContentAddressedGarbageCollectionLogElement::LED; break;
            case ContentAddressed::GcRoundLogRecord::Outcome::BackedOff: e.outcome = ContentAddressedGarbageCollectionLogElement::BACKED_OFF; break;
            case ContentAddressed::GcRoundLogRecord::Outcome::Aborted: e.outcome = ContentAddressedGarbageCollectionLogElement::ABORTED; break;
        }
        e.round = r.round; e.candidates_marked = r.candidates_marked; e.objects_deleted = r.objects_deleted;
        e.objects_absent = r.objects_absent; e.objects_replaced = r.objects_replaced; e.objects_spared = r.objects_spared;
        e.children_cascaded = r.children_cascaded; e.duration_ms = r.duration_ms; e.error = r.error;
        e.profile_events = r.profile_events;
        log->add(std::move(e));
    };
}
```
Include `<Interpreters/ContentAddressedGarbageCollectionLog.h>`, `<Interpreters/Context.h>`, `<Common/getFQDNOrHostName.h>`, the timestamp helper used by other logs (`timeInMicroseconds` — grep how `PartLog` callers fill `event_time_microseconds`). Pass `disk_name` and `makeGcRoundLogger()` to **both** `make_unique<CasGcScheduler>(...)` sites (`:145-152` and `:227-235`).

- [ ] **Step 2b:** Declare `makeGcRoundLogger()` (private) and `Cas::RoundReport runGarbageCollectionRoundNow();` (public) in `ContentAddressedMetadataStorage.h`.

- [ ] **Step 3: The synchronous trigger method.** In `ContentAddressedMetadataStorage.cpp`:
```cpp
Cas::RoundReport ContentAddressedMetadataStorage::runGarbageCollectionRoundNow()
{
    if (read_only || !gc_enabled)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Garbage collection is not enabled on this content-addressed disk");
    if (!gc_scheduler)
        gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(
            store(), gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full), disk_name, makeGcRoundLogger());
    return gc_scheduler->runOneRoundNow(ContentAddressed::GcRoundLogRecord::Trigger::Manual);
}
```
(Mirror the existing on-demand construction at `:145-152` — reuse `store()` exactly as that site does. Add `ErrorCodes::BAD_ARGUMENTS` if not already in the TU.)

- [ ] **Step 4: Build** `ninja -C build clickhouse unit_tests_dbms` → clean. Subagent summarizes.

- [ ] **Step 5: Commit** (Tasks 3-5):
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
git commit -m "CA GC log: scheduler emits Start/Finish records (per-round ProfileEvents) via injected sink"
```

---

## Phase 3 — the SYSTEM command

### Task 6: AST type + parser + access type
**Files:** Modify `src/Parsers/ASTSystemQuery.h`, `src/Parsers/ParserSystemQuery.cpp`, `src/Access/Common/AccessType.h`.

- [ ] **Step 1: AST type.** In `src/Parsers/ASTSystemQuery.h` `enum class Type`, add `CONTENT_ADDRESSED_GARBAGE_COLLECTION,` (before `END`). It reuses the existing `String disk;` field. `typeToString` is magic_enum-generated → `"CONTENT ADDRESSED GARBAGE COLLECTION"`.

- [ ] **Step 2: Parser.** In `src/Parsers/ParserSystemQuery.cpp`, add the case to the disk-targeted group (next to `RESTART_DISK` at ~452):
```cpp
        case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:
        {
            /// optional disk: omit => all CA disks on the node
            ParserKeyword{Keyword::ON}.ignore(pos, expected);   // (no-op guard; reuse target parser which handles ON)
            parseQueryWithOnClusterAndTarget(res, pos, expected, SystemQueryTargetType::Disk);
            break;
        }
```
**Important — optional disk:** `RESTART DISK` requires a disk. Ours is optional. So do NOT `return false` when no identifier follows. Implement as: try `parseQueryWithOnClusterAndTarget(...)`; if it returns false (no disk name present), leave `res->disk` empty and succeed (the keyword sequence already matched). Concretely, branch: if the next token is a string-literal/identifier, parse it into `res->disk`; otherwise accept with empty `disk`. Confirm the `Keyword::CONTENT_ADDRESSED_GARBAGE_COLLECTION` (or the multi-word keyword) exists in `src/Parsers/CommonParsers.h`'s keyword list; if keywords are matched word-by-word, add the keyword tokens. Check how a multi-word system command (e.g. `DROP COMPILED EXPRESSION CACHE`) is tokenized and mirror it.

- [ ] **Step 3: Access type.** In `src/Access/Common/AccessType.h` near `SYSTEM_RESTART_DISK` (~350):
```cpp
    M(SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION, "SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION", GLOBAL, SYSTEM) \
```

- [ ] **Step 4: Build** `ninja -C build clickhouse` → clean (the interpreter switch will warn about an unhandled case until Task 7 — acceptable; or do Task 7 before building). Subagent summarizes.

### Task 7: interpreter handler + disk enumeration
**Files:** Modify `src/Interpreters/InterpreterSystemQuery.cpp`.

- [ ] **Step 1: Access mapping.** In `getRequiredAccessForDDLOnCluster()` add:
```cpp
        case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:
        {
            required_access.emplace_back(AccessType::SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION);
            break;
        }
```

- [ ] **Step 2: The handler** in the big `switch (query.type)`:
```cpp
        case Type::CONTENT_ADDRESSED_GARBAGE_COLLECTION:
        {
            getContext()->checkAccess(AccessType::SYSTEM_CONTENT_ADDRESSED_GARBAGE_COLLECTION);
            runContentAddressedGarbageCollection(query.disk);
            break;
        }
```
And the method (declare in the header, define in the .cpp):
```cpp
void InterpreterSystemQuery::runContentAddressedGarbageCollection(const String & disk_name)
{
    auto run_on = [&](const String & name, const DiskPtr & disk)
    {
        auto md = disk->getMetadataStorage();
        if (!md || !md->isContentAddressed())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Disk '{}' is not a content-addressed disk", name);
        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(md.get());
        if (!ca)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Disk '{}' reports content-addressed but is not a ContentAddressedMetadataStorage", name);
        ca->runGarbageCollectionRoundNow();   // synchronous, one round
    };

    if (!disk_name.empty())
    {
        run_on(disk_name, getContext()->getDisk(disk_name));
        return;
    }
    size_t ran = 0;
    for (const auto & [name, disk] : getContext()->getDisksMap())
    {
        auto md = disk->getMetadataStorage();
        if (md && md->isContentAddressed())
        {
            run_on(name, disk);
            ++ran;
        }
    }
    if (ran == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "No content-addressed disks are configured on this node");
}
```
Includes: `<Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>`, `<Disks/IDisk.h>`. Confirm `getContext()->getDisk(name)` throws a clear error for an unknown disk (it does — keep it). Confirm `IMetadataStorage::isContentAddressed()` is the right predicate (Explore confirmed it exists, default false, overridden true in CA).

- [ ] **Step 3: Build** `ninja -C build clickhouse` → clean (switch now exhaustive). Subagent summarizes.

- [ ] **Step 4: Commit** (Tasks 6-7):
```bash
git add src/Parsers/ASTSystemQuery.h src/Parsers/ParserSystemQuery.cpp src/Parsers/CommonParsers.h \
        src/Access/Common/AccessType.h src/Interpreters/InterpreterSystemQuery.cpp src/Interpreters/InterpreterSystemQuery.h
git commit -m "CA GC: SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION [<disk>] — one synchronous round"
```

---

## Phase 4 — tests + docs

### Task 8: unit test for the scheduler sink
**Files:** Create `src/Disks/tests/gtest_cas_gc_log.cpp`.

- [ ] **Step 1: Write the test.** Construct a `CasGcScheduler` over an `InMemoryBackend`-backed `Store` with a **capturing sink** (`std::vector<GcRoundLogRecord>`), set up a dropped-then-collectable object (mirror `gtest_cas_truncate_reclaim.cpp`'s setup), then call `runOneRoundNow(Trigger::Manual)` twice. Assert: each call produced exactly one `Start` then one `Finish`; the first Finish has `candidates_marked > 0`, the second has `objects_deleted > 0`; `gc_id`/`disk_name` set; `duration_ms` is populated on Finish. Add a second case: a backend that throws on the round → Finish with `outcome == Aborted` and non-empty `error`, and `runOneRoundNow` rethrows.
```cpp
// skeleton — fill setup from gtest_cas_truncate_reclaim.cpp
TEST(CasGcLog, EmitsStartFinishWithCounts)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    /* ... build Store, publish+drop an object so it becomes collectable ... */
    std::vector<DB::ContentAddressed::GcRoundLogRecord> rows;
    DB::ContentAddressed::CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const DB::ContentAddressed::GcRoundLogRecord & r){ rows.push_back(r); });
    sched.runOneRoundNow(DB::ContentAddressed::GcRoundLogRecord::Trigger::Manual);  // marks
    sched.runOneRoundNow(DB::ContentAddressed::GcRoundLogRecord::Trigger::Manual);  // deletes
    ASSERT_EQ(rows.size(), 4u);
    ASSERT_EQ(rows[0].event_type, DB::ContentAddressed::GcRoundLogRecord::EventType::Start);
    ASSERT_EQ(rows[1].event_type, DB::ContentAddressed::GcRoundLogRecord::EventType::Finish);
    EXPECT_GT(rows[1].candidates_marked, 0u);
    EXPECT_GT(rows[3].objects_deleted, 0u);
    EXPECT_EQ(rows[1].disk_name, "ca");
}
```

- [ ] **Step 2: Run** `./build/src/unit_tests_dbms --gtest_filter='CasGcLog.*'` → PASS. Subagent summarizes. **Step 3: Commit.**

### Task 9: functional (stateless) test for the table + command
**Files:** `./tests/queries/0_stateless/add-test <NNNNN>_ca_gc_introspection` then edit the `.sql`/`.reference`.

- [ ] **Step 1: Write the SQL test.** Requires a CA disk in the test config (check whether the stateless test env has a `content_addressed` disk/policy; if not, gate with the appropriate tag or skip — grep existing CA stateless tests). Body:
```sql
-- create on the CA policy, insert, drop to make garbage, then GC and inspect the log
CREATE TABLE ca_gc_probe (id UInt64, v String) ENGINE = MergeTree ORDER BY id SETTINGS storage_policy = 'ca';
INSERT INTO ca_gc_probe SELECT number, toString(number) FROM numbers(1000);
TRUNCATE TABLE ca_gc_probe;
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca;   -- marks
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca;   -- deletes
SYSTEM FLUSH LOGS;
SELECT countIf(event_type='Start') > 0, countIf(event_type='Finish') > 0,
       countIf(trigger='Manual') > 0, sum(objects_deleted) >= 0
FROM system.content_addressed_garbage_collection_log WHERE disk_name='ca';
-- error path
SELECT throwIf(0);  -- placeholder so reference is stable
DROP TABLE ca_gc_probe;
```
Adjust the disk/policy name to whatever the stateless CA config exposes; assert at least one Start, one Finish, one Manual row, and that the `ProfileEvents` map is non-empty on a Finish that did work (`SELECT any(length(ProfileEvents)) > 0 FROM ... WHERE event_type='Finish' AND objects_deleted>0`). Add a `-- { serverError BAD_ARGUMENTS }` line for `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION <a-non-ca-disk-like-default>`.

- [ ] **Step 2: Run** the test via the stateless harness (or the praktika local runner) → PASS; write the `.reference`. Subagent summarizes. **Step 3: Commit.**

### Task 10: docs + final build/suite
**Files:** `docs/en/operations/system-tables/` (new page for the table, with the mandatory anchor + frontmatter per the repo doc rules), `docs/en/sql-reference/statements/system.md` (document the command).

- [ ] **Step 1:** Add a `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` page (frontmatter: `description`, `sidebar_label`, `sidebar_position`, `slug`, `title`, `doc_type`; every header with a `{#anchor}`) describing the columns; document `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION [<disk>]` in the SYSTEM statements page.
- [ ] **Step 2:** Full build + suites: `ninja -C build clickhouse unit_tests_dbms`; `./build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*'` (expect green except pre-existing B140); run the new stateless test. Subagent summarizes.
- [ ] **Step 3: Commit.**

---

## Self-review

**Spec coverage:** table (Task 1-2) ✓; every column incl. ProfileEvents (Task 1) ✓; Start+Finish every tick incl. no-ops (Task 4 `loop()` logs both branches) ✓; per-round ProfileEvents delta (Task 4 `ProfileEventsScope`, with the raw-thread caveat called out in Task 4 Step 3) ✓; outcome Led/BackedOff/Aborted (Task 4) ✓; injected-sink decoupling + best-effort (Task 3/5) ✓; SYSTEM command one-round/optional-disk/node-local (Task 6-7) ✓; non-CA-disk + no-CA-disks errors (Task 7) ✓; access grant (Task 6) ✓; unit + functional tests (Task 8-9) ✓; docs (Task 10) ✓.

**Placeholder scan:** the three genuine "verify-and-adjust" points are concrete, not vague: (Task 4 Step 3) ProfileEvents capture on the raw thread → named fix (`ThreadFromGlobalPool`); (Task 5 Step 1) disk-name sourcing → named fallback (`storage_path_prefix`); (Task 6 Step 2) optional-disk parsing + multi-word keyword tokenization → named files to confirm. No "TODO"/"handle edge cases".

**Type consistency:** record fields (`candidates_marked`/`objects_deleted`/…) consistent across the POD (Task 3), the element (Task 1), the sink mapping (Task 5), and the asserts (Task 8/9); enum names (`Start/Finish`, `Led/BackedOff/Aborted`, `Scheduled/Manual`) consistent across the POD enums, the SQL Enum8, and the tests; `runOneRoundNow(Trigger)` returns `RoundReport` consistently (Task 4) and is called in Task 5/8.
