# Task 2 Report: Stamp shard incarnation at birth

## Status: DONE

## Per-file changes

### `Core/CasStore.h`
- `mutateShard` (private declaration): added `ShardIncarnation birth_incarnation = {}` as last parameter,
  with a doc comment explaining the stamp-on-create semantics.
- `mutateShardForTest` (public test seam): added the same `birth_incarnation = {}` parameter and threads
  it through to the private `mutateShard`. Existing callers pass nothing and get the default `{}`.

### `Core/CasStore.cpp`
- `Store::mutateShard` definition: added the `ShardIncarnation birth_incarnation` parameter.
  In the CAS loop, right after `readShard`, added:
  ```cpp
  if (!token)
      root.incarnation = birth_incarnation;
  ```
  This stamps the incarnation on the create-if-absent path (token == nullopt) and leaves it untouched
  on subsequent mutations (token present).

### `Core/CasBuild.cpp`
- `Build::precommitAdd`: passes `ShardIncarnation{.writer_epoch = epoch, .build_sequence = build_seq}`
  as the new `birth_incarnation` arg to `store->mutateShard(...)`. The `epoch` member is
  the Store's durable `process_epoch` (= `writer_epoch`) and `build_seq` is the strictly-increasing
  per-process build sequence number. `ShardIncarnation` is visible transitively via `CasBuild.h`
  → `CasStore.h` → `CasRootShardCodec.h`.

### `src/Disks/tests/gtest_cas_store.cpp`
Two new tests added at the end of the file:

- `CasStore.ShardBornCarriesIncarnation`: creates a shard with `birth_incarnation{we=9, bs=2}` via
  `mutateShardForTest`, reads back via `backend + decodeRootShard`, and asserts the incarnation was
  stamped.
- `CasStore.RebornShardIncarnationStrictlyGreater`: creates with `{we=9, bs=2}`, simulates reclaim via
  `backend.deleteExact(key, token)`, recreates with `{we=9, bs=3}`, and asserts the second incarnation
  is strictly greater (INC-MONO invariant).

## INC-MONO Decision

**Decision: `(writer_epoch, build_sequence)` is a safe incarnation source. No dedicated allocator needed.**

Evidence:

1. **Within a single Store process**: `writer_epoch` = `process_epoch` (constant for the Store's lifetime).
   `build_sequence` = `next_build_seq++` (strictly increasing, never reset, never reused). Therefore
   any `precommitAdd` from a later build always carries a strictly greater `build_sequence`.

2. **Across process restarts**: `allocateWriterEpoch` in `CasServerRoot.cpp` provides a durable-monotone
   counter (read-modify-CAS on a persistent key). Every new process open gets `writer_epoch = next++`.
   So `writer_epoch` never decreases across restarts.

3. **Can a reclaim+recreate use a non-increasing pair?** No:
   - Within the same process: `build_seq` is always strictly greater on the next `startBuild()`.
   - Across processes: `writer_epoch` is always strictly greater after a restart.
   - A scenario where the same `writer_epoch` AND the same (or lower) `build_seq` appears on a
     newly-born shard would require reusing a process-epoch within the same process, which
     `allocateBuildSeq` (simple `next_build_seq++`, never reset) prevents.

4. `ShardIncarnation::operator<` implements lexicographic comparison over `(writer_epoch, build_sequence)`,
   which is exactly the ordering `RebornShardIncarnationStrictlyGreater` asserts.

## Test command and output

```
build/src/unit_tests_dbms --gtest_filter='CasStore.ShardBornCarriesIncarnation:CasStore.RebornShardIncarnationStrictlyGreater'
```

```
[==========] Running 2 tests from 1 test suite.
[ RUN      ] CasStore.ShardBornCarriesIncarnation
[       OK ] CasStore.ShardBornCarriesIncarnation (0 ms)
[ RUN      ] CasStore.RebornShardIncarnationStrictlyGreater
[       OK ] CasStore.RebornShardIncarnationStrictlyGreater (0 ms)
[==========] 2 tests from 1 test suite ran. (1 ms total)
[  PASSED  ] 2 tests.
```

Full regression run (`CasStore.*:CasBuild.*`): **52 tests, 0 failures**.

## Fix: deleteExact outcome assertion

**Change:** In `src/Disks/tests/gtest_cas_store.cpp`, test `CasStore.RebornShardIncarnationStrictlyGreater`, the call to `b->deleteExact(...)` on line 1464 previously discarded the returned `DeleteOutcome`. If the delete had failed, the test would proceed against a non-deleted shard and produce a confusing `EXPECT_LT` failure rather than a clear cause.

The line was changed from:
```cpp
b->deleteExact(layout.rootShardKey(ns, 0), first_got->token);
```
to:
```cpp
ASSERT_EQ(b->deleteExact(layout.rootShardKey(ns, 0), first_got->token).kind, DeleteOutcome::Kind::Deleted);
```

`DeleteOutcome` has no `operator==`; the `.kind` member (type `DeleteOutcome::Kind`) is compared directly.

**Test command and result:**
```
build/src/unit_tests_dbms --gtest_filter='CasStore.RebornShardIncarnationStrictlyGreater'
[ RUN      ] CasStore.RebornShardIncarnationStrictlyGreater
[       OK ] CasStore.RebornShardIncarnationStrictlyGreater (0 ms)
[  PASSED  ] 1 test.
```
