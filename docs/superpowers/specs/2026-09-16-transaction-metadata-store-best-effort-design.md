---
description: 'Design for the disk writes MergeTree transactions perform inside their noexcept commit and rollback callbacks: a write that fails with a storage error is retried in place, with a bounded budget, before the transaction changes state; only an invariant violation or an exhausted budget keeps the current behaviour (the server terminates). No transaction state is deferred, so the transaction-log retention, concurrency and restart arguments stay exactly as they are today.'
sidebar_label: 'Transaction metadata store, bounded retry'
sidebar_position: 12
slug: /superpowers/specs/transaction-metadata-store-best-effort-design
title: 'Transaction metadata writes under noexcept callbacks do not terminate the server'
doc_type: 'design'
---

# Transaction metadata writes under `noexcept` callbacks do not terminate the server {#transaction-metadata-store-best-effort}

Revision 3g, 2026-09-29 (revision 3f plus the PR review of https://github.com/Altinity/ClickHouse/pull/2396:
one retry budget per callback, no retry around `killMutation`, `loadMutations` lists names before touching files,
`csn` set after the replace; revision 3e plus the full post-fix review: tolerant temporary cleanup in `loadMutations`, session-shutdown lock note; revision 3d plus the post-implementation reviews: ONCE failpoints, a test without
polling, lazy log descriptions, a backend caveat; before that, revision 3 plus review rounds 3 to 6 and one user decision, no waiting during shutdown: no shutdown exit, idempotent mutation-CSN write
in its own temporary-file namespace, per-object budget, tests that inspect the persisted records). Revision 1 proposed plain best-effort persistence on the premise that the transaction log
can always re-derive a CSN by tid; review round 1 refuted it (`TransactionLog::removeOldEntries` deletes the entry
once every snapshot that could need it is released, and `VersionMetadata::tryGetCSN` then reads "no CSN, not
running" as rolled back). Revision 2 deferred finalization of a transaction whose writes failed and retried them
from the log updating thread; review round 2 showed that this needs a publish-ordering rule between the retry and
concurrent transactions, durable obligations that survive restart and `DETACH TABLE ... PERMANENTLY`, and a
coordination with `KILL MUTATION`, none of which the original problem asks for. Revision 3 removes the deferral:
the failing write is retried where it already runs, before the transaction changes state.

Scope: `src/Interpreters/MergeTreeTransaction.cpp` (one helper, six call sites), `src/Storages/StorageMergeTree.cpp`
(`setMutationCSN`, one branch), `src/Storages/MergeTree/MergeTreeMutationEntry.cpp` (`writeCSN` rewrites the record),
two failpoints, one stateless test. `TransactionLog`, `VersionMetadata` and `VersionMetadataOnDisk` (apart from one
failpoint line) are not changed.

## 1. Problem {#problem}

`MergeTreeTransaction::afterCommit` and `MergeTreeTransaction::rollback` are `noexcept`
(`MergeTreeTransaction.h:76-77`); so are `TransactionLog::finalizeCommittedTransaction` and
`TransactionLog::rollbackTransaction`. Inside them these disk writes happen:

| callback | call | what is written |
|---|---|---|
| `afterCommit` | `part->version->setAndStoreCreationCSN(csn)` | `creation_csn` in the part's `txn_version.txt` |
| `afterCommit` | `part->version->setAndStoreRemovalCSN(csn)` | `removal_csn` in the part's `txn_version.txt` |
| `afterCommit` | `storage->setMutationCSN(id, csn)` → `MergeTreeMutationEntry::writeCSN` | the `csn: N` line of the mutation file |
| `rollback` | `storage->killMutation(id)` → `MergeTreeMutationEntry::removeFile` | deletes the mutation file |
| `rollback` | `part->version->setAndStoreCreationCSN(Tx::RolledBackCSN)` | `creation_csn` |
| `rollback` | `part->version->setAndStoreRemovalTID(Tx::EmptyTID)` | clears `removal_tid` |

(`unlockRemovalTID` is in-memory only: `VersionMetadataOnDisk.cpp:141-169`.)

Every write goes through `IDataPartStorage::writeFile` / `IDisk::writeFile` / `IDisk::removeFileIfExists`. On a
local disk that practically never throws; on an object-storage disk the write is a `DiskObjectStorageTransaction`
whose commit can fail for ordinary reasons (a request timeout, a throttled or unavailable endpoint, a disk whose
background machinery is not ready yet). Any exception escaping a `noexcept` function calls `std::terminate`: one
transient storage error in one part's metadata takes down the whole server.

A second, unrelated way to reach `std::terminate` from the same place: `KILL MUTATION` on the mutation of a
transaction that is between its log write and `afterCommit`. `StorageMergeTree::killMutation` erases the entry
from `current_mutations_by_version` first (`StorageMergeTree.cpp:1409-1417`), then tries to roll the transaction
back, which fails because the transaction's `csn` is already `Tx::CommittingCSN`
(`MergeTreeTransaction.cpp:245`, `:320`; `TransactionLog.cpp:543-548`), then removes the file. `afterCommit` then
calls `setMutationCSN`, which throws `LOGICAL_ERROR` "Cannot find mutation" (`StorageMergeTree.cpp:1069`) under
`noexcept`. This is reachable from a user action today, and the retry in §3 widens the window, so it is fixed here.

## 2. Why the write is retried in place, not deferred {#why-in-place}

The transaction's commit is already "durable in the log, not yet reflected on disk" for the whole duration of
`afterCommit`. What keeps that state correct today, for however long `afterCommit` takes:

- The log entry (`csn-` node) exists, and the transaction is still in `running_list` with its snapshot pinned, so
  `removeOldEntries` cannot delete the entry (`tid.start_csn` equals the transaction's own snapshot and the predicate
  is `tid.start_csn < tail_ptr`, `TransactionLog.cpp:330-337`).
- Readers do not wait for `afterCommit`. `VersionMetadata::isVisible` (`VersionMetadata.cpp:46-100`) consults the
  log's CSN mapping directly when the in-memory CSN is unknown, so once the updating thread has published the CSN,
  another transaction with a suitable snapshot already sees the committed result. That is correct because the
  answer comes from the retained log entry, not from the file. (`waitStateChange` serves the commit interpreter's
  unknown-state handling, `InterpreterTransactionControlQuery.cpp:84`, not visibility.)
- The file writes are versioned updates (`updateInfoWithRefreshDataThenStoreAndSetMetadata`) that publish to memory
  only after the store succeeded; a crash at any point is repaired at the next load by
  `VersionMetadata::loadAndUpdateMetadata` / `StorageMergeTree::loadMutations` from the entry.

So the cheapest correct place to absorb a transient failure is inside the callback: retry the write there, before
finalization. Nothing new has to be remembered, ordered, made durable or reconciled with
`KILL MUTATION`, `DETACH ... PERMANENTLY` or restart. Revision 2 needed all of that only because it let the
transaction leave the callback with its writes still owed.

`rollback` flips `csn` to `RolledBackCSN` first (`MergeTreeTransaction.cpp:319-320`) and then does its cleanup
writes while the transaction keeps its registration and part references until `rollbackTransaction` returns. A
rolled-back transaction has no log entry, so on the next load a missing `RolledBackCSN` / `EmptyTID` is re-derived
(`tryGetCSN`: no CSN, not running); retrying the writes before `rollback` returns keeps the in-memory and on-disk
views moving together and avoids leaving a `removal_tid` lock on a part until restart.

The cost is that `COMMIT` (or `ROLLBACK`) blocks while the write is retried, bounded per object by §3.1.

## 3. Design {#design}

### 3.1 One helper: `retryMetadataStore` {#retry-helper}

In `MergeTreeTransaction.cpp`, an anonymous-namespace function:

```cpp
/// Runs a metadata write that must not fail inside a noexcept callback.
/// Errors are retried with backoff for a bounded time, except LOGICAL_ERROR and
/// NOT_IMPLEMENTED, which are rethrown at once. An exhausted budget rethrows,
/// which keeps the current behaviour (the server terminates) instead of hiding
/// a lost write.
template <typename Describe, typename F>
void retryMetadataStore(LoggerPtr log, Describe && describe, F && store);
```

`describe` returns the object description (`String`) and is called only when a log line is written: a callback
that never fails formats nothing, and nothing that can allocate or take a mutex runs outside the helper's `try`
in the `noexcept` callbacks.

Behaviour, for each attempt that throws:

1. `getCurrentExceptionCode()` is `LOGICAL_ERROR` or `NOT_IMPLEMENTED` → rethrow. `VersionMetadata::validateInfo`
   (`VersionMetadata.cpp:515-548`) and `VersionMetadataOnDisk::storeInfoUnlocked` (`:195-199`, `!can_write_metadata`
   on a read-only disk / any other disk) throw these; they stay fatal exactly as today.
2. Elapsed ≥ `TRANSACTION_METADATA_STORE_RETRY_TIMEOUT` (60 seconds; constant in `MergeTreeTransaction.cpp`),
   or `TransactionLog::instance().isShuttingDown()` → log at Error (`Cannot store transaction metadata for {what}
   after {n} attempts in {elapsed}, giving up`) and rethrow. Behaviour is then the same as today. During shutdown
   there is no point in waiting: a failed retry ends in the same rethrow, and rethrowing at once leaves the same
   on-disk state (the snapshot stays pinned, the log entry is retained, the next start repairs from it).
3. Otherwise: on the first failure log at Warning (`Cannot store transaction metadata for {what}, will retry:
   {exception}`), later failures at Debug; sleep with backoff 100 ms doubling to 2 s (`sleepForMilliseconds`);
   call `store()` again.

On success after at least one failure log at Information (`Stored transaction metadata for {what} after {n}
attempts`). `what` names the object: `part {name} of {table}` or `mutation {file} of {table}`.

There is no other exit. In particular the helper never returns successfully without the write having landed,
not even during shutdown: a callback that skipped a write and finalized could release its snapshot while an
updating-thread iteration already past its `stop_flag` check (`TransactionLog.cpp:232`) prunes the entry, and a
skipped `RolledBackCSN` store would make `removePartsFromWorkingSet` (`MergeTreeData.cpp:5747`) issue another
metadata write outside the helper. Shutdown only shortens the wait (step 2), it never skips the write.

**Classification.** Every error other than the two invariant codes is retried, including errors that are not
transient (a malformed metadata file, `FS_METADATA_ERROR`, `STALE_VERSION` after `MAX_RETRIES`). This is
deliberate: the helper has no path that swallows an error, so a misclassified permanent error costs at most the
budget and then terminates the server exactly as today. A finer classification would buy a faster abort for cases
that are bugs anyway; it is not worth an allow-list that has to follow every backend's error codes.

**Budget semantics.** The budget is a retry-admission budget shared by all writes of one callback: a `Stopwatch`
started at the beginning of `afterCommit` / `rollback` is passed to every `retryMetadataStore` call, and the check
after a failed attempt decides whether another attempt is started. So a commit of `k` parts waits at most one
budget in total, and a retry on the unknown-state path blocks the log updating thread for at most one budget. The
budget does not bound the duration of an attempt (a write that hangs in the backend hangs as today).

### 3.2 Idempotency of the retried writes {#idempotency}

A retry is correct only if repeating the write after an arbitrary failure of the previous attempt leaves the same
result as one successful write.

- **Part version stores** (`setAndStoreCreationCSN` / `setAndStoreRemovalCSN` / `setAndStoreRemovalTID`) go
  through `updateInfoWithRefreshDataThenStoreAndSetMetadata` (`VersionMetadata.cpp:284`): each call reloads the
  current info on a version mismatch, applies the update, validates, stores, and publishes to memory only after
  the store succeeded. `storeInfoToDataPartStorage` (`VersionMetadataOnDisk.cpp:323-360`) writes a temporary file
  and `replaceFile`s it over the old one (or, on a storage that supports atomic writes, writes the file once). A
  failed attempt leaves the old file when the backend's `replaceFile` keeps its destination on failure. The
  object-storage metadata backend does not always guarantee that: `ReplaceFileOperation::execute`
  (`MetadataStorageFromDiskTransactionOperations.cpp:428`) moves the destination aside and then the replacement in,
  and if the second move and its undo both fail, the destination is gone. The next load then manufactures
  non-transactional metadata (`VersionMetadataOnDisk.cpp:86`), and a retried removal store fails `validateInfo`
  with `LOGICAL_ERROR` → rethrow → terminate. Today the same double failure terminates the server at once, and at
  restart loads the part as non-transactional with the removal lost, silently. The retry is therefore never worse
  than the current behaviour on this path; making `replaceFile` keep its destination is a metadata-storage fix
  outside this design and is listed in §5.
- **Mutation CSN.** `MergeTreeMutationEntry::writeCSN` (`MergeTreeMutationEntry.cpp:111-116`) today appends one line
  through a buffer that loops over short writes (`WriteBufferFromFileDescriptor.cpp:52`): an attempt can append a
  prefix and then throw, and a retry would append a second, complete line after it; the loader
  (`:149-159`) requires exactly one `csn:` line followed by EOF. So `writeCSN` is changed to rewrite the whole
  record: the constructor's serialization (`:60-77`, format version, create time, commands, tid) moves into a
  private `writeRecord(WriteBuffer &)` also used by `writeCSN`, which writes the record plus the `csn:` line to
  `tmp_mutation_csn_<N>.txt` (its own namespace: `tmp_mutation_<N>.txt` is the constructor's temporary name and
  its `N` comes from an independent counter, `insert_increment`, so the two could collide; the `tmp_mutation_`
  prefix keeps the new name covered by the startup cleanup at `StorageMergeTree.cpp:1506`), `finalize`s and
  `sync`s it like the constructor does (`:78-79`), and `replaceFile`s it over `file_name`. A failed attempt leaves the old file (with the same
  backend caveat as above; a mutation file without a `csn:` line is repaired by `loadMutations` from the log
  entry, `StorageMergeTree.cpp:1481-1487`); a repeated attempt writes the same complete record. The loader is
  unchanged.
- **`killMutation`** is not retried: it erases the entry from `current_mutations_by_version` before deleting
  the file, so a repeated call after a failure returns `CancellationCode::NotFound` and cannot delete anything.
  It runs once inside `try/catch`; `LOGICAL_ERROR` and `NOT_IMPLEMENTED` propagate, any other error is logged at
  Warning. A mutation file that survived is ignored while the server runs (not in the map) and deleted by
  `loadMutations` at the next load because its tid has no CSN (`:1488-1494`).

### 3.3 Call sites {#call-sites}

All six writes in the table of §1 are wrapped, each object separately, keeping the existing loop order:

```cpp
for (const auto & part : creating_parts)
    retryMetadataStore(log, fmt::format("part {} of {}", part->name, part->storage.getStorageID().getNameForLogs()),
        [&] { part->version->setAndStoreCreationCSN(assigned_csn); });
```

and likewise for `removing_parts` (`setAndStoreRemovalCSN`), `mutations` (`setMutationCSN`), and in `rollback`
for `setAndStoreCreationCSN(Tx::RolledBackCSN)` and `setAndStoreRemovalTID(Tx::EmptyTID)`; `killMutation` gets
the single-attempt `try/catch` of §3.2. Each callback creates one `Stopwatch` and passes it to every call.
`removePartsFromWorkingSet` and `restoreAndActivatePart` in `rollback` are in-memory operations and are not wrapped.
`MergeTreeData::Transaction::rollback` (non-transactional path) already runs under a catch in its destructor and
is not changed.

What the retry holds meanwhile (verified in review round 3): no `TransactionLog` mutex (`finalizeCommittedTransaction`
takes `running_list_mutex` after `afterCommit`); the transaction's own mutex only while the work lists are copied;
`persisted_info_mutex` of the part and `currently_processing_in_background_mutex` of the table only for the
duration of one attempt, released before the backoff sleep. What it delays: the transaction's snapshot and part
references (so cleanup of the parts it touched, and log pruning behind its snapshot); a `DROP`/synchronous `DETACH`
of the table waiting for those references; for a transaction finalized from the unknown-state path, the log
updating thread itself, so other commits wait in `waitForCSNLoaded` for the duration of the retry. All of these
are the same things a slow write delays today, for longer.

### 3.4 `setMutationCSN` on a killed mutation {#set-mutation-csn}

`StorageMergeTree::setMutationCSN` (`StorageMergeTree.cpp:1061-1070`): when the mutation id is not in
`current_mutations_by_version`, log at Warning (`Mutation {} was killed before its CSN {} could be stored`) and
return instead of throwing `LOGICAL_ERROR`. The only code path that erases an entry while its transaction is
committing is `killMutation` (§1). The warning tolerates an in-memory cancellation; it does not claim the file is
gone: `killMutation` erases the entry, releases the mutex, and deletes the file afterwards (`:1415`, `:1431`), and
the deletion can be pending or fail. If the file survives, the next `loadMutations` resolves its tid to the
committed CSN while the log mapping exists, writes it and loads the mutation (`:1481-1487`), or deletes the
CSN-less file once the mapping is gone (`:1488-1494`); either way its parts were materialized by the committed
transaction, so nothing is left to do and no visibility answer depends on the skipped write.

### 3.5 Restart {#restart}

Unchanged. A crash leaves what an unmodified server leaves after a crash at the same point, and
`loadAndUpdateMetadata` / `loadMutations` repair it from the log entry, which is retained as argued in §2.

### 3.6 Failpoints {#failpoint}

Two ONCE failpoints in `src/Common/FailPoint.cpp`, both throwing `Exception(ErrorCodes::FAULT_INJECTED, ...)`
before any I/O, so a failed attempt leaves the old file:

- `transaction_metadata_store_fail`: first line of `VersionMetadataOnDisk::storeInfoToDataPartStorage`
  (`VersionMetadataOnDisk.cpp:323`).
- `transaction_mutation_csn_store_fail`: first line of `MergeTreeMutationEntry::writeCSN`, after `csn = csn_`.

ONCE (fires once, then disables itself) is the documented shape for "a transient error in an operation that
retries" (`FailPoint.h`): the retried write fails exactly once and the next attempt succeeds, so a test needs no
hand-off between the failing and the succeeding attempt and cannot race the retry budget. Two names because the
callback processes objects serially and each scenario targets one kind of write. Not in `removeFile`: a retry of
`killMutation` is a no-op after the map erase (§3.2), so a failpoint there would test restart cleanup, not the
retry. `FAULT_INJECTED` is neither `LOGICAL_ERROR` nor `NOT_IMPLEMENTED`, so the helper retries it. The
registration lines sit next to an entry that exists in every branch (`replicated_queue_unfail_entries`), so the
hunk applies to upstream `master` unchanged.

### 3.7 Comments {#comments}

Short, in plain English, on the helper (why errors are retried and invariant errors are not; why the budget
rethrows; why shutdown stops the retry but never skips the write), on `writeCSN` (why the record is rewritten instead of appended), on the
`setMutationCSN` branch (the `killMutation` ordering), and on each failpoint line (before any I/O). No comment
mentions a specific storage backend, a fork, or a ticket.

## 4. Tests, failing-first order {#tests}

One stateless test `0_stateless/<next number>_transaction_metadata_store_retry.sh`, tags `no-ordinary-database,
no-replicated-database, no-shared-merge-tree, no-encrypted-storage, no-object-storage, no-parallel` (server-wide failpoints;
raw metadata files are read; in a Replicated database the transactional `ALTER UPDATE` of scenario B is routed through
replicated DDL, which is refused inside a transaction), using `transactions.lib` (`tx_async`, `tx_wait`) like
`04141_transaction_after_commit_no_premature_wakeup.sh`, with a `trap` that disables both failpoints on exit.

The callback processes its objects one after another, and each scenario targets one kind of write with a ONCE
failpoint, so exactly one object fails exactly once, on the first attempt, and its retry succeeds. Each scenario:

1. `SYSTEM ENABLE FAILPOINT <name>`.
2. Run the transactional statement synchronously (`tx_sync`); it prints nothing on success and an error text on
   failure, which breaks the reference: that is the success assertion.
3. `SYSTEM FLUSH LOGS text_log`; assert from `system.text_log`, matching the table by its UUID (a rerun in the
   same database must not see lines of a previous run): exactly one `Cannot store transaction metadata for
   <object>, will retry` object, exactly one `Stored transaction metadata for <object> after N attempts` object,
   the two objects equal, and `N = 2`. Then the durable-state check below.

**Durable-state check.** `system.parts` reads in-memory metadata (`StorageSystemParts.cpp:365`), and a plain
`DETACH`/`ATTACH` repairs a missing CSN from the log entry (`loadAndUpdateMetadata`) for as long as the entry or
the cached `tid_to_csn` mapping exists; waiting for the entry to be pruned is not deterministic (pruning needs the
tail pointer to move past the entry and the updating thread to wake, and the cache is erased only after the
whole deletion loop, `TransactionLog.cpp:312`, `:336`, `:340`). So the test inspects the persisted records
directly, the way `04104_transaction_version_metadata_dummy_tid_load.sh` does: it takes `path` from
`system.parts` and reads `txn_version.txt` with the shell, and reads the mutation file `mutation_<N>.txt` from the
table's data path. The assertions are `creation_csn: <csn>` in the new part's file (A), and exactly one `csn: <csn>`
line, last in the file, in the mutation file (B). `<csn>` is read from `system.transactions_info_log` (`type = 'Commit'`,
column `csn`, matched by the `tid` that the test reads from `system.transactions` while the transaction is still
running, before `COMMIT`); the row is written before `afterCommit` runs (`TransactionLog.cpp:496`) and the test
`SYSTEM FLUSH LOGS` before reading it. Neither `system.transactions` (no CSN column; the first component of `tid`
is the start CSN) nor SQL `COMMIT` (returns nothing) provides the commit CSN. The
test is tagged `no-object-storage` for that reason (on an object-storage disk the local file is a metadata file,
not the record); the failpoint and the helper are backend-independent, this test exercises the retry against local
persistence only. After
the file check the test also runs `DETACH TABLE` / `ATTACH TABLE` and asserts `count()` (and `sum(v)` in B) as a
sanity check, not as the discriminator. Outdated parts are not asserted on: transactional `DROP PARTITION` removes
them without the `old_parts_lifetime` delay (`StorageMergeTree.cpp:2784`, `MergeTreeData.cpp:5743`), so any
assertion on them races with cleanup.

- **Scenario A, commit of parts.** Table `MergeTree ORDER BY k`, `SYSTEM STOP MERGES`, two inserts outside a
  transaction. In a transaction: `ALTER TABLE t DROP PARTITION ID 'all'`, then `INSERT` (one new part); failpoint
  `transaction_metadata_store_fail`; `COMMIT`. The retried object is the first part of `creating_parts` (the new
  part); nothing else stores version metadata between the enable and the commit (merges stopped, no parallel test). Durable-state check: the new part's `txn_version.txt` carries `creation_csn: <csn>`.
- **Scenario B, commit of a mutation.** Table with columns `k` (key) and `v`; `SYSTEM STOP MERGES` is **not**
  issued (a transactional mutation waits for its parts, `StorageMergeTree.cpp:1097`, and `STOP MERGES` blocks
  mutations). Insert outside a transaction; in a transaction `ALTER TABLE t UPDATE v = v + 1 WHERE 1`; failpoint
  `transaction_mutation_csn_store_fail` (the parts' stores succeed, the mutation's is retried); `COMMIT`. Durable-state
  check: the mutation file ends with exactly one `csn: <csn>` line.
- **Scenario C, rollback.** As A, but `ROLLBACK`, failpoint `transaction_metadata_store_fail`; the retried object is
  the first part of `creating_parts` (`RolledBackCSN` store). After `ROLLBACK` returns: `count()` equals the two
  original parts' rows, the `will retry` / `Stored` lines as in step 6. There is no durable-state discriminator
  for rollback: a rolled-back tid re-derives to the same result at load whether or not the store landed (§2). The
  scenario asserts that the rollback path returns instead of terminating and that the retry ran.

  A rerun of the whole file is deterministic: the only state the scenarios share is the server-wide failpoints,
  each of which has fired and disabled itself by the time the next scenario enables it again.

Failing-first order, single pull request:

1. Commit the failpoint lines (§3.6) and the test. On this tree the test terminates the server at scenario A
   (`FAULT_INJECTED` escapes `afterCommit`); on an unpatched server the `SYSTEM ENABLE FAILPOINT` itself fails
   with `BAD_ARGUMENTS`. Either way the test fails first.
2. Commit the `writeCSN` rewrite (§3.2), the helper, the six call sites (§3.1, §3.3) and the `setMutationCSN`
   branch (§3.4). The test passes.

The `KILL MUTATION` race of §1/§3.4 is not tested: it needs `KILL MUTATION` to land between the log write and
`afterCommit`, and the only hook that widens that window is the failpoint of this design itself, which would then
also exercise the retry; a dedicated pauseable failpoint would be test-only code the fix does not need. Partial
writes and lost `replaceFile` destinations (§3.2) are not injected either: they are backend behaviours the design
argues about, not code paths it adds.

## 5. What this does not change {#non-goals}

- A storage that stays unavailable for longer than the budget still terminates the server, as today, with an
  explicit Error line first. Removing that residual needs durable write obligations (revision 2's direction);
  it is not part of this change.
- A `replaceFile` on the object-storage metadata backend that loses its destination when both the move and its
  undo fail (§3.2) is a metadata-storage defect with consequences today (a part silently loaded as
  non-transactional after restart); it is not fixed here and the retry does not make it worse. With the retry the
  same end state is reached without a restart: a retried `setAndStoreRemovalTID(EmptyTID)` reloads the synthesized
  non-transactional record, finds the value already equal and returns, exactly what the next load would have
  produced after the termination. One difference remains and is accepted: a termination also discards every
  running snapshot, while a successful retry keeps them, so a snapshot that predates the part's creation could
  see the part once it is reloaded as non-transactional. Closing that needs the reload path to fail closed when a
  record that was stored before has vanished (in-memory `storing_version > 0` and no file on disk) instead of
  synthesizing a non-transactional record; that is a `VersionMetadataOnDisk` change outside this design and is
  tracked as a follow-up.
- `TransactionLog` retention, snapshots, the two-stage unknown-state lists, `VersionMetadata` validation and
  publication, the mutation file format and loader: untouched.
- No new setting, no new system table column, no new metric.

## 6. Risks {#risks}

- **`COMMIT` latency under a storage incident** grows by up to one budget per object touched. The client was
  already going to lose the server; it now waits and usually succeeds.
- **The log updating thread** retries in place when it finalizes an unknown-state transaction, stalling log loading
  and pruning for other transactions for the duration. A slow write stalls it the same way today.
- **Named-session shutdown.** `Session`'s named-session registry is cleared under its global mutex at shutdown
  (`Session.cpp:96`); a session whose transaction holder rolls back there runs `rollback`, and with it the retry,
  under that mutex, before `TransactionLog`'s stop flag is set. Today a slow write holds the same mutex the same
  way; the retry lengthens it by up to one budget per object. Accepted: it only delays other session operations
  during shutdown, and moving session destruction outside the mutex is a `Session` change outside this design.
- **Interrupted `writeCSN` and loading.** After an interruption between the temporary write and `replaceFile`, both
  `mutation_<N>.txt` (without `csn:`) and `tmp_mutation_csn_<N>.txt` exist. `loadMutations` repairs the canonical
  file through `writeCSN`, which replaces `mutation_<N>.txt` and consumes the temporary name while the directory
  is being read. A directory iterator gives no guarantee about entries that change under it (a consumed temporary
  file, or the replaced canonical file returned twice, which would hit the "already exists" `LOGICAL_ERROR`), so
  `loadMutations` takes the name list with `listFiles` before touching any file and removes temporary files with
  `removeFileIfExists`.
- **Shutdown signal timing.** `TransactionLog`'s stop flag is set late in server shutdown, after executors are drained
  and databases are shut down (`Context.cpp:981`). A retry running in a background merge commit at that point is
  waited for by the executor drain before the flag can cut it short, so the shutdown shortcut of §3.1 helps
  foreground commits and the updating thread, not every path.
- **Portability.** The helper and call sites are identical in upstream `master` (checked against `upstream/master`
  in review round 3); `storeInfoToDataPartStorage` upstream lacks the single-`writeFile` branch of this tree, which
  does not affect the failpoint line (first line of the function) or §3.2. `setMutationCSN` upstream has an extra
  `setCurrentComponent` line above the lookup, unrelated to the branch changed here. `writeCSN` and the
  constructor it shares `writeRecord` with are identical upstream.
