# Use-after-free: borrowed `ThreadGroup` outlives its parent group (S3 dedup-log + `DROP TABLE`)

**Status:** root cause confirmed under AddressSanitizer; fix implemented and verified.
**Severity:** server crash (heap use-after-free; SIGSEGV in release, ASan abort in sanitizer builds).
**Scope:** generic ClickHouse — any object-storage (S3/Azure) disk + a `MergeTree` with
`non_replicated_deduplication_window > 0` + `DROP TABLE` under concurrency. Surfaced via the
content-addressed storage backend, which puts every table on an S3-backed disk and enables the
deduplication log, so the rare race becomes frequent.

---

## TL;DR {#tldr}

A borrowed child `ThreadGroup` (created for a materialized-view pipeline or an async-insert flush)
stores only a **raw pointer** to its parent group's `MemoryTracker` / `ProfileEvents::Counters`,
without keeping the parent group alive. Separately, `MergeTreeDeduplicationLog` holds a long-lived
`WriteBufferFromS3` whose async-upload scheduler **captures and pins** the `ThreadGroup` of the query
that created the writer. On `DROP TABLE` the dedup-log is shut down, the writer is destroyed, and that
pinned query group is freed — while a concurrent detached S3 upload, running under a borrowed child of
that group, is still walking the parent chain. Result: a read of a freed `MemoryTracker` → UAF.

**Fix:** make a borrowed child `ThreadGroup` retain a `shared_ptr` to its parent group, so the parent's
trackers cannot be freed while any thread is still attached to the child. Works with parallel part
upload enabled.

---

## Symptom {#symptom}

Intermittent server crash during S3-heavy workloads with table drops. In release builds it appeared as
a `SIGSEGV` inside `ProfileEvents::Counters::increment` (a write to a tiny garbage address such as
`0x1413`) on a pooled async-upload worker. Under AddressSanitizer it reports as a `heap-use-after-free`
read of a `MemoryTracker*` in `MemoryTracker::setParent`.

The two presentations are the same bug reached through the two adjacent fields of a `ThreadGroup`:
`performance_counters` (release `0x1413` path) and `memory_tracker` (ASan path), both set in
`ThreadStatus::attachToGroupImpl` (`ThreadStatusExt.cpp:357`/`:358`).

---

## Root cause {#root-cause}

Two pre-existing facts combine into the race.

### Fact 1 — a borrowed child `ThreadGroup` does not keep its parent alive {#fact-1}

The child-group constructors initialise their trackers to point at the parent's by **raw pointer**, and
discard the parent `shared_ptr` once the constructor returns:

```cpp
// src/Interpreters/ThreadStatusExt.cpp  (createForMaterializedView / createForFlushAsyncInsertQueue)
ThreadGroup::ThreadGroup(ContextPtr query_context_, ThreadGroupPtr parent)
    : ...
    , performance_counters(VariableContext::Process, &parent->performance_counters)   // raw
    , memory_tracker(&parent->memory_tracker, VariableContext::Process, /*...*/ false) // raw
{ ... }   // no shared_ptr to `parent` is retained
```

A top-level query group (`createForQuery`) is safe on its own: its `memory_tracker.parent` is the
per-user tracker in `ProcessListForUser`, which lives for the server's lifetime (it is `reset()`, never
erased). The danger is only for **borrowed children**, whose parent is another *group* that can be freed.

### Fact 2 — the dedup-log pins a transient query group on an S3 disk {#fact-2}

`MergeTreeDeduplicationLog` keeps a long-lived `current_writer` (a `WriteBufferFromS3` on an S3 disk,
created in `load`/`rotate`). When parallel part upload is enabled, that writer's `TaskTracker` builds a
scheduler with `threadPoolCallbackRunnerUnsafe`, which captures `getCurrentThreadGroup()` — the
`ThreadGroup` of whatever query first opened the writer. That group is therefore **pinned alive by the
dedup-log** long after its query has finished.

`threadPoolCallbackRunnerUnsafe` is unsafe by contract; its header states:
*"you MUST ensure that all async tasks are finished before any objects they may use are destroyed."*

### The race {#the-race}

1. Query `Q` (connection `T1382`) creates query group **G** and opens the table's dedup-log writer →
   the writer's scheduler captures and pins **G**.
2. `Q` also creates a borrowed child group **X** (async-insert flush / MV pipeline) with
   `X.memory_tracker.parent = &G.memory_tracker` (raw). **X** schedules a deferred S3 upload.
3. `Q` returns. **G** stays alive only because the dedup-log writer pins it; **X** does *not* keep **G**
   alive.
4. `DROP TABLE` (same connection `T1382`) → `IStorage::flushAndShutdown` →
   `MergeTreeDeduplicationLog::shutdown()` destroys `current_writer` → `~WriteBufferFromS3` →
   `~TaskTracker` → the scheduler lambda drops **G**'s last reference → **G is freed**.
5. The deferred upload from step 2 runs on a pooled worker (`T1111`), constructs a `ThreadGroupSwitcher`
   for **X**, and `attachToGroupImpl` → `MemoryTracker::setParent` walks `X.parent` = `&G.memory_tracker`
   — now freed → **use-after-free**.

(The pooled worker's "created by" thread points at an unrelated connection; thread-pool workers are
global and reused, so that is just where the worker was first spawned, not the owner of the task.)

---

## Evidence {#evidence}

AddressSanitizer report (captured with a large quarantine so the real freed object survived recycling):

**Access — read of a freed `MemoryTracker*`:**
```
READ of size 8 ... thread T1111 (ThreadPool)
  MemoryTracker::setParent(MemoryTracker*)              src/Common/MemoryTracker.cpp:786
  ThreadStatus::attachToGroupImpl(...)                  src/Interpreters/ThreadStatusExt.cpp:358
  ThreadGroupSwitcher::ThreadGroupSwitcher(...)         src/Common/ThreadGroupSwitcher.cpp:51
  threadPoolCallbackRunnerUnsafe<void>(...)::lambda     src/Common/threadPoolCallbackRunner.h:37
```

**Freed by — `DROP TABLE` shutting down the dedup-log:**
```
operator delete
  threadPoolCallbackRunnerUnsafe<void>(...)::lambda::~()  src/Common/threadPoolCallbackRunner.h:33
  TaskTracker::~TaskTracker()                             src/Common/ThreadPoolTaskTracker.cpp:28
  WriteBufferFromS3::~WriteBufferFromS3()                 src/IO/WriteBufferFromS3.cpp:318
  WriteBufferWithFinalizeCallback::~WriteBufferWithFinalizeCallback()
  MergeTreeDeduplicationLog::shutdown()                   src/Storages/MergeTree/MergeTreeDeduplicationLog.cpp:402
  IStorage::flushAndShutdown(bool)
  InterpreterDropQuery::executeToTableImpl(...)           src/Interpreters/InterpreterDropQuery.cpp:350
  TCPHandler::runImpl()
```

**Previously allocated — a query `ThreadGroup`:**
```
operator new
  ThreadGroup::createForQuery(...)                        src/Interpreters/ThreadStatusExt.cpp:201
  QueryScope::create(...)                                 src/Common/QueryScope.cpp:76
  TCPHandler::runImpl()                                   src/Server/TCPHandler.cpp:572
```

---

## The fix {#the-fix}

Make a borrowed child `ThreadGroup` retain a `shared_ptr` to its parent group, so the parent's
`memory_tracker` / `performance_counters` (referenced by raw pointer) cannot be freed while any thread
is still attached to the child.

`src/Common/ThreadStatus.h` — new member on `ThreadGroup`:
```cpp
/// A borrowed child group (materialized view / async-insert flush) parents its memory_tracker and
/// performance_counters at the parent group's via RAW pointers. Retain a shared_ptr to the parent so
/// those trackers cannot be freed while any thread is still attached to this child group.
ThreadGroupPtr parent_thread_group;
```

`src/Interpreters/ThreadStatusExt.cpp` — both borrowed-child constructors
(`createForMaterializedView` and `createForFlushAsyncInsertQueue`) now initialise
`parent_thread_group(parent)`.

Top-level query / merge / background groups leave the member null — their parents are
process-lifetime trackers (user / total / background), which are never freed.

This keeps parallel part upload enabled; no workaround is required.

---

## Verification {#verification}

Reproduced on the content-addressed S3 stateless suite under AddressSanitizer with parallel upload
live:

| | Before fix | After fix |
|---|---|---|
| Time to first crash | reliably ~15 min | **58+ min, none** |
| `heap-use-after-free` in logs | present | **0** |
| `MemoryTracker::setParent` crash frame | present | **0** |
| `ThreadGroupSwitcher` crash frame | present | **0** |

---

## Upstream relevance and minimal reproduction {#upstream}

The faulting code (`ThreadGroup` borrowed-child constructors, `threadPoolCallbackRunnerUnsafe`,
`MergeTreeDeduplicationLog`) is generic ClickHouse, so this should reproduce on a plain `s3` disk
without the content-addressed backend. Proposed real-life recipe:

1. An `s3`-backed disk (e.g. MinIO) with `s3_allow_parallel_part_upload = 1` (default).
2. A `MergeTree` table on that disk with `non_replicated_deduplication_window > 0`.
3. Concurrent inserts (preferably async inserts / through a materialized view, to create borrowed child
   groups and deferred uploads) interleaved with repeated `DROP TABLE` / re-create of the table.
4. Build with `-DSANITIZE=address`.

Expected: a `heap-use-after-free` with the access / freed-by / allocated-by stacks shown above.
