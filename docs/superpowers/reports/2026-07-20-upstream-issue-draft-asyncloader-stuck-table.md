# DRAFT upstream issue — not yet filed

> This is a draft prepared for manual review and filing at `github.com/ClickHouse/ClickHouse`. It deliberately contains no content-addressed-storage (CAS) specifics — it describes the generic `AsyncLoader` behaviour so it stands on its own. Review, adjust, and file manually.

---

## Title

Table whose async load job failed is permanently stuck until server restart — even `DETACH TABLE` cannot recover it

## Summary

With `async_load_databases = true` (the default since 25.2, PR #74772), if a table's asynchronous `load table` job throws during startup — for example an engine whose constructor performs object-store I/O and hits a transient network error — `AsyncLoader` marks that job `FAILED` **terminally**. Every subsequent access to the table then rethrows the cached exception, and there is no way to recover the table without restarting the server. Critically, `DETACH TABLE` — the natural operator recovery action — cannot recover it either, because table resolution waits on the same failed load job and throws before the detach logic is reached.

The practical impact: a purely transient infrastructure blip (a few seconds of object-store unavailability during startup) converts into a permanent, restart-only table outage.

## Steps to reproduce (sketch)

1. Create a table on an S3-backed disk (any engine whose construction touches the object store at load time).
2. Ensure `async_load_databases = true` (default).
3. Restart the server and, during startup, make the S3 endpoint briefly unreachable (network pause, endpoint down) so the table's `load table` job throws a transient network error.
4. Restore S3 connectivity.
5. Observe: every `SELECT` / `INSERT` against the table rethrows the cached load error (`ASYNC_LOAD_WAIT_FAILED`). `DETACH TABLE db.t` fails with the same cached error. `ATTACH TABLE db.t` fails likewise. Only a full server restart recovers the table.

## Root cause (current master)

- Any access resolves the table through `DatabaseWithOwnTablesBase::tryGetTable` (`src/Databases/DatabasesCommon.cpp:430`), which calls `waitTableStarted` (`src/Databases/DatabaseOrdinary.cpp:629`) → `waitLoad` → rethrows the job's stored exception as `ASYNC_LOAD_WAIT_FAILED` (`src/Common/AsyncLoader.cpp:473`).
- `AsyncLoader` has no retry/requeue/reset path for a `FAILED` job — terminality is by design (the `AsyncLoader.h` contract states the exception is stored and rethrown from all existing and new `wait()` calls; dependent jobs are cancelled).
- `DETACH` cannot break the deadlock: `InterpreterDropQuery` resolves the table via `DatabaseCatalog::getDatabaseAndTable`, which calls `waitTableStarted` (`src/Interpreters/DatabaseCatalog.cpp:434-435`, comment "Wait for table to be started because we are going to return StoragePtr") and throws **before** reaching `DatabaseOrdinary::detachTableUnlocked` — where the state-erasing `eraseAsyncLoadState` actually lives. Catch-22: the code that would clear the failed-load state is unreachable while the failed-load state exists.

## What master already offers

- The only retry-on-access behaviour anywhere is the opt-in database setting `lazy_load_tables` (PR #96283, released in 26.2, closes #94039). A lazily-loaded table attaches as a lightweight `StorageTableProxy`; the real storage is constructed on first access, and the construction closure is discarded only on success — so a failed construction is retried on the next access. But this is opt-in, per-database, and excludes several engine kinds (views, materialized views, dictionaries, TimeSeries, table functions, and `FORCE_RESTORE` mode).
- There is no `SYSTEM` command to re-trigger a failed table load; `system.asynchronous_loader` is introspection-only.

## Related issues

- #88934 — `ASYNC_LOAD_WAIT_FAILED` leaving a table stuck after a node restart (the only maintainer reply was "repair it from another replica"; no acknowledgement of the recovery-path gap).
- #67521 — a table permanently failing to load after restart (dependency on an XML-declared dictionary); no fix.

## Suggested directions (either or both)

1. Allow `DETACH TABLE` (and `DETACH ... PERMANENTLY`) of a table whose load job is `FAILED` to bypass `waitTableStarted`, so it reaches the existing `eraseAsyncLoadState` teardown and the operator can recover the table without a restart. The erase plumbing already exists in `DatabaseOrdinary::detachTableUnlocked`; only the pre-resolution wait blocks it.
2. Provide an explicit, opt-in way to re-drive a `FAILED` table load without a full restart — either a `SYSTEM` verb (e.g. a "reload table" that reschedules the load job) or generalising the `lazy_load_tables` retry-on-touch semantics.

---

*Draft prepared 2026-07-20. Not filed. Verified against ClickHouse master as of the branch's most recent upstream merge (which included several `AsyncLoader` changes — none of them add a FAILED-job retry or alter the `DETACH` resolution order described above).*
