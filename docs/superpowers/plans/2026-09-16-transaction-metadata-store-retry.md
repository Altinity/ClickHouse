# Transaction Metadata Store Retry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A transient storage error in a metadata write under the `noexcept` commit/rollback callbacks of MergeTree transactions is retried in place instead of terminating the server.

**Architecture:** One helper `retryMetadataStore` in `MergeTreeTransaction.cpp` wraps the six writes performed by `MergeTreeTransaction::afterCommit` and `MergeTreeTransaction::rollback`; it retries any error except `LOGICAL_ERROR`/`NOT_IMPLEMENTED` for at most 60 s per object, then rethrows (today's behaviour). `MergeTreeMutationEntry::writeCSN` rewrites the whole mutation record through a temporary file so a retry is idempotent. `StorageMergeTree::setMutationCSN` tolerates a mutation killed during the commit. Two REGULAR failpoints and one stateless test cover it, failing first.

**Tech Stack:** C++ (ClickHouse `src/`), `FailPoint` macros, bash stateless test using `transactions.lib`.

**Spec:** `docs/superpowers/specs/2026-09-16-transaction-metadata-store-best-effort-design.md` (revision 3d, approved 2026-09-16; in the `master` worktree, branch `cas-gc-rebuild`, commit `43cbd78aa8ae`).

## Global Constraints

- Working tree: `/home/mfilimonov/workspace/ClickHouse/lane-g` (branch `fix/antalya-26.6/transaction-metadata-store-retry` checked out there, based on `altinity/antalya-26.6` at `f1abcf435a83`). Never `cd` into another worktree, never switch branches, never create a new build directory: the existing `build/` (release) and `build_asan/` (ASan) are the only build directories. Never push.
- Portability: the change must cherry-pick to upstream `ClickHouse/ClickHouse` unchanged. **No mention of CAS, content-addressed storage, Altinity, Antalya, a fork, a ticket or a plan anywhere**: not in code, comments, test names, test comments, log messages or commit messages. Grep the diff for `-i "cas\b\|content.addressed\|altinity\|antalya"` before every commit.
- The test must run on a plain local disk without any special storage.
- Comments: short, plain English, say the reason (why a write is retried, why invariant errors are not, why the record is rewritten). Function names in prose as `f`, not `f()`.
- C++ style: Allman braces; no `sleep` to fix races (the backoff sleep between retry attempts is a retry policy, not a race fix). `LOGICAL_ERROR` aborts under debug/sanitizer builds: never write a test that expects one.
- Failing-first order (spec §4): commit 1 = failpoints + test (Task 1); commit 2 = every production change (`writeCSN` rewrite from Task 2 together with the helper, call sites and `setMutationCSN` from Task 3), made at the end of Task 3. Task 2 builds and tests but does not commit. Each commit builds.
- Commits: `git diff --cached --stat` must be empty before `git add`; commit with `git commit -s -m "<msg>" -- <paths>` (paths after the message). Message ends with the two attribution lines given by the session (`Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01GhVd7eMAWdFubNk4g1B2Tx`).
- Builds: `flock build/.build.lock ninja -C build clickhouse > build/build_<task>.log 2>&1` (the lock serializes builds with other sessions sharing this tree), never `-j`, never `nproc`. Test runs redirect to `build/test_<name>.log`. The controller builds the base tree first (marker `build/BUILD_DONE`, exit code in the last line of `build/build_txn_base.log`); wait for that marker before your first build. Untracked files and directories in the tree (test leftovers, `tmp/`, jeprof files) are not yours: never `git add` anything but the files named in your task.
- Exact constants: retry budget `60` seconds; backoff `100` ms doubling to a cap of `2000` ms; failpoint names `transaction_metadata_store_fail` and `transaction_mutation_csn_store_fail`; temporary mutation file `tmp_mutation_csn_<block_number>.txt`; log messages exactly as written in Task 3 (the test greps them).

---

## File structure

| File | Responsibility in this change |
|---|---|
| `src/Common/FailPoint.cpp` | registers the two REGULAR failpoints |
| `src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp` | failpoint at the top of `storeInfoToDataPartStorage` (no other change) |
| `src/Storages/MergeTree/MergeTreeMutationEntry.{h,cpp}` | failpoint in `writeCSN`; `writeRecord` shared by the constructor and `writeCSN`; `writeCSN` rewrites the file through `tmp_mutation_csn_<N>.txt` |
| `src/Interpreters/MergeTreeTransaction.cpp` | `retryMetadataStore` helper and its six call sites |
| `src/Storages/StorageMergeTree.cpp` | `setMutationCSN`: killed mutation is a warning, not `LOGICAL_ERROR` |
| `tests/queries/0_stateless/05053_transaction_metadata_store_retry.{sh,reference}` | the failing-first test, three scenarios |

## How to run the test locally

The stateless runner needs a server with an embedded Keeper (the transaction log lives in Keeper) and the
transaction settings of the CI test config. `programs/server/config.d/` in this tree already holds **symlinks** to
the CI test configs (`keeper_port.xml`, `zookeeper.xml`, `transactions.xml`, ...), so a server started from
`programs/server/config.xml` has Keeper and transactions; `transactions_info_log.xml` is **not** linked, so that log
is enabled on the command line (the parent key `--transactions_info_log=1` is required, the log factory checks the
section's presence). Only ports, paths and this log are overridden on the command line. Never write into `programs/server/config.d/` or `users.d/`: those files are the CI configs themselves.
`$D` is a scratch dir under `build/`:

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
D=$PWD/build/srv; mkdir -p $D/data $D/tmp $D/uf $D/caches
# Users config: a copy of the server's users.xml plus the CI override that disables async inserts
# (this tree defaults async_insert to 1; async inserts are refused inside transactions). User/profile
# settings are loaded by AccessControl from the users file, so `--profiles.*` command-line overrides do
# not reach them; the users file is pointed at with the `users_config` key instead. Nothing is written
# into programs/server/.
mkdir -p $D/users/users.d
cp programs/server/users.xml $D/users/users.xml
for f in programs/server/users.d/*; do ln -sf "$(readlink -f "$f")" "$D/users/users.d/$(basename "$f")"; done
ln -sf $PWD/tests/config/users.d/disable_async_inserts.xml $D/users/users.d/disable_async_inserts.xml
nohup build/programs/clickhouse server --config-file=programs/server/config.xml -- \
  --path=$D/data/ --tmp_path=$D/tmp/ --user_files_path=$D/uf/ --logger.log=$D/server.log --logger.errorlog=$D/server.err.log \
  --tcp_port=19481 --http_port=19482 --interserver_http_port=19483 --mysql_port=0 --postgresql_port=0 --prometheus.port=0 \
  --keeper_server.tcp_port=19484 --keeper_server.raft_configuration.server.port=19485 --zookeeper.node.port=19484 \
  --transactions_info_log=1 --transactions_info_log.database=system --transactions_info_log.table=transactions_info_log --transactions_info_log.flush_interval_milliseconds=7500 \
  --users_config=$D/users/users.xml \
  --filesystem_caches_path=$D/caches/ --custom_cached_disks_base_directory=$D/caches/ \
  > $D/server.out 2>&1 &
echo $! > $D/server.pid
sleep 8
build/programs/clickhouse client --port 19481 -q "CREATE DATABASE IF NOT EXISTS test"
build/programs/clickhouse client --port 19481 -q "SELECT count() > 0 FROM system.zookeeper WHERE path = '/'"   # 1: Keeper reachable
build/programs/clickhouse client --port 19481 -q "BEGIN TRANSACTION; ROLLBACK"   # no error: transactions enabled
build/programs/clickhouse client --port 19481 -q "SYSTEM FLUSH LOGS transactions_info_log; SELECT count() >= 0 FROM system.transactions_info_log"   # 1: the log table exists
build/programs/clickhouse client --port 19481 -q "SELECT value FROM system.settings WHERE name = 'async_insert'"   # 0: this tree defaults async_insert to 1, CI disables it via users.d/disable_async_inserts.xml, which is not linked locally; async inserts are refused inside transactions
```

(Keeper log and snapshot storage default to directories under `--path`, so nothing else needs a path.) If the
server does not start or one of the four checks fails, read `$D/server.err.log` and `$D/server.out`; if it is not a
trivial port clash you can resolve by picking other free ports (report the ports you used), report `NEEDS_CONTEXT`
with the exact error instead of improvising another setup.

Run one test:

```bash
CLICKHOUSE_PORT_TCP=19481 CLICKHOUSE_PORT_HTTP=19482 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05053_transaction_metadata_store_retry > build/test_05053.log 2>&1; tail -20 build/test_05053.log
```

Stop the server with `kill $(cat $D/server.pid)` (pid only, never a pattern).

---

### Task 1: Failpoints and the failing test

**Files:**
- Modify: `src/Common/FailPoint.cpp` (the `APPLY_FOR_FAILPOINTS` macro list, near the other `REGULAR(...)` entries)
- Modify: `src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:323` (`storeInfoToDataPartStorage`)
- Modify: `src/Storages/MergeTree/MergeTreeMutationEntry.cpp:111` (`writeCSN`)
- Create: `tests/queries/0_stateless/05053_transaction_metadata_store_retry.sh`
- Create: `tests/queries/0_stateless/05053_transaction_metadata_store_retry.reference`

**Interfaces:**
- Produces: failpoint names `transaction_metadata_store_fail`, `transaction_mutation_csn_store_fail` (used by the test and referenced by Task 3's comments); the log-message contract the test greps (Task 3 must emit exactly these):
  - `Cannot store transaction metadata for {what}, will retry: {error}` (Warning, first failure per object)
  - `Stored transaction metadata for {what} after {n} attempts` (Information, success after ≥1 failure)
  - `{what}` is `part {part_name} of {db}.{table}` or `mutation {file_name} of {db}.{table}`.

- [ ] **Step 1: Register the failpoints**

In `src/Common/FailPoint.cpp`, add to the macro list, after `REGULAR(hybrid_watermarks_read_fail) \`:

```cpp
    REGULAR(transaction_metadata_store_fail) \
    REGULAR(transaction_mutation_csn_store_fail) \
```

- [ ] **Step 2: Failpoint in the part metadata store**

In `src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp`, next to the existing `namespace ErrorCodes` block add:

```cpp
namespace FailPoints
{
    extern const char transaction_metadata_store_fail[];
}
```

(`#include <Common/FailPoint.h>` if the file does not include it yet; add `extern const int FAULT_INJECTED;` to `namespace ErrorCodes`.) Then make the first statements of `storeInfoToDataPartStorage` (line 323, before `static constexpr auto filename`):

```cpp
    /// Fault injection for tests: fail before any I/O, so the old file stays intact.
    fiu_do_on(FailPoints::transaction_metadata_store_fail,
    {
        throw Exception(ErrorCodes::FAULT_INJECTED, "Injected failure while storing version metadata");
    });
```

- [ ] **Step 3: Failpoint in the mutation CSN write**

In `src/Storages/MergeTree/MergeTreeMutationEntry.cpp`, add `#include <Common/FailPoint.h>`, `extern const int FAULT_INJECTED;` in `namespace ErrorCodes`, and:

```cpp
namespace FailPoints
{
    extern const char transaction_mutation_csn_store_fail[];
}
```

and change `writeCSN` to:

```cpp
void MergeTreeMutationEntry::writeCSN(CSN csn_)
{
    csn = csn_;
    /// Fault injection for tests: fail before any I/O, so the old file stays intact.
    fiu_do_on(FailPoints::transaction_mutation_csn_store_fail,
    {
        throw Exception(ErrorCodes::FAULT_INJECTED, "Injected failure while storing mutation CSN");
    });
    auto out = disk->writeFile(path_prefix + file_name, 256, WriteMode::Append);
    *out << "csn: " << csn << "\n";
    out->finalize();
}
```

- [ ] **Step 4: Write the test**

Create `tests/queries/0_stateless/05053_transaction_metadata_store_retry.sh` (make it executable, `chmod +x`):

```bash
#!/usr/bin/env bash
# Tags: no-ordinary-database, no-encrypted-storage, no-object-storage, no-parallel
# Tag rationale: enables server-wide failpoints; reads raw metadata files from the data directory.
#
# A metadata write that fails inside the noexcept commit/rollback callbacks of a
# transaction must be retried instead of terminating the server. Each scenario makes
# the first write of the callback fail with a failpoint, waits until the server has
# logged the first retry, disables the failpoint, and checks that the statement
# completed and the metadata is really on disk.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh
# shellcheck source=./transactions.lib
. "$CUR_DIR"/transactions.lib

function cleanup()
{
    $CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT transaction_metadata_store_fail" ||:
    $CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT transaction_mutation_csn_store_fail" ||:
}
trap cleanup EXIT

# Poll text_log until the first "will retry" line for the given object kind and table exists.
function wait_for_retry()
{
    local kind=$1
    local table=$2
    for _ in $(seq 1 600); do
        $CLICKHOUSE_CLIENT -q "SYSTEM FLUSH LOGS text_log"
        local n
        n=$($CLICKHOUSE_CLIENT -q "SELECT count() FROM system.text_log WHERE event_date >= yesterday() AND message LIKE 'Cannot store transaction metadata for ${kind} % of ${CLICKHOUSE_DATABASE}.${table} (%), will retry%'")
        [ "$n" -ge 1 ] && return 0
        sleep 0.1
    done
    echo "timeout waiting for the retry of ${kind} in ${table}"
    return 1
}

# Prints how many objects were retried, how many were reported stored after a retry,
# and whether the two lines name the same object. The table description in the messages
# is `db.table (uuid)`, as printed by StorageID::getNameForLogs for Atomic databases.
function report_retry_lines()
{
    local kind=$1
    local table=$2
    $CLICKHOUSE_CLIENT -q "SYSTEM FLUSH LOGS text_log"
    $CLICKHOUSE_CLIENT -q "
        WITH
            (SELECT groupUniqArray(extract(message, 'for (${kind} .+ \\\\(.+\\\\)), will retry')) FROM system.text_log
                WHERE event_date >= yesterday() AND message LIKE 'Cannot store transaction metadata for ${kind} % of ${CLICKHOUSE_DATABASE}.${table} (%), will retry%') AS retried,
            (SELECT groupArray(extract(message, 'for (${kind} .+ \\\\(.+\\\\)) after')) FROM system.text_log
                WHERE event_date >= yesterday() AND message LIKE 'Stored transaction metadata for ${kind} % of ${CLICKHOUSE_DATABASE}.${table} (%) after % attempts') AS stored
        SELECT 'retried objects', length(retried), 'stored after retry', length(stored), 'same object', retried = stored
        FORMAT TSV"
}

function commit_csn()
{
    local tid=$1
    $CLICKHOUSE_CLIENT -q "SYSTEM FLUSH LOGS transactions_info_log"
    $CLICKHOUSE_CLIENT -q "SELECT csn FROM system.transactions_info_log WHERE type = 'Commit' AND tid = ${tid} ORDER BY event_time DESC LIMIT 1"
}

# ---------------------------------------------------------------------------
echo "--- A: commit, part metadata"
$CLICKHOUSE_CLIENT -q "
    DROP TABLE IF EXISTS t_meta_retry;
    CREATE TABLE t_meta_retry (k Int64) ENGINE = MergeTree ORDER BY k SETTINGS old_parts_lifetime = 3600;
    SYSTEM STOP MERGES t_meta_retry;
    INSERT INTO t_meta_retry VALUES (1);
    INSERT INTO t_meta_retry VALUES (2);
"
tx_sync 1 "BEGIN TRANSACTION"
tx_sync 1 "ALTER TABLE t_meta_retry DROP PARTITION ID 'all'"
tx_sync 1 "INSERT INTO t_meta_retry VALUES (3)"
tid=$(tx 1 "SELECT transactionID()" | cut -f2)

$CLICKHOUSE_CLIENT -q "SYSTEM ENABLE FAILPOINT transaction_metadata_store_fail"
tx_async 1 "COMMIT"
wait_for_retry part t_meta_retry
$CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT transaction_metadata_store_fail"
tx_wait 1

report_retry_lines part t_meta_retry
csn=$(commit_csn "$tid")
part_path=$($CLICKHOUSE_CLIENT -q "SELECT path FROM system.parts WHERE database = currentDatabase() AND table = 't_meta_retry' AND active")
echo "creation_csn persisted: $(grep -c "^creation_csn: ${csn}$" "${part_path}txn_version.txt")"
$CLICKHOUSE_CLIENT -q "SELECT 'rows after commit', count() FROM t_meta_retry"
$CLICKHOUSE_CLIENT -q "DETACH TABLE t_meta_retry; ATTACH TABLE t_meta_retry"
$CLICKHOUSE_CLIENT -q "SELECT 'rows after reattach', count() FROM t_meta_retry"

# ---------------------------------------------------------------------------
echo "--- B: commit, mutation CSN"
$CLICKHOUSE_CLIENT -q "
    DROP TABLE IF EXISTS t_meta_retry_mut;
    CREATE TABLE t_meta_retry_mut (k Int64, v Int64) ENGINE = MergeTree ORDER BY k;
    INSERT INTO t_meta_retry_mut VALUES (1, 1), (2, 2), (3, 3);
"
tx_sync 2 "BEGIN TRANSACTION"
tx_sync 2 "ALTER TABLE t_meta_retry_mut UPDATE v = v + 1 WHERE 1"
tid=$(tx 2 "SELECT transactionID()" | cut -f2)

$CLICKHOUSE_CLIENT -q "SYSTEM ENABLE FAILPOINT transaction_mutation_csn_store_fail"
tx_async 2 "COMMIT"
wait_for_retry mutation t_meta_retry_mut
$CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT transaction_mutation_csn_store_fail"
tx_wait 2

report_retry_lines mutation t_meta_retry_mut
csn=$(commit_csn "$tid")
data_path=$($CLICKHOUSE_CLIENT -q "SELECT data_paths[1] FROM system.tables WHERE database = currentDatabase() AND name = 't_meta_retry_mut'")
mutation_id=$($CLICKHOUSE_CLIENT -q "SELECT mutation_id FROM system.mutations WHERE database = currentDatabase() AND table = 't_meta_retry_mut'")
echo "csn lines in mutation file: $(grep -c '^csn: ' "${data_path}${mutation_id}")"
echo "last line is the csn: $([ "$(tail -n 1 "${data_path}${mutation_id}")" == "csn: ${csn}" ] && echo 1 || echo 0)"
$CLICKHOUSE_CLIENT -q "SELECT 'sum after commit', sum(v) FROM t_meta_retry_mut"
$CLICKHOUSE_CLIENT -q "DETACH TABLE t_meta_retry_mut; ATTACH TABLE t_meta_retry_mut"
$CLICKHOUSE_CLIENT -q "SELECT 'sum after reattach', sum(v) FROM t_meta_retry_mut"
$CLICKHOUSE_CLIENT -q "SELECT 'mutations after reattach', count() FROM system.mutations WHERE database = currentDatabase() AND table = 't_meta_retry_mut'"

# ---------------------------------------------------------------------------
echo "--- C: rollback, part metadata"
$CLICKHOUSE_CLIENT -q "
    DROP TABLE IF EXISTS t_meta_retry_rb;
    CREATE TABLE t_meta_retry_rb (k Int64) ENGINE = MergeTree ORDER BY k SETTINGS old_parts_lifetime = 3600;
    SYSTEM STOP MERGES t_meta_retry_rb;
    INSERT INTO t_meta_retry_rb VALUES (1);
    INSERT INTO t_meta_retry_rb VALUES (2);
"
tx_sync 3 "BEGIN TRANSACTION"
tx_sync 3 "ALTER TABLE t_meta_retry_rb DROP PARTITION ID 'all'"
tx_sync 3 "INSERT INTO t_meta_retry_rb VALUES (3)"

$CLICKHOUSE_CLIENT -q "SYSTEM ENABLE FAILPOINT transaction_metadata_store_fail"
tx_async 3 "ROLLBACK"
wait_for_retry part t_meta_retry_rb
$CLICKHOUSE_CLIENT -q "SYSTEM DISABLE FAILPOINT transaction_metadata_store_fail"
tx_wait 3

report_retry_lines part t_meta_retry_rb
$CLICKHOUSE_CLIENT -q "SELECT 'rows after rollback', count() FROM t_meta_retry_rb"
$CLICKHOUSE_CLIENT -q "DETACH TABLE t_meta_retry_rb; ATTACH TABLE t_meta_retry_rb"
$CLICKHOUSE_CLIENT -q "SELECT 'rows after reattach', count() FROM t_meta_retry_rb"

$CLICKHOUSE_CLIENT -q "DROP TABLE t_meta_retry; DROP TABLE t_meta_retry_mut; DROP TABLE t_meta_retry_rb"
```

Create `tests/queries/0_stateless/05053_transaction_metadata_store_retry.reference`:

```
--- A: commit, part metadata
retried objects	1	stored after retry	1	same object	1
creation_csn persisted: 1
rows after commit	1
rows after reattach	1
--- B: commit, mutation CSN
retried objects	1	stored after retry	1	same object	1
csn lines in mutation file: 1
last line is the csn: 1
sum after commit	9
sum after reattach	9
mutations after reattach	1
--- C: rollback, part metadata
retried objects	1	stored after retry	1	same object	1
rows after rollback	2
rows after reattach	2
```

The outputs of `tx_sync` / `tx_async` are deliberately not redirected: on success these statements print nothing
over HTTP, and any error text (`Code: ...`) lands in the test output and breaks the reference, which is the
success assertion for `COMMIT` and `ROLLBACK`.

Notes for the implementer: `tx`, `tx_sync`, `tx_async`, `tx_wait` come from `transactions.lib` (HTTP sessions, one session per number); `tx` prefixes its output with `tx<N>\t`, hence `cut -f2` to get the `transactionID()` tuple as text, e.g. `(6,2,'00000000-0000-0000-0000-000000000000')`; it is pasted as a tuple literal into `tid = ${tid}`, the same way `01168_mutations_isolation.sh` pastes it into `kill transaction where tid=...`. If the test number `05053` is already taken when you start (`ls tests/queries/0_stateless/ | grep ^05053`), use `./tests/queries/0_stateless/add-test transaction_metadata_store_retry.sh` to get the next free number and rename both files consistently; report the final name.

- [ ] **Step 5: Build and run the test, expecting the failing-first outcome**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
flock build/.build.lock ninja -C build clickhouse > build/build_task1.log 2>&1; tail -3 build/build_task1.log
```

Start the server (recipe above), run the test into `build/test_05053_task1.log`. Expected on this tree: scenario A's `COMMIT` makes the server terminate (`FAULT_INJECTED` escapes `afterCommit`); the runner reports the test as failed (server died / connection refused). Record the exact failure line in the report. Restart the server afterwards if you need it.

- [ ] **Step 6: Commit**

```bash
git diff --cached --stat   # must print nothing
git add tests/queries/0_stateless/05053_transaction_metadata_store_retry.sh tests/queries/0_stateless/05053_transaction_metadata_store_retry.reference
git commit -s -m "Add failpoints and a test for transaction metadata writes under noexcept callbacks

The test enables a failpoint in the part version metadata store (or the
mutation CSN write), commits or rolls back a transaction, and expects the
server to retry the write instead of terminating. On this tree the test
fails: the injected error escapes MergeTreeTransaction::afterCommit.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GhVd7eMAWdFubNk4g1B2Tx" -- src/Common/FailPoint.cpp src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp src/Storages/MergeTree/MergeTreeMutationEntry.cpp tests/queries/0_stateless/05053_transaction_metadata_store_retry.sh tests/queries/0_stateless/05053_transaction_metadata_store_retry.reference
```

---

### Task 2: `writeCSN` rewrites the whole mutation record

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeMutationEntry.h` (add a private `writeRecord`)
- Modify: `src/Storages/MergeTree/MergeTreeMutationEntry.cpp:50-116` (constructor, `writeCSN`)

**Interfaces:**
- Consumes: failpoint `transaction_mutation_csn_store_fail` inside `writeCSN` (Task 1), kept at the top of the function.
- Produces: `void MergeTreeMutationEntry::writeCSN(CSN csn_)` with the same signature and the same on-disk record format (loader unchanged).

- [ ] **Step 1: Declare `writeRecord`**

In `MergeTreeMutationEntry.h`, inside the struct, add a private section (the struct is all-public today; add at the end):

```cpp
private:
    /// Serializes everything except the `csn:` line, in the format the loading constructor reads.
    void writeRecord(WriteBuffer & out) const;
```

Add `class WriteBuffer;` forward declaration near the top of the header if `WriteBuffer` is not already visible there.

- [ ] **Step 2: Implement `writeRecord` and use it in the constructor**

In `MergeTreeMutationEntry.cpp` replace the body of the writing constructor's `try` block so that it reads:

```cpp
    try
    {
        if (tid.isNonTransactional())
            csn = Tx::NonTransactionalCSN;

        auto out = disk->writeFile(std::filesystem::path(path_prefix) / file_name, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, settings);
        writeRecord(*out);
        out->finalize();
        out->sync();
    }
    catch (...)
    {
        removeFile();
        throw;
    }
```

and add, after `commit`:

```cpp
void MergeTreeMutationEntry::writeRecord(WriteBuffer & out) const
{
    out << "format version: 1\n"
        << "create time: " << LocalDateTime(create_time, DateLUT::serverTimezoneInstance()) << "\n";
    out << "commands: ";
    commands->writeText(out, /* with_pure_metadata_commands = */ false);
    out << "\n";
    if (!tid.isNonTransactional())
    {
        out << "tid: ";
        TransactionID::write(tid, out);
        out << "\n";
    }
}
```

- [ ] **Step 3: Rewrite `writeCSN`**

```cpp
void MergeTreeMutationEntry::writeCSN(CSN csn_)
{
    csn = csn_;
    /// Fault injection for tests: fail before any I/O, so the old file stays intact.
    fiu_do_on(FailPoints::transaction_mutation_csn_store_fail,
    {
        throw Exception(ErrorCodes::FAULT_INJECTED, "Injected failure while storing mutation CSN");
    });

    /// The whole record is rewritten through a temporary file instead of appending the
    /// `csn:` line: a write that fails half-way, or is repeated, must not leave a partial
    /// or duplicated line behind, because the loader accepts exactly one.
    /// The name must not collide with the constructor's `tmp_mutation_<N>.txt`, whose
    /// number comes from a different counter; the `tmp_mutation_` prefix keeps it covered
    /// by the startup cleanup of leftover temporary files.
    String tmp_file_name = "tmp_mutation_csn_" + toString(block_number) + ".txt";
    auto out = disk->writeFile(path_prefix + tmp_file_name, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite);
    writeRecord(*out);
    *out << "csn: " << csn << "\n";
    out->finalize();
    out->sync();
    disk->replaceFile(path_prefix + tmp_file_name, path_prefix + file_name);
}
```

- [ ] **Step 4: Build, restart the server on the new binary, run the existing transactional-mutation tests**

```bash
flock build/.build.lock ninja -C build clickhouse > build/build_task2.log 2>&1; tail -3 build/build_task2.log
kill $(cat build/srv/server.pid) 2>/dev/null; sleep 3    # the server from Task 1 (it may already be dead)
# start the server again with the recipe from "How to run the test locally" (same $D, same ports)
CLICKHOUSE_PORT_TCP=19481 CLICKHOUSE_PORT_HTTP=19482 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 01168_mutations_isolation 01169_alter_partition_isolation_stress 01170_alter_partition_isolation 04141_transaction_after_commit_no_premature_wakeup > build/test_task2.log 2>&1; tail -8 build/test_task2.log
```

Expected: all pass (`01169` is a stress test and takes a while; if it is flaky for an environmental reason unrelated to mutations, say so with the log excerpt). Also verify the rewritten file loads: after `01168` finishes, no `Cannot parse` / `assertEOF` errors mentioning `mutation_` in `build/srv/server.log`.

- [ ] **Step 5: No commit in this task**

The production changes are committed together in Task 3 (spec §4: the second commit holds the whole fix). Leave
`MergeTreeMutationEntry.{h,cpp}` modified in the working tree; verify with `git status --short src/` that only these
two files are modified.

---

### Task 3: The retry helper, its call sites, and the killed-mutation branch

**Files:**
- Modify: `src/Interpreters/MergeTreeTransaction.cpp:19-32` (error codes, anonymous namespace), `:262-313` (`afterCommit`), `:315-372` (`rollback`)
- Modify: `src/Storages/StorageMergeTree.cpp:1123-1132` (`setMutationCSN`)

**Interfaces:**
- Consumes: the log-message contract from Task 1 (exact strings below); `TransactionLog::instance().isShuttingDown()` (exists, `TransactionLog.h:131`); `sleepForMilliseconds` from `<base/sleep.h>` (already included); `Stopwatch` from `<Common/Stopwatch.h>`.
- Produces: nothing used by later tasks.

- [ ] **Step 1: Add the helper**

In `src/Interpreters/MergeTreeTransaction.cpp`, after the `namespace FailPoints { ... }` block and before the first function, add (also add `#include <Common/Stopwatch.h>` and `#include <Common/logger_useful.h>` to the includes if missing):

```cpp
namespace
{

/// A metadata write made after the commit point, or during rollback, has no one to report
/// an error to: the caller is a noexcept callback and the transaction's fate is already
/// decided in the transaction log. Instead of letting the exception terminate the server,
/// the write is retried for a bounded time. LOGICAL_ERROR and NOT_IMPLEMENTED are invariant
/// violations and are rethrown at once. When the budget is exhausted, or the server is
/// shutting down, the error is rethrown too: this keeps the old behaviour rather than
/// hiding a write that did not happen. The budget is per object; a write that hangs
/// inside the storage is not interrupted.
constexpr UInt64 TRANSACTION_METADATA_STORE_RETRY_TIMEOUT_SECONDS = 60;
constexpr UInt64 TRANSACTION_METADATA_STORE_RETRY_BACKOFF_MS = 100;
constexpr UInt64 TRANSACTION_METADATA_STORE_RETRY_MAX_BACKOFF_MS = 2000;

template <typename F>
void retryMetadataStore(LoggerPtr log, const String & what, F && store)
{
    Stopwatch watch;
    UInt64 backoff_ms = TRANSACTION_METADATA_STORE_RETRY_BACKOFF_MS;
    size_t attempts = 0;
    while (true)
    {
        ++attempts;
        try
        {
            store();
            if (attempts > 1)
                LOG_INFO(log, "Stored transaction metadata for {} after {} attempts", what, attempts);
            return;
        }
        catch (...)
        {
            int code = getCurrentExceptionCode();
            if (code == ErrorCodes::LOGICAL_ERROR || code == ErrorCodes::NOT_IMPLEMENTED)
                throw;

            bool give_up = watch.elapsedSeconds() >= TRANSACTION_METADATA_STORE_RETRY_TIMEOUT_SECONDS
                || TransactionLog::instance().isShuttingDown();
            if (give_up)
            {
                LOG_ERROR(log, "Cannot store transaction metadata for {} after {} attempts in {:.1f} s, giving up: {}",
                    what, attempts, watch.elapsedSeconds(), getCurrentExceptionMessage(false));
                throw;
            }

            if (attempts == 1)
                LOG_WARNING(log, "Cannot store transaction metadata for {}, will retry: {}", what, getCurrentExceptionMessage(false));
            else
                LOG_DEBUG(log, "Cannot store transaction metadata for {}, attempt {}: {}", what, attempts, getCurrentExceptionMessage(false));
        }

        sleepForMilliseconds(backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, TRANSACTION_METADATA_STORE_RETRY_MAX_BACKOFF_MS);
    }
}

String partDescription(const IMergeTreeDataPart & part)
{
    return fmt::format("part {} of {}", part.name, part.storage.getStorageID().getNameForLogs());
}

String mutationDescription(const IStorage & storage, const String & mutation_id)
{
    return fmt::format("mutation {} of {}", mutation_id, storage.getStorageID().getNameForLogs());
}

}
```

`ErrorCodes::LOGICAL_ERROR` and `ErrorCodes::NOT_IMPLEMENTED` are already declared in this file. Use `getLogger("MergeTreeTransaction")` for `log` at the call sites (the class has no logger member); store it once per callback in a local `auto log = getLogger("MergeTreeTransaction");`.

- [ ] **Step 2: Wrap the three writes in `afterCommit`**

Replace the two loops and the mutation loop in `afterCommit` with:

```cpp
    auto log = getLogger("MergeTreeTransaction");

    for (const auto & part : created_parts)
        retryMetadataStore(log, partDescription(*part), [&] { part->version->setAndStoreCreationCSN(assigned_csn); });

    for (const auto & part : removed_parts)
        retryMetadataStore(log, partDescription(*part), [&] { part->version->setAndStoreRemovalCSN(assigned_csn); });

    for (const auto & storage_and_mutation : committed_mutations)
        retryMetadataStore(log, mutationDescription(*storage_and_mutation.first, storage_and_mutation.second),
            [&] { storage_and_mutation.first->setMutationCSN(storage_and_mutation.second, assigned_csn); });
```

Update the comment block above the loops: keep the ordering explanation (writes before the `csn` flip) and the crash-safety note; replace the sentence in the pause-failpoint comment that says the `setAndStore...CSN` calls "already trust their callees not to throw" with: `The writes above go through retryMetadataStore, which absorbs recoverable storage errors within its retry budget.` Keep comments short.

- [ ] **Step 3: Wrap the three writes in `rollback`**

```cpp
    auto log = getLogger("MergeTreeTransaction");

    /// Forcefully stop related mutations if any
    for (const auto & table_and_mutation : mutations_to_kill)
        retryMetadataStore(log, mutationDescription(*table_and_mutation.first, table_and_mutation.second),
            [&] { table_and_mutation.first->killMutation(table_and_mutation.second); });
    ...
    for (const auto & part : parts_to_remove)
    {
        /// Write special RolledBackCSN, so we will be able to cleanup transaction log
        retryMetadataStore(log, partDescription(*part), [&] { part->version->setAndStoreCreationCSN(Tx::RolledBackCSN); });
    }
    ...
    for (const auto & part : parts_to_activate)
    {
        /// Clear removal_tid from version metadata file, so we will not need to distinguish TIDs that were not committed
        /// and TIDs that were committed long time ago and were removed from the log on log cleanup.
        retryMetadataStore(log, partDescription(*part), [&] { part->version->setAndStoreRemovalTID(Tx::EmptyTID); });
        part->version->unlockRemovalTID(tid, TransactionInfoContext{part->storage.getStorageID(), part->name});
    }
```

`removePartsFromWorkingSet` and `restoreAndActivatePart` stay as they are (in-memory). `killMutation` returns a `CancellationCode`; the lambda discards it, which is what the current code does too.

- [ ] **Step 4: The killed-mutation branch in `setMutationCSN`**

In `src/Storages/StorageMergeTree.cpp`, `setMutationCSN`:

```cpp
    std::lock_guard lock(currently_processing_in_background_mutex);
    auto it = current_mutations_by_version.find(version);
    if (it == current_mutations_by_version.end())
    {
        /// KILL MUTATION erases the entry before the committing transaction stores the CSN,
        /// and cannot roll that transaction back any more. The parts are already mutated,
        /// so there is nothing left to write for this mutation.
        LOG_WARNING(log, "Mutation {} was killed before its CSN {} could be stored", mutation_id, csn);
        return;
    }
    it->second.writeCSN(csn);
```

- [ ] **Step 5: Build, run the new test and the transaction tests**

```bash
flock build/.build.lock ninja -C build clickhouse > build/build_task3.log 2>&1; tail -3 build/build_task3.log
```

Restart the server on the new binary (kill by pid, start with the recipe), then:

```bash
CLICKHOUSE_PORT_TCP=19481 CLICKHOUSE_PORT_HTTP=19482 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 05053_transaction_metadata_store_retry > build/test_05053_task3.log 2>&1; tail -5 build/test_05053_task3.log
CLICKHOUSE_PORT_TCP=19481 CLICKHOUSE_PORT_HTTP=19482 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful 0116 0117 04141 > build/test_txn_task3.log 2>&1; tail -8 build/test_txn_task3.log
```

Expected: `05053` passes with output equal to the reference; every `0116*`/`0117*` transaction test and `04141` pass (tests that need a replicated database or a real Keeper cluster may be skipped by tag; skipped is fine, failed is not). Run `05053` three times in a row to check it is stable.

- [ ] **Step 6: Forbidden-word sweep and commit**

```bash
git diff altinity/antalya-26.6 | grep -in "cas\b\|content.addressed\|altinity\|antalya" ; echo "sweep exit=$?"   # must print only 'sweep exit=1'
git status --short src/   # exactly: MergeTreeTransaction.cpp, StorageMergeTree.cpp, MergeTreeMutationEntry.h, MergeTreeMutationEntry.cpp
git diff --cached --stat   # must print nothing
git commit -s -m "Retry transaction metadata writes under noexcept callbacks instead of terminating

MergeTreeTransaction::afterCommit and rollback write part version metadata
and mutation CSNs to disk. A storage error there escaped a noexcept
function and terminated the server, although the transaction is already
committed (or rolled back) and a restart repairs the files from the
transaction log. The writes now go through retryMetadataStore: the error is
retried with backoff for up to 60 seconds per object; LOGICAL_ERROR and
NOT_IMPLEMENTED are rethrown at once; an exhausted budget, or a server
shutdown, rethrows as before.

KILL MUTATION between the log write and afterCommit erases the mutation
entry and cannot roll the committing transaction back; setMutationCSN then
threw LOGICAL_ERROR under noexcept. It now logs a warning: the parts are
already mutated and there is nothing left to write.

MergeTreeMutationEntry::writeCSN appended one line to the mutation file; a
write that fails half-way, or is repeated, could leave a partial or a
duplicated csn line, which the loader rejects. The record is now written
through a temporary file and replaces the old one, so a retry is
idempotent.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GhVd7eMAWdFubNk4g1B2Tx" -- src/Interpreters/MergeTreeTransaction.cpp src/Storages/StorageMergeTree.cpp src/Storages/MergeTree/MergeTreeMutationEntry.h src/Storages/MergeTree/MergeTreeMutationEntry.cpp
```

---

### Task 4: Sanitizer gate and final verification

**Files:** none modified; produces the logs below in `build_asan/`.

- [ ] **Step 1: Build the ASan binary in the existing `build_asan/`**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
flock build_asan/.build.lock bash -c "cmake -S . -B build_asan > build_asan/cmake_txn.log 2>&1; ninja -C build_asan clickhouse > build_asan/build_txn.log 2>&1"; tail -3 build_asan/build_txn.log
```

(`build_asan/` is already configured with `SANITIZE=address`; the `cmake` call only regenerates the graph for the current tree. Do not create any other build directory.)

- [ ] **Step 2: Run the new test and the transaction subset under ASan**

Start a server from `build_asan/programs/clickhouse` on ports `19491-19495` (same recipe with `$D=$PWD/build_asan/srv`; the Keeper ports in `programs/server/config.d/keeper_port.xml` and `zookeeper.xml` are shared with the release server, so stop the release server first, by pid), then:

```bash
CLICKHOUSE_PORT_TCP=19491 CLICKHOUSE_PORT_HTTP=19492 python3 tests/clickhouse-test -b $PWD/build_asan/programs/clickhouse --no-stateful 05053_transaction_metadata_store_retry 04141 0116 0117 > build_asan/test_txn.log 2>&1; tail -8 build_asan/test_txn.log
grep -c "AddressSanitizer\|LOGICAL_ERROR" build_asan/srv/server.log
```

Expected: all pass or skipped; no `AddressSanitizer` report; no `LOGICAL_ERROR` for the tested tables. Stop both servers by pid.

- [ ] **Step 3: Style check on the diff**

```bash
git diff altinity/antalya-26.6 | grep -n "^+.*) {$" ; echo "K&R brace lines above must be none (Allman style)"
git diff altinity/antalya-26.6 | grep -in "cas\b\|content.addressed\|altinity\|antalya" ; echo "forbidden-word lines above must be none"
./ci/jobs/scripts/check_style/check_cpp.sh > build/check_cpp.log 2>&1; echo "check_cpp exit=$?"; grep -i "MergeTreeTransaction\|MergeTreeMutationEntry\|StorageMergeTree\|05053" build/check_cpp.log || echo "check_cpp: nothing for our files"
```

- [ ] **Step 4: Report**

Write `build/FINAL_REPORT.md` in the worktree with: the two commit hashes, the test outputs (tail of each log), the ASan result, and any deviation from the plan. No commit for this task.
