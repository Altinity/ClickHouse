# MergeTree pending versions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three mutation-ordering races on plain `StorageMergeTree` (mutation vs mutation, lightweight update vs mutation, merge vs in-flight mutation) with one mechanism, as an upstream pull request built on top of PR #119155 that supersedes it.

**Architecture:** A version that is allocated but not yet visible to the selectors (a mutation between its file write and its registration, a lightweight update between block allocation and patch commit) announces itself in `pending_versions`, a map guarded by `currently_processing_in_background_mutex`, the mutex every selector already holds. Allocation and announcement are one atomic step under that mutex; erasure happens under it after the state is visible. The two version accessors treat pending versions as boundaries, so the merge predicate and patch selection inherit the rule; `selectPartsToMutate` ends its applicable range at the first pending version above the part. Plus two independent fixes the reviews found: the alter lock is held through `startMutation`, and `OPTIMIZE FINAL` rebuilds its predicate after every wait that released the mutex.

**Tech Stack:** C++23 (ClickHouse), gtest (`unit_tests_dbms`), stateless bash tests (`tests/clickhouse-test`), failpoints (`src/Common/FailPoint.cpp`), ninja/cmake in the worktree `~/workspace/ClickHouse/mt-pending-versions` (branch `mt-pending-versions` on `upstream/master` 13d023155c7).

**Spec:** `docs/superpowers/specs/2026-09-28-mergetree-uncommitted-version-boundary-design.md` (rev.10, final). Reviews: `docs/superpowers/reports/2026-09-28-cas-252-codex-reviews/`.

## Global Constraints

- Target: `upstream/master` (13d023155c7) plus the two real commits of PR #119155 (8db4dc2b953, 4398d337461) cherry-picked first; merge commits of that PR are not taken. No `MovedPart` writer on this target: `movePartitionToTable` on master already commits the destination under the parts lock (PR #108922).
- Only plain `StorageMergeTree` changes. `ReplicatedMergeTree`, `MergeTreePartsCollector`, `PatchPartsUtils`, `MergeTreeData::Transaction` are not modified. `MergeTreeMergePredicate.cpp` changes only the two accessor call names.
- No lock is held longer than today. Lock order added: `currently_processing_in_background_mutex` → `committing_blocks_mutex` (inside `allocatePendingVersion`). Never take the background mutex while holding a data-parts lock.
- Keep PR #119155's `PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE` string and its test `05227_mutation_waits_for_lightweight_update.sh` verbatim, except for a `timeout` around its `SYSTEM WAIT FAILPOINT`. Its `removeCommittingBlock` trigger is removed (the pending erase owns the wake-up).
- `canUsePartInMerges`'s `min_update_block` refusal stays. `getPatchesToApplyOnMerge`'s comparison in `PatchPartsUtils.cpp` is not changed.
- The transaction rollback recheck already in `startMutation` on master stays exactly where it is (inside the registration critical section).
- Nothing in code, comments, tests or commit messages mentions CAS, content-addressed disks or Altinity; this is a generic MergeTree fix.
- Style: Allman braces; comments explain the reason, never a plan or backlog id; functions named as `f` in prose; every new `LOGICAL_ERROR` expectation in a gtest is a death test (none is planned here).
- Stateless tests: created with `tests/queries/0_stateless/add-test <name>.sh` in the worktree (next free number there is 05291); tags `no-parallel, no-replicated-database, no-shared-merge-tree` with one comment line per tag saying why; every `SYSTEM WAIT FAILPOINT` is wrapped in `timeout 60`; a trap disables every failpoint, `wait`s background clients and drops the tables; no `sleep` as synchronisation (polling loops with a bound only).
- Commits: one per task, `git commit -s`, message subject in the imperative, body says what and why; no `Co-Authored-By` or session trailers in this branch (it is an upstream contribution); do not push.
- Build: `ninja -C build clickhouse > build/build_<task>.log 2>&1` in the worktree (no `-j`); unit tests: `ninja -C build unit_tests_dbms` then `build/src/unit_tests_dbms --gtest_filter='PendingVersions*'`; stateless tests against a standalone server started from `build/programs/clickhouse` (recipe in Task 1).

## Review Focus

1. A mutation that fails after `allocatePendingVersion` but before `prepareMutationEntry` returns (disk full while writing `mutation_N.txt`): the pending entry must disappear and a merge refused because of it must proceed afterwards. Pinned in Task 4 (gtest of the guard) and Task 8 (S7 arm).
2. `OPTIMIZE ... FINAL` on a table with a running merge while a lightweight update publishes: the retry must see the patch (fresh predicate), not silently merge without it. Pinned in Task 7 (S6).
3. `ALTER TABLE ... MODIFY SETTING enable_block_offset_column = 0` while an update is paused after allocation: the mutation must still be postponed. Pinned in Task 8 (S2b).
4. A barrier `ALTER` (`RENAME COLUMN`) when the highest pending mutation fails preparation while an earlier registered mutation is unfinished: the barrier must still wait for the earlier one. Pinned in Task 6 (S4 fence arms).
5. `KILL MUTATION` of a registered mutation while a later one is pending: `killMutation` is map-only and must neither find nor disturb the pending entry; the pending one registers normally afterwards. Pinned in Task 8 (S7 kill arm).

---

### Task 0: Branch setup and PR #119155 commits

**Files:**
- Worktree: `~/workspace/ClickHouse/mt-pending-versions` (already created on `upstream/master`, branch `mt-pending-versions`)
- Cherry-picked: `src/Storages/MergeTree/MergeTreeMutationStatus.h`, `src/Storages/StorageMergeTree.cpp`, `src/Common/FailPoint.cpp`, `tests/queries/0_stateless/05227_mutation_waits_for_lightweight_update.{sh,reference}`

**Interfaces:**
- Produces: `PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE`, failpoint `mt_lightweight_update_pause_after_block_allocation` (both from PR #119155), and the `Op::Update` loop in `selectPartsToMutate` that Task 5 replaces.

- [ ] **Step 1: Rename the branch and cherry-pick the two commits**

```bash
cd ~/workspace/ClickHouse/mt-pending-versions
git branch -m mt-pending-versions fix/mergetree-pending-versions
git log --oneline -1            # expect 13d023155c7
git cherry-pick 8db4dc2b953     # "Leave a part alone while a lightweight update of it is uncommitted"
git cherry-pick 4398d337461     # "Wake up the postponed mutation and make the test deterministic"
git log --oneline -3
```

If a cherry-pick stops on a conflict, resolve it keeping both sides' intent (the PR adds code, master may have moved lines), `git add` the files and `git cherry-pick --continue`. Do not squash and do not edit the messages: the authorship of these two commits is preserved.

- [ ] **Step 2: Confirm the cherry-picked state builds**

```bash
ninja -C build clickhouse > build/build_task0.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task0.log
tail -1 build/build_task0.log   # expect NINJA_EXIT=0
```

Use a subagent to read `build/build_task0.log` if the exit is not 0 and report only the first error.

- [ ] **Step 3: Bound the `SYSTEM WAIT FAILPOINT` of the cherry-picked test (audit finding)**

In `tests/queries/0_stateless/05227_mutation_waits_for_lightweight_update.sh`, change

```bash
$CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $FAILPOINT PAUSE"
```

to

```bash
# shellcheck disable=SC2086
if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $FAILPOINT PAUSE"; then
    echo "The lightweight update did not reach the failpoint"
    exit 1
fi
```

- [ ] **Step 4: Commit**

```bash
git add tests/queries/0_stateless/05227_mutation_waits_for_lightweight_update.sh
git commit -s -m "Bound the failpoint wait in the lightweight-update mutation test

SYSTEM WAIT FAILPOINT ... PAUSE has no timeout of its own, so an update
that fails before reaching the failpoint hangs the test until the outer
timeout instead of failing it."
```

---

### Task 1: Standalone server recipe and the ordering test (S1) as the first failing test

**Files:**
- Create: `tests/queries/0_stateless/05291_mutations_register_in_order.sh`, `.reference` (via `add-test`)
- Modify: `src/Common/FailPoint.cpp` (add `PAUSEABLE_ONCE(mt_pause_once_before_register_mutation)` next to `PAUSEABLE(mt_pause_before_register_mutation)` at line 384)
- Modify: `src/Storages/StorageMergeTree.cpp` (`extern const char mt_pause_once_before_register_mutation[];` in `namespace FailPoints`, and one `pauseFailPoint` call in `startMutation` right after the existing `mt_pause_before_register_mutation` pause)

**Interfaces:**
- Produces: failpoint `mt_pause_once_before_register_mutation` (`PAUSEABLE_ONCE`: pauses only the first mutation that reaches it) used by every later test.

- [ ] **Step 1: Add the failpoint**

`src/Common/FailPoint.cpp`, after line 384 `PAUSEABLE(mt_pause_before_register_mutation) \`:

```cpp
    PAUSEABLE_ONCE(mt_pause_once_before_register_mutation) \
```

`src/Storages/StorageMergeTree.cpp`, in `namespace FailPoints` next to `mt_pause_before_register_mutation`:

```cpp
    extern const char mt_pause_once_before_register_mutation[];
```

and in `startMutation`, directly after `FailPointInjection::pauseFailPoint(FailPoints::mt_pause_before_register_mutation);`:

```cpp
    /// Same window, but pauses only the first mutation that reaches it, so a test can hold one
    /// mutation here and watch what a second one does.
    FailPointInjection::pauseFailPoint(FailPoints::mt_pause_once_before_register_mutation);
```

- [ ] **Step 2: Write the failing test**

```bash
cd ~/workspace/ClickHouse/mt-pending-versions
./tests/queries/0_stateless/add-test mutations_register_in_order.sh   # creates 05291_*.sh and .reference
```

Content of `tests/queries/0_stateless/05291_mutations_register_in_order.sh`:

```bash
#!/usr/bin/env bash
# Tags: no-parallel, no-shared-merge-tree, no-replicated-database
# no-parallel -- the pause failpoint is server-wide: it would hold the first mutation started on any MergeTree table.
# no-shared-merge-tree -- the ordering under test is the in-memory mutation registration of StorageMergeTree.
# no-replicated-database -- a Replicated database runs DELETE through replicated DDL, not with the direct local timing assumed here.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

function cleanup()
{
    # Disabling the failpoint also releases a mutation still paused at it, so the background client finishes.
    $CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT mt_pause_once_before_register_mutation" 2>/dev/null
    wait
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_mut_order SYNC" 2>/dev/null
}
trap cleanup EXIT

# Polls a query until it returns the expected value; fails the test instead of hanging.
function wait_for_query_result()
{
    local query=$1
    local expected=$2
    local result=""
    for _ in $(seq 1 600); do
        result=$($CLICKHOUSE_CLIENT --query "$query")
        if [[ "$result" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for '$expected' from: $query (last result: '$result')"
    exit 1
}

MUTATIONS="FROM system.mutations WHERE database = currentDatabase() AND table = 't_mut_order'"

$CLICKHOUSE_CLIENT --query "
    DROP TABLE IF EXISTS t_mut_order SYNC;
    CREATE TABLE t_mut_order (id UInt64) ENGINE = MergeTree ORDER BY id;
    INSERT INTO t_mut_order SELECT number FROM numbers(100);
"

# The first DELETE allocates its block number and writes mutation_N.txt, then pauses before registering the entry.
# The failpoint pauses only the first mutation that reaches it; a later one passes through.
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT mt_pause_once_before_register_mutation"
$CLICKHOUSE_CLIENT --lightweight_deletes_sync=0 --lightweight_delete_mode=alter_update --query "DELETE FROM t_mut_order WHERE id < 50" &
paused_pid=$!
# shellcheck disable=SC2086
if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT mt_pause_once_before_register_mutation PAUSE"; then
    echo "The first DELETE did not reach the failpoint"
    exit 1
fi

echo "--- while the first mutation is paused before registration"
# Nothing is registered yet: the paused mutation has a block number and a file, but no entry.
$CLICKHOUSE_CLIENT --query "SELECT count() $MUTATIONS"
# A second mutation must wait for the first one to register instead of registering a higher version first
# (which would move every part to the higher version and lose the first mutation). It waits on the alter lock,
# so with a short lock timeout it fails instead of overtaking.
$CLICKHOUSE_CLIENT --lightweight_deletes_sync=0 --lightweight_delete_mode=alter_update --lock_acquire_timeout=1 \
    --query "DELETE FROM t_mut_order WHERE id >= 50" 2>&1 | grep -m1 -o "TIMEOUT_EXCEEDED" || echo "the second DELETE did not wait"
$CLICKHOUSE_CLIENT --query "SELECT count() $MUTATIONS"

echo "--- after the first mutation is released"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT mt_pause_once_before_register_mutation"
wait "$paused_pid"
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
# The second DELETE is retried now that the first one is registered.
$CLICKHOUSE_CLIENT --lightweight_deletes_sync=2 --lightweight_delete_mode=alter_update --query "DELETE FROM t_mut_order WHERE id >= 50"
$CLICKHOUSE_CLIENT --query "
    SELECT count() FROM t_mut_order;
    SELECT count(), countIf(is_done) $MUTATIONS;
    SELECT mutation_id, is_done $MUTATIONS ORDER BY toUInt64(extract(mutation_id, '[0-9]+'));
"
```

`tests/queries/0_stateless/05291_mutations_register_in_order.reference`:

```
--- while the first mutation is paused before registration
0
TIMEOUT_EXCEEDED
0
--- after the first mutation is released
0
2	2
mutation_2.txt	1
mutation_3.txt	1
```

- [ ] **Step 3: Build and start a standalone server**

```bash
ninja -C build clickhouse > build/build_task1.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task1.log; tail -1 build/build_task1.log
D=$PWD/tmp/srv; mkdir -p $D/data $D/tmp $D/uf $D/caches
nohup setsid build/programs/clickhouse server --config-file=programs/server/config.xml -- --path=$D/data/ --tmp_path=$D/tmp/ \
  --user_files_path=$D/uf/ --logger.log=$D/server.log --tcp_port=19681 --http_port=19682 --interserver_http_port=19683 \
  --mysql_port=0 --postgresql_port=0 --keeper_server.tcp_port=0 --keeper_server.raft_configuration.server.port=19684 \
  --prometheus.port=0 --filesystem_caches_path=$D/caches/ --custom_cached_disks_base_directory=$D/caches/ > $D/server.out 2>&1 &
sleep 8; build/programs/clickhouse client --port 19681 --query "SELECT version(); CREATE DATABASE IF NOT EXISTS test"
```

Before every test run, check that the binary is newer than the last commit: `ls -la --time-style=+%T build/programs/clickhouse; git log -1 --format=%ci`. Restart the server after every rebuild (`build/programs/clickhouse client --port 19681 --query "SYSTEM SHUTDOWN"`, then the command above).

- [ ] **Step 4: Run the test and verify it fails for the right reason**

```bash
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 600 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order > build/test_05291_before.log 2>&1; echo "TEST_EXIT=$?"
grep -A12 "^--- " build/test_05291_before.log | head -30
```

Expected: FAIL; the diff shows `the second DELETE did not wait` instead of `TIMEOUT_EXCEEDED` and `1` instead of `0` for the mutation count (the second DELETE registered first), and the wait for one done mutation times out reporting `2` (both reported done, the first one's rows lost).

- [ ] **Step 5: Commit the test and the failpoint (red)**

```bash
git add src/Common/FailPoint.cpp src/Storages/StorageMergeTree.cpp tests/queries/0_stateless/05291_mutations_register_in_order.sh tests/queries/0_stateless/05291_mutations_register_in_order.reference
git commit -s -m "Add a once-only pause failpoint before mutation registration and a test for the registration order

The test holds the first DELETE between its mutation file write and its
registration and checks that a second DELETE waits instead of
registering a higher version first. It fails before the fix: the second
DELETE succeeds, both mutations are reported done, and the rows of the
first one survive."
```

---

### Task 2: Hold the alter lock through `startMutation` (S1 green)

**Files:**
- Modify: `src/Storages/StorageMergeTree.cpp` (`mutate`, lines 1370-1380 on the base)

**Interfaces:**
- Consumes: test `05291` from Task 1.

- [ ] **Step 1: Make the lock outlive `startMutation`**

Replace, in `StorageMergeTree::mutate`:

```cpp
        /// It's important to serialize order of mutations with alter queries because
        /// they can depend on each other.
        if (auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]); alter_lock == std::nullopt)
        {
            throw Exception(
                ErrorCodes::TIMEOUT_EXCEEDED,
                "Cannot start mutation in {}ms because some metadata-changing ALTER (MODIFY|RENAME|ADD|DROP) is currently executing. "
                "You can change this timeout with `lock_acquire_timeout` setting",
                query_context->getSettingsRef()[Setting::lock_acquire_timeout].totalMilliseconds());
        }
        version = startMutation(commands, query_context);
```

with

```cpp
        /// It's important to serialize order of mutations with alter queries because
        /// they can depend on each other. The lock is held until the mutation is registered:
        /// `startMutation` allocates the block number and writes `mutation_*.txt` before it
        /// registers the entry in `current_mutations_by_version`, and two mutations that run
        /// this window concurrently can register out of order. A part mutated to the higher
        /// version then never receives the lower one (`selectPartsToMutate` starts above the
        /// part's data version), yet the lower mutation is reported done with its rows untouched.
        auto alter_lock = tryLockForAlter(query_context->getSettingsRef()[Setting::lock_acquire_timeout]);
        if (alter_lock == std::nullopt)
        {
            throw Exception(
                ErrorCodes::TIMEOUT_EXCEEDED,
                "Cannot start mutation in {}ms because some metadata-changing ALTER (MODIFY|RENAME|ADD|DROP) "
                "or another mutation is currently starting. "
                "You can change this timeout with `lock_acquire_timeout` setting",
                query_context->getSettingsRef()[Setting::lock_acquire_timeout].totalMilliseconds());
        }
        version = startMutation(commands, query_context);
```

- [ ] **Step 2: Rebuild, restart the server, run the test**

```bash
ninja -C build clickhouse > build/build_task2.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task2.log; tail -1 build/build_task2.log
build/programs/clickhouse client --port 19681 --query "SYSTEM SHUTDOWN"; sleep 5; # restart with the Task 1 command
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 600 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order > build/test_05291_after.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`, `[ OK ]`.

- [ ] **Step 3: Commit**

```bash
git add src/Storages/StorageMergeTree.cpp
git commit -s -m "Hold the alter lock until the mutation is registered

StorageMergeTree::mutate took the alter lock in the initializer of an
if statement, so it was released before startMutation ran. startMutation
allocates the mutation block number and writes mutation_<N>.txt before
registering the entry, so two concurrent mutations (for example two
lightweight DELETEs) could register out of order: a mutate task selected
in between applied N+1 alone, the part's data version became N+1, and
mutation N was never applied to it yet reported done. Rows targeted by
the lost mutation silently survived.

Keep the lock through startMutation, as the comment above it always
intended. This also lets a barrier ALTER see every mutation that
started before it."
```

---

### Task 3: `PendingVersions` container with a unit test

**Files:**
- Create: `src/Storages/MergeTree/PendingVersions.h`
- Create: `src/Storages/MergeTree/tests/gtest_pending_versions.cpp`

**Interfaces:**
- Produces:

```cpp
namespace DB
{
enum class PendingVersionKind : uint8_t { Mutation, LightweightUpdate };

/// Versions that exist (a block number is allocated) but whose state the selectors cannot see yet.
/// Guarded by the caller (StorageMergeTree::currently_processing_in_background_mutex).
class PendingVersions
{
public:
    void insert(Int64 version, PendingVersionKind kind);   /// LOGICAL_ERROR on a duplicate version
    void erase(Int64 version);                              /// no-op if absent
    bool empty() const;
    bool contains(Int64 version) const;
    std::optional<PendingVersionKind> kindOf(Int64 version) const;
    /// Lowest pending version strictly above data_version (nullopt if none).
    std::optional<Int64> firstAfter(Int64 data_version) const;
    /// Highest pending version at or below data_version (nullopt if none).
    std::optional<Int64> lastAtOrBelow(Int64 data_version) const;
    /// Highest pending version of the given kind (nullopt if none).
    std::optional<Int64> highestOfKind(PendingVersionKind kind) const;
    /// True if a pending version of the given kind exists at or below `version`.
    bool anyOfKindAtOrBelow(PendingVersionKind kind, Int64 version) const;
private:
    std::map<Int64, PendingVersionKind> versions;
};
}
```

- [ ] **Step 1: Write the failing unit test**

`src/Storages/MergeTree/tests/gtest_pending_versions.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Storages/MergeTree/PendingVersions.h>

using namespace DB;

TEST(PendingVersions, EmptyHasNoBoundaries)
{
    PendingVersions pending;
    EXPECT_TRUE(pending.empty());
    EXPECT_FALSE(pending.firstAfter(0).has_value());
    EXPECT_FALSE(pending.lastAtOrBelow(100).has_value());
    EXPECT_FALSE(pending.highestOfKind(PendingVersionKind::Mutation).has_value());
    EXPECT_FALSE(pending.anyOfKindAtOrBelow(PendingVersionKind::Mutation, 100));
}

TEST(PendingVersions, FirstAfterIsStrict)
{
    PendingVersions pending;
    pending.insert(10, PendingVersionKind::Mutation);
    pending.insert(20, PendingVersionKind::LightweightUpdate);
    EXPECT_EQ(pending.firstAfter(0), 10);
    EXPECT_EQ(pending.firstAfter(9), 10);
    EXPECT_EQ(pending.firstAfter(10), 20);   /// strictly above
    EXPECT_EQ(pending.firstAfter(19), 20);
    EXPECT_FALSE(pending.firstAfter(20).has_value());
}

TEST(PendingVersions, LastAtOrBelowIsInclusive)
{
    PendingVersions pending;
    pending.insert(10, PendingVersionKind::Mutation);
    pending.insert(20, PendingVersionKind::LightweightUpdate);
    EXPECT_FALSE(pending.lastAtOrBelow(9).has_value());
    EXPECT_EQ(pending.lastAtOrBelow(10), 10);
    EXPECT_EQ(pending.lastAtOrBelow(15), 10);
    EXPECT_EQ(pending.lastAtOrBelow(20), 20);
    EXPECT_EQ(pending.lastAtOrBelow(1000), 20);
}

TEST(PendingVersions, KindQueries)
{
    PendingVersions pending;
    pending.insert(10, PendingVersionKind::Mutation);
    pending.insert(20, PendingVersionKind::LightweightUpdate);
    pending.insert(30, PendingVersionKind::Mutation);
    EXPECT_EQ(pending.kindOf(20), PendingVersionKind::LightweightUpdate);
    EXPECT_FALSE(pending.kindOf(25).has_value());
    EXPECT_EQ(pending.highestOfKind(PendingVersionKind::Mutation), 30);
    EXPECT_EQ(pending.highestOfKind(PendingVersionKind::LightweightUpdate), 20);
    EXPECT_TRUE(pending.anyOfKindAtOrBelow(PendingVersionKind::Mutation, 10));
    EXPECT_FALSE(pending.anyOfKindAtOrBelow(PendingVersionKind::Mutation, 9));
    EXPECT_FALSE(pending.anyOfKindAtOrBelow(PendingVersionKind::LightweightUpdate, 19));
    EXPECT_TRUE(pending.anyOfKindAtOrBelow(PendingVersionKind::LightweightUpdate, 30));
}

TEST(PendingVersions, EraseAndDuplicate)
{
    PendingVersions pending;
    pending.insert(10, PendingVersionKind::Mutation);
    EXPECT_TRUE(pending.contains(10));
    pending.erase(10);
    EXPECT_FALSE(pending.contains(10));
    pending.erase(10);   /// absent: no-op
    EXPECT_TRUE(pending.empty());
    pending.insert(10, PendingVersionKind::Mutation);
    EXPECT_THROW(pending.insert(10, PendingVersionKind::LightweightUpdate), DB::Exception);
}
```

- [ ] **Step 2: Build the unit tests and verify the compile failure**

```bash
ninja -C build unit_tests_dbms > build/build_task3_red.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task3_red.log; tail -1 build/build_task3_red.log
```

Expected: `NINJA_EXIT=1`, the log's first error names `PendingVersions.h` as missing.

- [ ] **Step 3: Write the container**

`src/Storages/MergeTree/PendingVersions.h`:

```cpp
#pragma once

#include <Common/Exception.h>
#include <base/types.h>

#include <map>
#include <optional>

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

enum class PendingVersionKind : uint8_t
{
    Mutation,
    LightweightUpdate,
};

/// Versions that exist (a block number is allocated) but whose state the selectors of a plain
/// MergeTree cannot see yet: a mutation between its file write and its registration, a lightweight
/// update between block allocation and patch commit. The selectors treat them as boundaries that no
/// mutation, merge or patch selection may cross. Not thread-safe: the owner guards it with the
/// mutex the selectors decide under.
class PendingVersions
{
public:
    void insert(Int64 version, PendingVersionKind kind)
    {
        auto [it, inserted] = versions.try_emplace(version, kind);
        if (!inserted)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "Version {} is already pending", version);
    }

    void erase(Int64 version)
    {
        versions.erase(version);
    }

    bool empty() const { return versions.empty(); }
    bool contains(Int64 version) const { return versions.contains(version); }

    std::optional<PendingVersionKind> kindOf(Int64 version) const
    {
        auto it = versions.find(version);
        if (it == versions.end())
            return std::nullopt;
        return it->second;
    }

    /// Lowest pending version strictly above data_version.
    std::optional<Int64> firstAfter(Int64 data_version) const
    {
        auto it = versions.upper_bound(data_version);
        if (it == versions.end())
            return std::nullopt;
        return it->first;
    }

    /// Highest pending version at or below data_version.
    std::optional<Int64> lastAtOrBelow(Int64 data_version) const
    {
        auto it = versions.upper_bound(data_version);
        if (it == versions.begin())
            return std::nullopt;
        --it;
        return it->first;
    }

    std::optional<Int64> highestOfKind(PendingVersionKind kind) const
    {
        for (auto it = versions.rbegin(); it != versions.rend(); ++it)
            if (it->second == kind)
                return it->first;
        return std::nullopt;
    }

    bool anyOfKindAtOrBelow(PendingVersionKind kind, Int64 version) const
    {
        for (const auto & [v, k] : versions)
        {
            if (v > version)
                break;
            if (k == kind)
                return true;
        }
        return false;
    }

private:
    std::map<Int64, PendingVersionKind> versions;
};

}
```

- [ ] **Step 4: Build and run the unit tests**

```bash
ninja -C build unit_tests_dbms > build/build_task3.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task3.log; tail -1 build/build_task3.log
build/src/unit_tests_dbms --gtest_filter='PendingVersions*' 2>&1 | tail -3
```

Expected: `[  PASSED  ] 5 tests.`

- [ ] **Step 5: Commit**

```bash
git add src/Storages/MergeTree/PendingVersions.h src/Storages/MergeTree/tests/gtest_pending_versions.cpp
git commit -s -m "Add PendingVersions, the set of allocated versions a MergeTree's selectors cannot see yet

A mutation between its file write and its registration and a lightweight
update between block allocation and patch commit have a block number but
no visible state. The container keeps them ordered so the version
accessors can treat them as boundaries."
```

---

### Task 4: Atomic allocate-and-announce, the guard, and the boundary accessors

**Files:**
- Modify: `src/Storages/StorageMergeTree.h` (members, `PendingVersionGuard`, `allocatePendingVersion`, accessor renames, `PreparedMutationEntry` member order)
- Modify: `src/Storages/StorageMergeTree.cpp` (`allocatePendingVersion`, guard methods, accessors, `prepareMutationEntry`, `addPreparedMutationEntry`, the `alter` prepared/registration path)
- Modify: `src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp` (accessor names)
- Modify: `src/Storages/MergeTree/PatchParts/PatchPartsLock.h` (`PlainLightweightUpdateHolder` gains the guard)
- Test: `src/Storages/MergeTree/tests/gtest_pending_versions.cpp` (no change; the guard is exercised by the stateless tests of Tasks 5-8 because it needs a `StorageMergeTree`)

**Interfaces:**
- Produces, in `StorageMergeTree` (all names exact, later tasks use them):

```cpp
    /// Erases the pending version when the writer's state became visible or the writer failed.
    /// Destroy it under no lock; call releaseLocked() while already holding
    /// currently_processing_in_background_mutex.
    struct PendingVersionGuard
    {
        StorageMergeTree * storage = nullptr;
        Int64 version = 0;
        PendingVersionGuard() = default;
        PendingVersionGuard(StorageMergeTree & storage_, Int64 version_) : storage(&storage_), version(version_) {}
        PendingVersionGuard(PendingVersionGuard && other) noexcept;
        PendingVersionGuard & operator=(PendingVersionGuard && other) noexcept;
        PendingVersionGuard(const PendingVersionGuard &) = delete;
        PendingVersionGuard & operator=(const PendingVersionGuard &) = delete;
        ~PendingVersionGuard();                 /// takes the background mutex if still armed
        void releaseLocked();                   /// erase + notify + disarm; caller holds the mutex
        bool armed() const { return storage != nullptr; }
    };

    struct AllocatedPendingVersion
    {
        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
        PendingVersionGuard guard;
    };

    /// Allocates a block number and announces it as pending in one step under
    /// currently_processing_in_background_mutex, so no selector can see the version without it.
    AllocatedPendingVersion allocatePendingVersion(CommittingBlock::Op op, PendingVersionKind kind);

    UInt64 getCurrentVersionBoundary(UInt64 data_version, std::unique_lock<std::mutex> &) const;   /// was getCurrentMutationVersion
    UInt64 getNextVersionBoundary(UInt64 data_version, std::unique_lock<std::mutex> &) const;      /// was getNextMutationVersion
    PendingVersions pending_versions;   /// guarded by currently_processing_in_background_mutex
```

  Locking rule for the guard: `currently_processing_in_background_mutex` is a plain `std::mutex`, so a `PendingVersionGuard` must never be destroyed (or move-assigned over while armed) by a thread that already holds that mutex; either call `releaseLocked()` while locked, or declare the guard so it is destroyed after the lock is released. Every task that adds a guard must state where it is destroyed and which locks are held there.

  and `PreparedMutationEntry` reordered to `{ block_holder, pending_guard, entry, version, mutation_id, additional_info }` so destruction runs entry (file removal) → pending_guard (erase) → block_holder (release).

- [ ] **Step 1: Header changes**

In `src/Storages/StorageMergeTree.h`: add `#include <Storages/MergeTree/PendingVersions.h>`; next to `current_mutations_by_version` add

```cpp
    /// Allocated versions whose state the selectors cannot see yet; same mutex as the map above.
    PendingVersions pending_versions;
```

add the `PendingVersionGuard`, `AllocatedPendingVersion` and `allocatePendingVersion` declarations from the Interfaces block (public section, near `allocateBlockNumber`); rename the two accessor declarations (lines 283-284) to `getCurrentVersionBoundary` / `getNextVersionBoundary` with the comment

```cpp
    /// Highest / lowest version boundary around data_version: registered mutations and pending
    /// versions alike. The merge predicate refuses to merge across a boundary and patch selection
    /// never materializes a patch from above one.
```

and rewrite `PreparedMutationEntry`:

```cpp
    struct PreparedMutationEntry
    {
        /// Declared first so it is destroyed last: the file is removed (entry), then the version
        /// stops being pending (pending_guard), then the block number is released.
        std::unique_ptr<PlainCommittingBlockHolder> block_holder;
        PendingVersionGuard pending_guard;
        MergeTreeMutationEntry entry;
        Int64 version;
        String mutation_id;
        String additional_info;
    };
```

In `src/Storages/MergeTree/PatchParts/PatchPartsLock.h`, forward-declare nothing: instead change `PlainLightweightUpdateHolder` to

```cpp
struct PlainLightweightUpdateHolder
{
    std::unique_ptr<PlainLightweightUpdateLock> update_lock;
    /// Declared before the guard so the version stops being pending before its block is released.
    std::unique_ptr<PlainCommittingBlockHolder> block_holder;
    std::function<void()> release_pending;   /// set by StorageMergeTree::updateLightweight; empty until then
};
```

(The holder lives in a header that must not include `StorageMergeTree.h`; a `std::function` set by the storage keeps the dependency one-way. Destruction order: `release_pending` is destroyed first but it is only a callable; the storage installs a callable that erases the pending version, and `updateLightweight` calls it from a small RAII wrapper described in Task 5.)

- [ ] **Step 2: Implement the guard and the allocation**

In `src/Storages/StorageMergeTree.cpp`:

```cpp
StorageMergeTree::PendingVersionGuard::PendingVersionGuard(PendingVersionGuard && other) noexcept
    : storage(std::exchange(other.storage, nullptr)), version(other.version)
{
}

StorageMergeTree::PendingVersionGuard & StorageMergeTree::PendingVersionGuard::operator=(PendingVersionGuard && other) noexcept
{
    if (this != &other)
    {
        if (armed())
        {
            std::lock_guard lock(storage->currently_processing_in_background_mutex);
            releaseLocked();
        }
        storage = std::exchange(other.storage, nullptr);
        version = other.version;
    }
    return *this;
}

StorageMergeTree::PendingVersionGuard::~PendingVersionGuard()
{
    if (!armed())
        return;
    {
        std::lock_guard lock(storage->currently_processing_in_background_mutex);
        releaseLocked();
    }
    /// Something may have been postponed behind this version; let the scheduler look again now
    /// rather than at its next backoff tick.
    storage->background_operations_assignee.trigger();
}

void StorageMergeTree::PendingVersionGuard::releaseLocked()
{
    if (!armed())
        return;
    storage->pending_versions.erase(version);
    LOG_DEBUG(storage->log, "Version {} is no longer pending", version);
    storage->currently_processing_in_background_condition.notify_all();
    storage = nullptr;
}

StorageMergeTree::AllocatedPendingVersion StorageMergeTree::allocatePendingVersion(CommittingBlock::Op op, PendingVersionKind kind)
{
    /// Allocation and announcement are one step with respect to the selectors, which read
    /// pending_versions under this mutex: no selector can observe the block number before it is
    /// announced. Lock order: this mutex, then committing_blocks_mutex inside allocateBlockNumber,
    /// the order the merge predicate already uses when it snapshots the committing blocks.
    std::lock_guard lock(currently_processing_in_background_mutex);
    auto block_holder = allocateBlockNumber(op);
    const Int64 version = block_holder->block.number;
    pending_versions.insert(version, kind);
    LOG_DEBUG(log, "Version {} is pending ({})", version, kind == PendingVersionKind::Mutation ? "mutation" : "lightweight update");
    return {std::move(block_holder), PendingVersionGuard(*this, version)};
}
```

The destructor triggers the assignee **after** releasing the mutex (the trigger takes its own lock and can run the scheduler). In `releaseLocked` the trigger is not needed: registration (`addPreparedMutationEntry`) already triggers.

- [ ] **Step 3: Accessors**

Replace `getCurrentMutationVersion` and `getNextMutationVersion` (base lines 2607-2616, 2762-2770) with

```cpp
UInt64 StorageMergeTree::getCurrentVersionBoundary(UInt64 data_version, std::unique_lock<std::mutex> & /*currently_processing_in_background_mutex_lock*/) const
{
    UInt64 result = 0;
    auto it = current_mutations_by_version.upper_bound(data_version);
    if (it != current_mutations_by_version.begin())
        result = std::prev(it)->first;
    if (auto pending = pending_versions.lastAtOrBelow(static_cast<Int64>(data_version)))
        result = std::max(result, static_cast<UInt64>(*pending));
    return result;
}

UInt64 StorageMergeTree::getNextVersionBoundary(UInt64 data_version, std::unique_lock<std::mutex> & /*currently_processing_in_background_mutex_lock*/) const
{
    /// 0 means "no boundary above", as before.
    UInt64 result = 0;
    auto it = current_mutations_by_version.upper_bound(data_version);
    if (it != current_mutations_by_version.end())
        result = it->first;
    if (auto pending = pending_versions.firstAfter(static_cast<Int64>(data_version)))
        result = result == 0 ? static_cast<UInt64>(*pending) : std::min(result, static_cast<UInt64>(*pending));
    return result;
}
```

In `MergeTreeMergePredicate.cpp` rename the three calls: `storage.getCurrentMutationVersion(` → `storage.getCurrentVersionBoundary(` (two, in `canMergeParts`), `storage.getNextMutationVersion(` → `storage.getNextVersionBoundary(` (one, in `getPatchesToApplyOnMerge`). Grep the tree for other callers (`git grep -n "getCurrentMutationVersion\|getNextMutationVersion" -- src`) and rename them the same way; there are no others on the base.

- [ ] **Step 4: Mutation preparation and registration**

In `prepareMutationEntry` replace

```cpp
    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());
    auto block_holder = allocateBlockNumber(CommittingBlock::Op::Mutation);

    Int64 version = block_holder->block.number;
    entry.commit(version);
```

with

```cpp
    /// Allocate and announce first, then build the entry: on an exception the locals unwind in
    /// reverse order, so the file is removed before the version stops being pending and its block
    /// is released.
    auto allocated = allocatePendingVersion(CommittingBlock::Op::Mutation, PendingVersionKind::Mutation);
    MergeTreeMutationEntry entry(commands, disk, relative_data_path, insert_increment.get(), current_tid, getContext()->getWriteSettings());

    Int64 version = allocated.block_holder->block.number;
    entry.commit(version);
```

and the `return` with

```cpp
    return {std::move(allocated.block_holder), std::move(allocated.guard), std::move(entry), version, std::move(mutation_id), std::move(additional_info)};
```

In `addPreparedMutationEntry`, after `it->second.is_registered = true;`:

```cpp
    /// Registered: the entry is visible to the selectors under this same mutex.
    prepared.pending_guard.releaseLocked();
```

(`addPreparedMutationEntry` takes `prepared` by value; the guard member is moved in with it.)

- [ ] **Step 5: The `alter` path: registration and the recovery interval**

In `StorageMergeTree::alter`, the `prepared.emplace(prepareMutationEntry(...))` call needs no change (the guard travels inside `prepared`). In the recovery branch (base lines 848-869), where the registered mutation is moved into `held_entry`, keep the version pending across the durable rollback:

```cpp
                    std::optional<MergeTreeMutationEntry> held_entry;
                    Int64 held_version = -1;
                    PendingVersionGuard held_guard;
                    if (mutation_registered)
                    {
                        auto it = current_mutations_by_version.find(mutation_version);
                        if (it != current_mutations_by_version.end())
                        {
                            decrementMutationsCounters(mutation_counters, *it->second.commands);
                            held_version = it->first;
                            held_entry.emplace(std::move(it->second));
                            current_mutations_by_version.erase(it);
                            /// Registered again or terminally gone only after the durable rollback is
                            /// decided; until then the version stays a boundary, never absent from
                            /// both the map and the pending set.
                            pending_versions.insert(held_version, PendingVersionKind::Mutation);
                            held_guard = PendingVersionGuard(*this, held_version);
                        }
                    }
                    else if (prepared)
                    {
                        held_version = prepared->version;
                        held_entry.emplace(std::move(prepared->entry));
                        held_guard = std::move(prepared->pending_guard);   /// still armed: never registered
                    }

                    background_lock.unlock();

                    FailPointInjection::pauseFailPoint(FailPoints::mt_alter_pause_before_durable_rollback);
```

then, in the `durable_rolled_back` branch after `held_entry->removeFile()` (inside its try), nothing else: `held_guard` is destroyed at the end of the scope, which erases the pending version under the mutex and notifies. In the `else` branch (re-registration under `relock`), after `background_operations_assignee.trigger();` add

```cpp
                                held_guard.releaseLocked();   /// registered again, under relock
```

and if `held_entry` is absent (nothing to re-register) the guard destructor handles it. Add the failpoint `PAUSEABLE_ONCE(mt_alter_pause_before_durable_rollback)` to `src/Common/FailPoint.cpp` next to `ONCE(mt_alter_throw_in_durable_rollback)` and its `extern` in `namespace FailPoints`.

- [ ] **Step 6: Build, run the two existing tests, commit**

```bash
ninja -C build clickhouse unit_tests_dbms > build/build_task4.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task4.log; tail -1 build/build_task4.log
build/src/unit_tests_dbms --gtest_filter='PendingVersions*' 2>&1 | tail -1
# restart the server, then:
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 900 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order 05227_mutation_waits_for_lightweight_update 04516_mutation_kill_transaction_race > build/test_task4.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: all three `[ OK ]` (04516 needs the transactions config of the standalone server: if it is skipped or fails only on `transactions.lib` setup, note it and run it in the CI-style runner later; it must not fail on a mutation assertion).

```bash
git add src/Storages/StorageMergeTree.h src/Storages/StorageMergeTree.cpp src/Storages/MergeTree/Compaction/MergePredicates/MergeTreeMergePredicate.cpp src/Storages/MergeTree/PatchParts/PatchPartsLock.h src/Common/FailPoint.cpp
git commit -s -m "Announce a mutation's version as pending from its allocation until its registration

The block number of a mutation is allocated and mutation_<N>.txt is
written before the entry is registered, and the selectors decide from
the registered map only. Allocate and announce in one step under
currently_processing_in_background_mutex, erase the pending version in
the registration critical section (or on failure), and count pending
versions in the two version boundaries the merge predicate and patch
selection read. The alter recovery path keeps a temporarily
unregistered mutation pending until its durable rollback is decided."
```

---

### Task 5: Lightweight updates announce their version; `selectPartsToMutate` ends at the first pending version (S2b, S2c, cancellation)

**Files:**
- Modify: `src/Storages/StorageMergeTree.cpp` (`updateLightweight`, `selectPartsToMutate`, remove the `Op::Update` loop of #119155 and the `removeCommittingBlock` trigger)
- Modify: `src/Storages/MergeTree/MergeTreeMutationStatus.h` (`PENDING_MUTATION_REGISTRATION`)
- Create: `tests/queries/0_stateless/05292_mutation_waits_for_pending_versions.sh`, `.reference`

**Interfaces:**
- Consumes: `allocatePendingVersion`, `PendingVersionGuard`, `PlainLightweightUpdateHolder::release_pending` (Task 4); failpoint `mt_lightweight_update_pause_after_block_allocation` (Task 0).
- Produces: `PostponeReasons::PENDING_MUTATION_REGISTRATION = "Mutation with a lower block number is not registered yet"`.

- [ ] **Step 1: Write the failing test**

```bash
./tests/queries/0_stateless/add-test mutation_waits_for_pending_versions.sh
```

`tests/queries/0_stateless/05292_mutation_waits_for_pending_versions.sh`:

```bash
#!/usr/bin/env bash
# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
# no-parallel -- server-wide failpoints pause the next lightweight update and the next mutation on any table.
# no-replicated-database -- DELETE and UPDATE run through replicated DDL there, not with the local timing assumed here.
# no-shared-merge-tree -- the ordering under test is the in-memory version bookkeeping of StorageMergeTree.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

UPDATE_FP="mt_lightweight_update_pause_after_block_allocation"
MUTATION_FP="mt_pause_once_before_register_mutation"
TASK_FP="mt_mutate_task_pause_in_prepare"
TABLE="t_pending_versions"
MUTATIONS="FROM system.mutations WHERE database = currentDatabase() AND table = '$TABLE'"

function cleanup()
{
    for fp in $UPDATE_FP $MUTATION_FP $TASK_FP; do
        $CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $fp" 2>/dev/null
    done
    wait
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS $TABLE SYNC" 2>/dev/null
}
trap cleanup EXIT

function wait_for_query_result()
{
    local query=$1
    local expected=$2
    local result=""
    for _ in $(seq 1 600); do
        result=$($CLICKHOUSE_CLIENT --query "$query")
        if [[ "$result" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for '$expected' from: $query (last result: '$result')"
    exit 1
}

function wait_failpoint()
{
    # shellcheck disable=SC2086
    if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $1 PAUSE"; then
        echo "failpoint $1 was not reached"
        exit 1
    fi
}

function fresh_table()
{
    $CLICKHOUSE_CLIENT --query "
        DROP TABLE IF EXISTS $TABLE SYNC;
        CREATE TABLE $TABLE (id UInt64, v UInt64, w UInt64) ENGINE = MergeTree ORDER BY id
        SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1, min_bytes_for_wide_part = 0;
        INSERT INTO $TABLE SELECT number, 1, 0 FROM numbers(1000);
    "
}

echo "--- the guard of the mutation selector does not depend on the table settings"
fresh_table
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $UPDATE_FP"
$CLICKHOUSE_CLIENT --enable_lightweight_update 1 --query "UPDATE $TABLE SET v = 2 WHERE v = 1" &
update_pid=$!
wait_failpoint $UPDATE_FP
# A settings ALTER that makes supportsLightweightUpdate() false must not hide the pending update.
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE MODIFY SETTING enable_block_offset_column = 0"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE w = 5 WHERE 1 SETTINGS mutations_sync = 0"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done AND arrayExists(x -> x LIKE 'Lightweight update%', mapValues(parts_postpone_reasons))" "1"
$CLICKHOUSE_CLIENT --query "SELECT 'postponed:', arrayDistinct(mapValues(parts_postpone_reasons)) $MUTATIONS"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE MODIFY SETTING enable_block_offset_column = 1"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $UPDATE_FP"
wait "$update_pid"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done" "0"
$CLICKHOUSE_CLIENT --query "SELECT 'update survived', count() FROM $TABLE WHERE v = 2; SELECT 'mutation applied', count() FROM $TABLE WHERE w = 5;"

echo "--- a cancelled pending update releases the postponed mutation"
fresh_table
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $UPDATE_FP"
$CLICKHOUSE_CLIENT --enable_lightweight_update 1 --query_id "05292_cancelled_update_${CLICKHOUSE_DATABASE}" --query "UPDATE $TABLE SET v = 3 WHERE v = 1" 2>/dev/null &
update_pid=$!
wait_failpoint $UPDATE_FP
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE w = 7 WHERE 1 SETTINGS mutations_sync = 0"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done AND arrayExists(x -> x LIKE 'Lightweight update%', mapValues(parts_postpone_reasons))" "1"
# A thread parked in a failpoint notices the kill only when the failpoint is disabled, so the kill must not wait.
$CLICKHOUSE_CLIENT --query "KILL QUERY WHERE query_id = '05292_cancelled_update_${CLICKHOUSE_DATABASE}' ASYNC FORMAT Null"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $UPDATE_FP"
wait "$update_pid"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done" "0"
$CLICKHOUSE_CLIENT --query "SELECT 'no patch committed', count() FROM $TABLE WHERE v = 3; SELECT 'mutation applied', count() FROM $TABLE WHERE w = 7;"

echo "--- a lower registered mutation applies while a higher one waits behind a pending update"
fresh_table
# K registers and is held inside its mutate task, so it stays 'in progress' while U and M are arranged.
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $TASK_FP"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE w = 1 WHERE 1 SETTINGS mutations_sync = 0"
wait_failpoint $TASK_FP
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $UPDATE_FP"
$CLICKHOUSE_CLIENT --enable_lightweight_update 1 --query "UPDATE $TABLE SET v = 4 WHERE v = 1" &
update_pid=$!
wait_failpoint $UPDATE_FP
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE w = w + 10 WHERE 1 SETTINGS mutations_sync = 0"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $TASK_FP"
# K finishes; M is postponed behind U.
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done AND arrayExists(x -> x LIKE 'Lightweight update%', mapValues(parts_postpone_reasons))" "1"
$CLICKHOUSE_CLIENT --query "SELECT 'after K, before U:', w, count() FROM $TABLE GROUP BY w ORDER BY w"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $UPDATE_FP"
wait "$update_pid"
wait_for_query_result "SELECT count() $MUTATIONS AND NOT is_done" "0"
$CLICKHOUSE_CLIENT --query "SELECT 'after U and M:', v, w, count() FROM $TABLE GROUP BY v, w ORDER BY v, w"
```

`.reference`:

```
--- the guard of the mutation selector does not depend on the table settings
postponed:	['Lightweight update with a lower block number is not committed yet']
update survived	1000
mutation applied	1000
--- a cancelled pending update releases the postponed mutation
no patch committed	0
mutation applied	1000
--- a lower registered mutation applies while a higher one waits behind a pending update
after K, before U:	1	1000
after U and M:	4	11	1000
```

- [ ] **Step 2: Run it and verify it fails for the right reason**

Rebuild is not needed (no code change yet). Run:

```bash
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 900 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05292_mutation_waits_for_pending_versions > build/test_05292_before.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: FAIL in the first section: with `enable_block_offset_column = 0` the #119155 guard skips the snapshot, the mutation is not postponed, the wait for the postpone reason times out (or the update is lost: `update survived 0`).

- [ ] **Step 3: Implement**

`src/Storages/MergeTree/MergeTreeMutationStatus.h`, after `PENDING_LIGHTWEIGHT_UPDATE`:

```cpp
    inline constexpr auto PENDING_MUTATION_REGISTRATION = "Mutation with a lower block number is not registered yet";
```

`updateLightweight`: replace

```cpp
    update_holder.block_holder = allocateBlockNumber(CommittingBlock::Op::Update);
```

with

```cpp
    /// Announced as a pending version from here until the patch parts are committed: mutation
    /// selection stops below it, merges do not cross it, and patch selection does not reach over it.
    auto allocated = allocatePendingVersion(CommittingBlock::Op::Update, PendingVersionKind::LightweightUpdate);
    update_holder.block_holder = std::move(allocated.block_holder);
    update_holder.release_pending = [guard = std::make_shared<PendingVersionGuard>(std::move(allocated.guard))]() mutable { guard.reset(); };
```

and add a destructor to `PlainLightweightUpdateHolder` in `PatchPartsLock.h`:

```cpp
    ~PlainLightweightUpdateHolder()
    {
        /// The version stops being pending after the patch parts are committed (this holder is
        /// destroyed with the sink) and before the block number is released below.
        if (release_pending)
            release_pending();
    }
```

(Members are destroyed after the destructor body, so `block_holder` is released after the pending erase.) Remove the `mt_lightweight_update_pause_after_block_allocation` pause? No: it stays where #119155 put it (after `waitForCommittingInsertsAndMutations`), which is after the announcement, so the paused update is already pending.

`selectPartsToMutate`: delete the `Op::Update` block that #119155 added (the `committing_blocks_snapshot` declaration and the `pending_update_block` loop with `PENDING_LIGHTWEIGHT_UPDATE`), and replace

```cpp
        auto mutations_begin_it = current_mutations_by_version.upper_bound(part->info.getDataVersion());
        if (mutations_begin_it == mutations_end_it)
            continue;
```

with

```cpp
        auto mutations_begin_it = current_mutations_by_version.upper_bound(part->info.getDataVersion());
        if (mutations_begin_it == current_mutations_by_version.end())
            continue;

        /// A pending version (a mutation not yet registered, a lightweight update whose patch is not
        /// committed) is a boundary: only the registered mutations below it may be applied now. If
        /// every mutation this part still needs lies above one, the part waits.
        auto mutations_end_it = current_mutations_by_version.end();
        if (auto pending = pending_versions.firstAfter(part->info.getDataVersion()))
        {
            mutations_end_it = current_mutations_by_version.lower_bound(static_cast<UInt64>(*pending));
            if (mutations_begin_it == mutations_end_it)
            {
                const auto kind = pending_versions.kindOf(*pending);
                current_parts_postpone_reasons[part->name] = kind == PendingVersionKind::LightweightUpdate
                    ? PostponeReasons::PENDING_LIGHTWEIGHT_UPDATE
                    : PostponeReasons::PENDING_MUTATION_REGISTRATION;
                LOG_DEBUG(log, "Will not mutate part {} yet: version {} is pending", part->name, *pending);
                continue;
            }
        }
```

and delete the earlier `auto mutations_end_it = current_mutations_by_version.end();` above the loop (it is now per part). In `removeCommittingBlock`, delete the `if (block.op == CommittingBlock::Op::Update) background_operations_assignee.trigger();` that #119155 added (the guard destructor triggers).

- [ ] **Step 4: Rebuild, restart, run the three tests**

```bash
ninja -C build clickhouse > build/build_task5.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task5.log; tail -1 build/build_task5.log
# restart the server
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 900 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order 05292_mutation_waits_for_pending_versions 05227_mutation_waits_for_lightweight_update > build/test_task5.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: all `[ OK ]`; `05227` still prints its original reason string.

- [ ] **Step 5: Commit**

```bash
git add src/Storages/StorageMergeTree.cpp src/Storages/MergeTree/MergeTreeMutationStatus.h src/Storages/MergeTree/PatchParts/PatchPartsLock.h tests/queries/0_stateless/05292_mutation_waits_for_pending_versions.sh tests/queries/0_stateless/05292_mutation_waits_for_pending_versions.reference
git commit -s -m "Announce a lightweight update's version as pending until its patch parts are committed

Mutation selection now ends the applicable range of a part at the first
pending version above it, for lightweight updates and for mutations
that are not registered yet alike, and reports which kind it waits for.
This replaces the committing-block scan that was guarded by
supportsLightweightUpdate(): that guard reads two table settings a
concurrent settings ALTER can flip while an update still holds its
block number, which hid the update from the scan. The wake-up moves
from removeCommittingBlock to the release of the pending version."
```

---

### Task 6: Barrier `ALTER` waits through a fence of registered and pending mutations (S4)

> **Execution ruling (2026-09-28):** the fence in Step 3 was not implemented. On this target a pending mutation's writer always holds the alter lock (`mutate` since Task 2, `alter` for its rename mutation), and the barrier runs under that lock, so `pending_versions` never holds a `Mutation` at the fence. The barrier branch received a comment stating that invariant instead; test 05293 pins the lock (`DEADLOCK_AVOIDED` on the direct path) and that a barrier issued while a mutation is pending completes only after every earlier mutation. See the SDD ledger.

**Files:**
- Modify: `src/Storages/StorageMergeTree.cpp` (`alter`, the barrier branch at base lines 688-708)
- Create: `tests/queries/0_stateless/05293_barrier_alter_waits_for_pending_mutation.sh`, `.reference`

**Interfaces:**
- Consumes: `pending_versions`, `PendingVersionKind::Mutation`, `currently_processing_in_background_condition` notified by `PendingVersionGuard::releaseLocked` (Task 4).

- [ ] **Step 1: Write the failing test**

```bash
./tests/queries/0_stateless/add-test barrier_alter_waits_for_pending_mutation.sh
```

```bash
#!/usr/bin/env bash
# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
# no-parallel -- server-wide failpoints pause the next mutation registration on any table.
# no-replicated-database -- ALTER and DELETE run through replicated DDL there, not with the local timing assumed here.
# no-shared-merge-tree -- the barrier logic under test is StorageMergeTree::alter.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

MUTATION_FP="mt_pause_once_before_register_mutation"
THROW_FP="mt_throw_after_mutation_commit"
TASK_FP="mt_mutate_task_pause_in_prepare"
TABLE="t_barrier_fence"
MUTATIONS="FROM system.mutations WHERE database = currentDatabase() AND table = '$TABLE'"

function cleanup()
{
    for fp in $MUTATION_FP $THROW_FP $TASK_FP; do
        $CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $fp" 2>/dev/null
    done
    wait
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS $TABLE SYNC" 2>/dev/null
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS ${TABLE}_alias SYNC" 2>/dev/null
}
trap cleanup EXIT

function wait_for_query_result()
{
    local query=$1
    local expected=$2
    local result=""
    for _ in $(seq 1 600); do
        result=$($CLICKHOUSE_CLIENT --query "$query")
        if [[ "$result" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for '$expected' from: $query (last result: '$result')"
    exit 1
}

function wait_failpoint()
{
    # shellcheck disable=SC2086
    if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $1 PAUSE"; then
        echo "failpoint $1 was not reached"
        exit 1
    fi
}

function fresh_table()
{
    $CLICKHOUSE_CLIENT --query "
        DROP TABLE IF EXISTS ${TABLE}_alias SYNC;
        DROP TABLE IF EXISTS $TABLE SYNC;
        CREATE TABLE $TABLE (id UInt64, v UInt64) ENGINE = MergeTree ORDER BY id;
        CREATE TABLE ${TABLE}_alias ENGINE = Alias('$TABLE');
        INSERT INTO $TABLE SELECT number, 0 FROM numbers(100);
    "
}

echo "--- a barrier ALTER does not overtake a mutation that is paused before registration"
fresh_table
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $MUTATION_FP"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE v = 1 WHERE 1 SETTINGS mutations_sync = 0" &
paused_pid=$!
wait_failpoint $MUTATION_FP
# Directly: the alter lock is held by the paused mutation, so a short lock timeout fails the barrier.
$CLICKHOUSE_CLIENT --lock_acquire_timeout=1 --query "ALTER TABLE $TABLE RENAME COLUMN v TO v2" 2>&1 | grep -m1 -o "TIMEOUT_EXCEEDED" || echo "the direct barrier did not wait"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $MUTATION_FP"
wait "$paused_pid"
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE RENAME COLUMN v TO v2"
$CLICKHOUSE_CLIENT --query "SELECT 'renamed after the mutation:', count() FROM $TABLE WHERE v2 = 1"

echo "--- the fence still waits for an earlier registered mutation when the highest pending one fails"
fresh_table
# N registers and is held inside its task; P allocates above N and pauses before registration.
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $TASK_FP"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE v = 2 WHERE 1 SETTINGS mutations_sync = 0"
wait_failpoint $TASK_FP
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $MUTATION_FP"
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $THROW_FP"
# P will throw right after its file is written, once released.
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE v = 3 WHERE 1 SETTINGS mutations_sync = 0" 2>/dev/null &
failing_pid=$!
wait_failpoint $MUTATION_FP
# Through the alias, the barrier reaches the storage while P is pending: it must capture P as the
# fence and, after P fails, still wait for N. The RENAME is started in the background so the test
# can observe it waiting.
$CLICKHOUSE_CLIENT --query "ALTER TABLE ${TABLE}_alias RENAME COLUMN v TO v2" &
barrier_pid=$!
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $MUTATION_FP"
wait "$failing_pid" || true
# P is gone (it failed); N is still running; the barrier must not have completed.
wait_for_query_result "SELECT count() $MUTATIONS" "1"
if ! kill -0 "$barrier_pid" 2>/dev/null; then
    echo "the barrier overtook the running mutation"
fi
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $TASK_FP"
wait "$barrier_pid"
$CLICKHOUSE_CLIENT --query "SELECT 'renamed after N:', count() FROM $TABLE WHERE v2 = 2"
$CLICKHOUSE_CLIENT --query "SELECT 'mutations:', count(), countIf(is_done) $MUTATIONS"
```

`.reference`:

```
--- a barrier ALTER does not overtake a mutation that is paused before registration
TIMEOUT_EXCEEDED
renamed after the mutation:	100
--- the fence still waits for an earlier registered mutation when the highest pending one fails
renamed after N:	100
mutations:	1	1
```

Note on the second arm: on this base `StorageAlias::alter` already takes the target's alter lock, so the alias barrier blocks on the lock held by the paused P rather than on the fence; the arm still pins the fence behaviour once P fails and releases the lock (the barrier then proceeds into the fence logic with N registered and unfinished). If the arm cannot distinguish the two mechanisms, keep it as the regression pin and say so in the test comment.

- [ ] **Step 2: Run it and record the failure**

Expected before the change: the first arm passes (Task 2's lock), the second arm fails: after P fails, the barrier picks `rbegin` = N and waits for it, so it may pass too. If both arms pass before the fence change, the fence is defence in depth on this base (the alias lock fix upstream made the direct path sufficient); record that in the commit message and keep the test.

- [ ] **Step 3: Implement the fence**

Replace the barrier branch in `alter`:

```cpp
        if (!maybe_mutation_commands.empty() && maybe_mutation_commands.containBarrierCommand())
        {
            int64_t prev_mutation = 0;
            {
                std::lock_guard lock(currently_processing_in_background_mutex);
                auto it = current_mutations_by_version.rbegin();
                if (it != current_mutations_by_version.rend())
                    prev_mutation = it->first;
            }

            /// Always wait previous mutations synchronously, because alters
            /// should be executed in sequential order.
            if (prev_mutation != 0)
            {
                LOG_DEBUG(log, "Cannot change metadata with barrier alter query, will wait for mutation {}", prev_mutation);
                wait_for_mutation_unguarded(prev_mutation);
                LOG_DEBUG(log, "Mutation {} finished", prev_mutation);
            }
        }
```

with

```cpp
        if (!maybe_mutation_commands.empty() && maybe_mutation_commands.containBarrierCommand())
        {
            /// Alters are executed in sequential order with mutations. A mutation that has a block
            /// number but is not registered yet counts too: capture a fence over both, wait until no
            /// pending mutation at or below it remains (it registers, fails, or rolls back), then wait
            /// for the highest registered mutation at or below it. A mutation allocated after the
            /// fence has a higher version and is not waited for.
            int64_t prev_mutation = 0;
            {
                std::unique_lock lock(currently_processing_in_background_mutex);
                int64_t fence = 0;
                if (auto it = current_mutations_by_version.rbegin(); it != current_mutations_by_version.rend())
                    fence = it->first;
                if (auto pending = pending_versions.highestOfKind(PendingVersionKind::Mutation))
                    fence = std::max<int64_t>(fence, *pending);

                if (fence != 0)
                {
                    currently_processing_in_background_condition.wait(lock, [&]
                    {
                        return !pending_versions.anyOfKindAtOrBelow(PendingVersionKind::Mutation, fence);
                    });
                    auto it = current_mutations_by_version.upper_bound(fence);
                    if (it != current_mutations_by_version.begin())
                        prev_mutation = std::prev(it)->first;
                }
            }

            if (prev_mutation != 0)
            {
                LOG_DEBUG(log, "Cannot change metadata with barrier alter query, will wait for mutation {}", prev_mutation);
                wait_for_mutation_unguarded(prev_mutation);
                LOG_DEBUG(log, "Mutation {} finished", prev_mutation);
            }
        }
```

`currently_processing_in_background_condition` is notified by `PendingVersionGuard::releaseLocked` (registration, failure, rollback erase) and by the existing merge/mutate completion paths, so the wait returns when the pending set changes.

- [ ] **Step 4: Rebuild, restart, run 05291-05293 and 05227, commit**

```bash
ninja -C build clickhouse > build/build_task6.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task6.log; tail -1 build/build_task6.log
# restart the server
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 1200 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order 05292_mutation_waits_for_pending_versions 05293_barrier_alter_waits_for_pending_mutation 05227_mutation_waits_for_lightweight_update > build/test_task6.log 2>&1; echo "TEST_EXIT=$?"
git add src/Storages/StorageMergeTree.cpp tests/queries/0_stateless/05293_barrier_alter_waits_for_pending_mutation.sh tests/queries/0_stateless/05293_barrier_alter_waits_for_pending_mutation.reference
git commit -s -m "Make a barrier ALTER wait through pending mutations

The barrier picked the highest registered mutation and waited for it;
a mutation that had its block number but was not registered yet was
invisible to it. Capture a fence over registered and pending mutations,
wait until no pending mutation at or below the fence remains, then wait
for the highest registered one at or below it. waitForMutation itself
stays map-only: it is never called for a still-pending version."
```

---

### Task 7: Merges do not cross a pending version; `OPTIMIZE FINAL` rebuilds its predicate after a wait (S3, S6)

> **Execution ruling (2026-09-28):** the second arm of 05294 as written below expected `CANNOT_ASSIGN_OPTIMIZE` while an UPDATE is pending. That expectation was wrong: both source parts lie below the pending block, the boundary rule does not separate them, and `min_update_block` only excludes parts at or above it, so the merge is legitimate and the patch (keyed by the block number/offset columns) still applies to the merged part. The arm was re-pinned as "a merge of parts below a pending update keeps the update applicable" (0 updated rows before the update is released, 2000 after, one regular part). See the SDD ledger.

**Files:**
- Modify: `src/Storages/StorageMergeTree.cpp` (`selectPartsToMerge`: predicate/collector construction per attempt)
- Create: `tests/queries/0_stateless/05294_merge_does_not_cross_pending_version.sh`, `.reference`

**Interfaces:**
- Consumes: `getCurrentVersionBoundary` (Task 4) already refuses merges across pending versions through `canMergeParts`; this task pins it and fixes the stale predicate.

- [ ] **Step 1: Write the failing test**

```bash
./tests/queries/0_stateless/add-test merge_does_not_cross_pending_version.sh
```

```bash
#!/usr/bin/env bash
# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
# no-parallel -- server-wide failpoints pause the next mutation registration, the next lightweight update and merge selection.
# no-replicated-database -- the local timing of DELETE, UPDATE and OPTIMIZE is assumed.
# no-shared-merge-tree -- the merge selection under test is StorageMergeTree's.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

MUTATION_FP="mt_pause_once_before_register_mutation"
UPDATE_FP="mt_lightweight_update_pause_after_block_allocation"
MERGE_SELECT_FP="mt_merge_selecting_task_pause_when_scheduled"
TABLE="t_merge_pending"
MUTATIONS="FROM system.mutations WHERE database = currentDatabase() AND table = '$TABLE'"

function cleanup()
{
    for fp in $MUTATION_FP $UPDATE_FP $MERGE_SELECT_FP; do
        $CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $fp" 2>/dev/null
    done
    wait
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS $TABLE SYNC" 2>/dev/null
}
trap cleanup EXIT

function wait_for_query_result()
{
    local query=$1
    local expected=$2
    local result=""
    for _ in $(seq 1 600); do
        result=$($CLICKHOUSE_CLIENT --query "$query")
        if [[ "$result" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for '$expected' from: $query (last result: '$result')"
    exit 1
}

function wait_failpoint()
{
    # shellcheck disable=SC2086
    if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $1 PAUSE"; then
        echo "failpoint $1 was not reached"
        exit 1
    fi
}

echo "--- a merge does not cross a mutation that is paused before registration"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE IF EXISTS $TABLE SYNC;
    CREATE TABLE $TABLE (id UInt64) ENGINE = MergeTree ORDER BY id;
    SYSTEM STOP MERGES $TABLE;
    INSERT INTO $TABLE VALUES (0);
"
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $MUTATION_FP"
$CLICKHOUSE_CLIENT --lightweight_deletes_sync=0 --lightweight_delete_mode=alter_update --query "DELETE FROM $TABLE WHERE id = 1" &
paused_pid=$!
wait_failpoint $MUTATION_FP
# This row's block number is above the pending mutation: the mutation must not apply to it.
$CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE VALUES (1)"
$CLICKHOUSE_CLIENT --query "SYSTEM START MERGES $TABLE"
# OPTIMIZE must refuse: the two parts have a pending version between their data versions.
$CLICKHOUSE_CLIENT --query "OPTIMIZE TABLE $TABLE FINAL SETTINGS optimize_throw_if_noop = 1" 2>&1 | grep -m1 -o "CANNOT_ASSIGN_OPTIMIZE" || echo "the merge crossed the pending mutation"
$CLICKHOUSE_CLIENT --query "SELECT 'active parts:', count() FROM system.parts WHERE database = currentDatabase() AND table = '$TABLE' AND active"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $MUTATION_FP"
wait "$paused_pid"
wait_for_query_result "SELECT countIf(is_done) $MUTATIONS" "1"
$CLICKHOUSE_CLIENT --query "SELECT 'rows:', groupArray(id) FROM (SELECT id FROM $TABLE ORDER BY id)"

echo "--- OPTIMIZE FINAL that waited for a running merge sees the patch published meanwhile"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE $TABLE SYNC;
    CREATE TABLE $TABLE (id UInt64, v UInt64) ENGINE = MergeTree ORDER BY id
    SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1, min_bytes_for_wide_part = 0;
    INSERT INTO $TABLE SELECT number, 1 FROM numbers(1000);
    INSERT INTO $TABLE SELECT number + 1000, 1 FROM numbers(1000);
"
# The stateless harness cannot hold a merge 'running' while OPTIMIZE FINAL waits, so this arm pins the
# weaker property: with the update pending, OPTIMIZE FINAL must refuse rather than merge without the patch,
# and once the patch is committed the merge must carry it.
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $UPDATE_FP"
$CLICKHOUSE_CLIENT --enable_lightweight_update 1 --query "UPDATE $TABLE SET v = 2 WHERE 1" &
update_pid=$!
wait_failpoint $UPDATE_FP
$CLICKHOUSE_CLIENT --query "OPTIMIZE TABLE $TABLE FINAL SETTINGS optimize_throw_if_noop = 1" 2>&1 | grep -m1 -o "CANNOT_ASSIGN_OPTIMIZE" || echo "OPTIMIZE merged across the pending update"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $UPDATE_FP"
wait "$update_pid"
$CLICKHOUSE_CLIENT --query "OPTIMIZE TABLE $TABLE FINAL"
$CLICKHOUSE_CLIENT --query "SELECT 'updated rows:', count() FROM $TABLE WHERE v = 2; SELECT 'active parts:', count() FROM system.parts WHERE database = currentDatabase() AND table = '$TABLE' AND active"
```

`.reference`:

```
--- a merge does not cross a mutation that is paused before registration
CANNOT_ASSIGN_OPTIMIZE
active parts:	2
rows:	[0,1]
--- OPTIMIZE FINAL that waited for a running merge sees the patch published meanwhile
CANNOT_ASSIGN_OPTIMIZE
updated rows:	2000
active parts:	1
```

- [ ] **Step 2: Run and record**

Expected before Task 4's accessor change: the first arm would fail (merge happens, `rows: [0]`); on this branch Task 4 is already in, so the first arm passes and pins rule 2. The second arm pins rule 3 through `min_update_block` and the boundary; it is expected to pass. The stale-predicate defect needs a running merge during the wait, which the stateless harness cannot arrange deterministically without a merge-pause failpoint that this base lacks; the fix below is therefore reviewed by reading, and the arm above remains the integration pin.

- [ ] **Step 3: Rebuild the predicate and collector on every `OPTIMIZE FINAL` attempt**

In `selectPartsToMerge`, move the two constructions inside the attempt: keep

```cpp
    auto merge_predicate = std::make_shared<MergeTreeMergePredicate>(*this, txn, lock);
    auto parts_collector = std::make_shared<MergeTreePartsCollector>(*this, txn, merge_predicate);
```

but make them reassignable per attempt: in the `select_in_partition` lambda's retry loop, right after `continue;` would re-enter the loop, i.e. at the top of the `while (true)` body (before `merger_mutator.selectAllPartsToMergeWithinPartition(...)`), add

```cpp
            /// The predicate captured the committing blocks and the patch parts under the mutex
            /// acquisition it was constructed in. After a wait that released the mutex, that state
            /// is stale: a lightweight update may have published its patch and stopped being
            /// pending. Rebuild both before reading the parts again.
            if (attempt > 0)
            {
                merge_predicate = std::make_shared<MergeTreeMergePredicate>(*this, txn, lock);
                parts_collector = std::make_shared<MergeTreePartsCollector>(*this, txn, merge_predicate);
            }
            ++attempt;
```

with `size_t attempt = 0;` declared before the loop, and declare `merge_predicate`/`parts_collector` as non-const `std::shared_ptr` so they can be reassigned (they already are `auto` locals; make sure the lambdas capture them by reference, `[&]`, which they do).

- [ ] **Step 4: Rebuild, restart, run 05291-05294 and 05227, commit**

```bash
ninja -C build clickhouse > build/build_task7.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_task7.log; tail -1 build/build_task7.log
# restart the server
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 1500 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05291_mutations_register_in_order 05292_mutation_waits_for_pending_versions 05293_barrier_alter_waits_for_pending_mutation 05294_merge_does_not_cross_pending_version 05227_mutation_waits_for_lightweight_update > build/test_task7.log 2>&1; echo "TEST_EXIT=$?"
git add src/Storages/StorageMergeTree.cpp tests/queries/0_stateless/05294_merge_does_not_cross_pending_version.sh tests/queries/0_stateless/05294_merge_does_not_cross_pending_version.reference
git commit -s -m "Refuse merges across a pending version and rebuild the OPTIMIZE FINAL predicate after a wait

A part below a mutation that is not registered yet and a part inserted
above it had the same current mutation version, so they could merge; the
merged part kept the lower data version and the mutation later applied
to rows inserted after it. The version boundaries now count pending
versions, so canMergeParts refuses such a merge. OPTIMIZE FINAL waits for
running merges with the selection mutex released and reused a predicate
that captured the committing blocks and patch parts before the wait;
it is rebuilt on every retry."
```

---

### Task 8: Failure cleanup and kill interplay (S7)

**Files:**
- Create: `tests/queries/0_stateless/05295_pending_version_failure_cleanup.sh`, `.reference`

**Interfaces:**
- Consumes: failpoints `mt_throw_after_mutation_commit` (existing, `ONCE`), `mt_pause_once_before_register_mutation`, `mt_lightweight_update_pause_after_block_allocation`.

- [ ] **Step 1: Write the test**

```bash
./tests/queries/0_stateless/add-test pending_version_failure_cleanup.sh
```

```bash
#!/usr/bin/env bash
# Tags: no-parallel, no-replicated-database, no-shared-merge-tree
# no-parallel -- server-wide failpoints affect the next mutation and the next lightweight update on any table.
# no-replicated-database -- the local timing of DELETE, UPDATE and OPTIMIZE is assumed.
# no-shared-merge-tree -- the bookkeeping under test is StorageMergeTree's.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

MUTATION_FP="mt_pause_once_before_register_mutation"
THROW_FP="mt_throw_after_mutation_commit"
UPDATE_FP="mt_lightweight_update_pause_after_block_allocation"
TABLE="t_pending_cleanup"
MUTATIONS="FROM system.mutations WHERE database = currentDatabase() AND table = '$TABLE'"

function cleanup()
{
    for fp in $MUTATION_FP $THROW_FP $UPDATE_FP; do
        $CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $fp" 2>/dev/null
    done
    wait
    $CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS $TABLE SYNC" 2>/dev/null
}
trap cleanup EXIT

function wait_for_query_result()
{
    local query=$1
    local expected=$2
    local result=""
    for _ in $(seq 1 600); do
        result=$($CLICKHOUSE_CLIENT --query "$query")
        if [[ "$result" == "$expected" ]]; then
            return 0
        fi
        sleep 0.1
    done
    echo "Timed out waiting for '$expected' from: $query (last result: '$result')"
    exit 1
}

function wait_failpoint()
{
    # shellcheck disable=SC2086
    if ! timeout 60 $CLICKHOUSE_CLIENT --query "SYSTEM WAIT FAILPOINT $1 PAUSE"; then
        echo "failpoint $1 was not reached"
        exit 1
    fi
}

echo "--- a mutation that fails after its file is written stops being a boundary"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE IF EXISTS $TABLE SYNC;
    CREATE TABLE $TABLE (id UInt64) ENGINE = MergeTree ORDER BY id;
    SYSTEM STOP MERGES $TABLE;
    INSERT INTO $TABLE VALUES (0);
"
# The throw fires after the file commit, i.e. after allocation and announcement.
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $THROW_FP"
$CLICKHOUSE_CLIENT --lightweight_deletes_sync=0 --lightweight_delete_mode=alter_update --query "DELETE FROM $TABLE WHERE id = 1" 2>&1 | grep -m1 -o "FAULT_INJECTED"
$CLICKHOUSE_CLIENT --query "INSERT INTO $TABLE VALUES (1)"
$CLICKHOUSE_CLIENT --query "SYSTEM START MERGES $TABLE"
# No pending version remains: the two parts merge.
$CLICKHOUSE_CLIENT --query "OPTIMIZE TABLE $TABLE FINAL SETTINGS optimize_throw_if_noop = 1"
$CLICKHOUSE_CLIENT --query "SELECT 'active parts:', count() FROM system.parts WHERE database = currentDatabase() AND table = '$TABLE' AND active; SELECT 'mutations:', count() $MUTATIONS"

echo "--- KILL MUTATION of a registered mutation leaves a later pending one intact"
$CLICKHOUSE_CLIENT --query "
    DROP TABLE $TABLE SYNC;
    CREATE TABLE $TABLE (id UInt64, v UInt64) ENGINE = MergeTree ORDER BY id;
    INSERT INTO $TABLE SELECT number, 0 FROM numbers(100);
    SYSTEM STOP MERGES $TABLE;
"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE v = 1 WHERE 1 SETTINGS mutations_sync = 0"
wait_for_query_result "SELECT count() $MUTATIONS" "1"
$CLICKHOUSE_CLIENT --query "SYSTEM ENABLE FAILPOINT $MUTATION_FP"
$CLICKHOUSE_CLIENT --query "ALTER TABLE $TABLE UPDATE v = 2 WHERE 1 SETTINGS mutations_sync = 0" &
paused_pid=$!
wait_failpoint $MUTATION_FP
first=$($CLICKHOUSE_CLIENT --query "SELECT mutation_id $MUTATIONS ORDER BY mutation_id LIMIT 1")
$CLICKHOUSE_CLIENT --query "KILL MUTATION WHERE database = currentDatabase() AND table = '$TABLE' AND mutation_id = '$first' SYNC FORMAT Null"
$CLICKHOUSE_CLIENT --query "SYSTEM DISABLE FAILPOINT $MUTATION_FP"
wait "$paused_pid"
$CLICKHOUSE_CLIENT --query "SYSTEM START MERGES $TABLE"
wait_for_query_result "SELECT count(), countIf(is_done) $MUTATIONS" "1	1"
$CLICKHOUSE_CLIENT --query "SELECT 'v after the second mutation:', v, count() FROM $TABLE GROUP BY v ORDER BY v"
```

`.reference`:

```
--- a mutation that fails after its file is written stops being a boundary
FAULT_INJECTED
active parts:	1
mutations:	0
--- KILL MUTATION of a registered mutation leaves a later pending one intact
v after the second mutation:	2	100
```

- [ ] **Step 2: Rebuild is not needed; run, then commit**

```bash
# server running the Task 7 binary
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 900 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05295_pending_version_failure_cleanup > build/test_task8.log 2>&1; echo "TEST_EXIT=$?"
git add tests/queries/0_stateless/05295_pending_version_failure_cleanup.sh tests/queries/0_stateless/05295_pending_version_failure_cleanup.reference
git commit -s -m "Test that a failed mutation stops being a pending version and that KILL MUTATION ignores pending ones"
```

If the first arm fails because the guard leaked (`CANNOT_ASSIGN_OPTIMIZE` printed), the destructor path of `PendingVersionGuard` in `prepareMutationEntry`'s unwinding is wrong: check the local declaration order in Task 4 Step 4 (`allocated` must be declared before `entry`).

---

### Task 9: Whole-branch checks and the pull request text

**Files:**
- Read: all commits of the branch; `tests/queries/0_stateless/05227_*`, `05291_*` to `05295_*`; `src/Storages/StorageMergeTree.{h,cpp}`
- Create: `~/workspace/ClickHouse/mt-pending-versions/build/pr_body.md` (not committed)

- [ ] **Step 1: Style and grep gates**

```bash
git grep -n -i "cas\b\|content-addressed\|altinity" -- src/Storages/StorageMergeTree.cpp src/Storages/StorageMergeTree.h src/Storages/MergeTree/PendingVersions.h src/Storages/MergeTree/tests/gtest_pending_versions.cpp tests/queries/0_stateless/0529[1-5]_* ; echo "must print nothing"
git log --format=%B upstream/master..HEAD | grep -i -c "cas\b\|content-addressed\|altinity\|Co-Authored\|Claude" ; echo "must be 0"
python3 utils/check-style/check-style 2>/dev/null | head; ./utils/check-style/check-doc-aspell 2>/dev/null | head -0
```

- [ ] **Step 2: Run the wider mutation and merge suites**

```bash
CLICKHOUSE_PORT_TCP=19681 CLICKHOUSE_PORT_HTTP=19682 timeout 3600 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful mutation lightweight_update lightweight_delete optimize_final patch_part > build/test_task9_suites.log 2>&1; echo "TEST_EXIT=$?"
grep -E "^\[|passed|FAIL" build/test_task9_suites.log | tail -5
```

Use a subagent to read the log and report only failed tests with their first error line. Environmental failures (Keeper, `/var/lib/clickhouse` paths) are listed, not fixed; any failure inside `StorageMergeTree` mutation or merge selection is a defect of this branch and blocks the PR.

- [ ] **Step 3: Unit tests under ASan (if a sanitizer build directory is available) and the gtest**

```bash
build/src/unit_tests_dbms --gtest_filter='PendingVersions*' 2>&1 | tail -1
```

- [ ] **Step 4: Write the PR body (not committed) from `.github/PULL_REQUEST_TEMPLATE.md`**

Content, in this order: three sentences on the three races and the one mechanism; "Built on top of #119155 (its two commits kept), supersedes it: the committing-block scan guarded by `supportsLightweightUpdate()` is replaced by the pending version; its reason string and test are kept."; Changelog category `Bug Fix (user-visible misbehavior in an official stable release)`; Changelog entry: "Fixed lost mutations on plain `MergeTree` when two mutations register out of order, when a mutation runs while a lightweight `UPDATE` is uncommitted, and when a merge spans a mutation that is not registered yet; `OPTIMIZE FINAL` no longer reuses a stale merge predicate after waiting for running merges."; `Closes: https://github.com/ClickHouse/ClickHouse/issues/122083`, `Closes: https://github.com/ClickHouse/ClickHouse/issues/116686`, `Related: https://github.com/ClickHouse/ClickHouse/pull/119155`; the Documentation checkbox unchecked. No "Generated with" line, no session link.

- [ ] **Step 5: Report**

Final message: branch name and HEAD, commit list, test results per test, the grep gates, the PR body path, and an explicit list of what was not verified (the stale-predicate arm of S6, `04516` if it could not run standalone).
