# ASan battery RCA — 7 findings (2026-07-18)

Root-cause analysis of 7 AddressSanitizer findings from the gtest battery. Binary:
`build_asan/src/unit_tests_dbms` (BuildId `f984015a…`). Logs:
`build/asan_battery_logs/round_<N>.log`. Symbolizer: `llvm-symbolizer-21`.

Two distinct root causes. Six of the seven collapse into one test-lifetime bug (event-sink
captures a local vector that dies before the `Pool` that fires the sink). The seventh is a
separate test-lifetime bug (a `Layout &` held across `pool.reset()`). **Neither is a product
bug**, and #23 is **not** codex finding №8.

---

## Root cause A — event-sink fires into an already-destroyed `events` vector (rounds 9, 10, 18, 19, 20, 21)

### Symbolized key frames (all six identical in shape)

ACCESS (write into freed stack slot), example round 9:
```
push_back(CasEvent) into std::vector<CasEvent>
  <lambda> TestBody()::$_0::operator()(CasEvent const&)   gtest_cas_orphan_manifest_sweep.cpp:260
  ...std::function<void(CasEvent)> dispatch...
  ~shared_ptr<DB::Cas::Pool>()                            gtest_cas_orphan_manifest_sweep.cpp:277  (scope close)
```
The `Pool` teardown that drives the sink (from the task-lead note, same for all six):
`~Pool` → `SingleWriterSlot::doTerminate` → `MountLeaseKeeper::terminate` →
`emitMountEvent` (`CasServerRoot.cpp:211`) → `CasMountRuntime::emitEvent`
(`CasMountRuntime.h:220`) → the installed `std::function` sink → `events.push_back`.

SCOPE-OWNER frame #0 in each report is the `TestBody` itself; the violated object is the
local `events` vector:

| round | test file:line (lambda push_back) | `events` decl line | `store`/`s` decl line |
|-------|-----------------------------------|--------------------|------------------------|
| 9  | gtest_cas_orphan_manifest_sweep.cpp:260 | 259  | 257 (`Pool::open`) |
| 10 | gtest_cas_orphan_manifest_sweep.cpp:300 | 299  | 298 (`openPoolForTest`) |
| 18 | gtest_cas_part_write.cpp:1515 | 1514 | before (`s = openPool`) |
| 19 | gtest_cas_part_write.cpp:1892 | 1891 | before (`s = openPool`) |
| 20 | gtest_cas_part_write.cpp:2101 | 2100 | before (`s = openBlobFaultPool`) |
| 21 | gtest_cas_part_write.cpp:2176 | 2175 | before (`s = openBlobFaultPool`) |

### Lifetime diagram

```
TestBody scope:
  line N   : auto store = Pool::open(...)          // Pool constructed FIRST
  line N+2 : std::vector<CasEvent> events;         // vector constructed SECOND
             store->setEventSink([&]{ events.push_back(e); });  // sink holds &events

  scope exit (reverse destruction order):
    1) ~events            <-- vector destroyed FIRST (declared later)
    2) ~store  ->  ~Pool  ->  mount-lease terminate  ->  emitEvent
                              ->  sink()  ->  events.push_back()   // USE-AFTER-SCOPE
```
The sink outlives the vector it writes into because the object that owns and fires the sink
(the `Pool`) is declared *before* the vector and therefore destroyed *after* it. The `Pool`
legitimately emits a farewell/terminate event during destruction; the test just wired the
sink to a shorter-lived object.

### Classification: **TEST-LIFETIME** (all six)

Product behavior is correct — emitting an audit event on mount-lease terminate during
`~Pool` is intended. The defect is purely in test object-declaration order.

### Fix direction

Guarantee the sink's captured state outlives the `Pool`. Cleanest per-test change: **declare
`std::vector<CasEvent> events;` before `auto store = …/s = …`** so destruction order tears
down the Pool first, then the vector. Equivalent alternatives: explicitly `store.reset()`
(or `store->setEventSink(nullptr)`) before the vector leaves scope, or lift `events` into a
fixture whose lifetime brackets the Pool. Prefer the reorder — it is local and matches the
"sink target must outlive the emitter" rule. This same trap will recur in any future test
that installs a reference-capturing sink after opening the Pool, so it is worth a one-line
comment near `setEventSink` in the test helpers.

---

## Root cause B — dangling `Layout &` used after `pool.reset()` (round 23)

`CasPoolShutdown.UnresolvedWedgeSkipsFarewell` — heap-use-after-free, `gtest_cas_pool.cpp`.

### Symbolized key frames

```
ACCESS  read of freed heap:
  Layout::serverRootPrefix(...)      CasLayout.h:396   (reads a std::string living in the Pool)
  Layout::mountKey(...)              CasLayout.h:422
  DB::Cas::claimMount(...)           CasServerRoot.cpp:220
  TestBody()                         gtest_cas_pool.cpp:1370

FREED by:
  operator delete
  reset()/~shared_ptr<Pool>          gtest_cas_pool.cpp:1358   ("store.reset()")

ALLOCATED by:
  operator new
  DB::Cas::Pool::open(...)           CasPool.cpp:260
  TestBody()                         gtest_cas_pool.cpp:1344
```
The 1784-byte freed region is the `Pool` object itself.

### Lifetime diagram

```
1344: auto store = Pool::open(...)               // Pool heap-allocated
1346: const Layout & layout = store->layout();    // reference BOUND INTO the Pool
...
1358: store.reset();                              // Pool freed -> `layout` now dangles
...
1370: claimMount(*backend, layout, ...)           // deref dangling `layout` -> serverRootPrefix
                                                  //   reads a freed std::string  == HEAP-UAF
```
Note `mount_key` at 1357 is copied out before the reset (safe); only the `layout` *reference*
bound at 1346 is carried across `store.reset()`.

### Classification: **TEST-LIFETIME**

`claimMount` and `Layout` are innocent — the test handed `claimMount` a reference into an
object it had already freed. No product code retains this pointer.

### Fix direction

Copy the `Layout` out of the Pool before the reset (it is copyable and cheap for the test):
replace `const Layout & layout = store->layout();` with `const Layout layout = store->layout();`
(a value), so it survives `store.reset()`. Alternatively capture whatever fields `claimMount`
needs before the reset. The reference form must not outlive the Pool.

### Is this codex finding №8? **NO.**

Codex №8 (triage doc §3.8) is a *product* raw-pointer UAF: `runGarbageCollectionRoundNow`
(`ContentAddressedMetadataStorage.cpp:359-368`) snapshots `gc_scheduler.get()` under a mutex,
unlocks, then derefs while `shutdown` (`:541`) can `reset()` the scheduler in between. Round 23
is a different object (the `Pool`, not `gc_scheduler`), a different layer (test body, not
`ContentAddressedMetadataStorage`), and a different mechanism (single-threaded dangling
reference across `reset`, no race). They are unrelated; fixing №8 will not touch this hit and
vice-versa.

---

## Summary table

| round | test | class | root cause | fix direction |
|-------|------|-------|------------|---------------|
| 9  | CasSweepLateLog.LogBetweenSealedFromAndSealIdIsReportedNotRevived | stack-use-after-scope | sink captures local `events` (line 259) by ref; `store` (line 257) destroyed after it, `~Pool` terminate fires sink into dead vector | declare `events` before `store` (or reset store first) |
| 10 | CasSweepLateLog.SecondPassSuppressedWithDedupLatchButNotWithoutOne | stack-use-after-scope | same shape; `events`@299, `store`@298 | declare `events` before `store` |
| 18 | CasPartWriteTxnRepoint.PromoteRepointsCommittedRef | stack-use-after-scope | same shape; `events`@1514, `s` before | declare `events` before `s` |
| 19 | CasPartWriteTxnStageManifestRetry.AmbiguousLandedWriteResolvesToCommittedWithoutReissue | stack-use-after-scope | same shape; `events`@1891 | declare `events` before `s` |
| 20 | CasPartWriteTxnBlobPutRetry.AmbiguousLandedWriteAdoptsOccupantWithoutReupload | stack-use-after-scope | same shape; `events`@2100 | declare `events` before `s` |
| 21 | CasPartWriteTxnPromoteStagedRetry.AmbiguousCopyLandedAdoptsDestinationWithoutRecopy | stack-use-after-scope | same shape; `events`@2175 | declare `events` before `s` |
| 23 | CasPoolShutdown.UnresolvedWedgeSkipsFarewell | heap-use-after-free | `const Layout & layout = store->layout()` (1346) used at 1370 after `store.reset()` (1358) frees the Pool | take `layout` by value, not reference |

All 7 are TEST-LIFETIME, zero product bugs. #23 ≠ codex №8.
