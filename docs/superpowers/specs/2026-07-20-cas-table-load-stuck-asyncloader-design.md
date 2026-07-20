# CAS table load stuck forever in `AsyncLoader` after a transient S3 failure — two-layer fix

Status: user-approved design (2026-07-20).

## Background {#background}

Soak v11 (`utils/ca-soak/logs/soak_5h_20260719_v11.run.log`) failed with `PHASE3 FAILED (rc=1)`.
Full RCA is in `utils/ca-soak/scenarios/BACKLOG.md` ("PRODUCT BUG (availability, MEDIUM-HIGH) — a
transient S3-backend NETWORK_ERROR during CAS table-startup recovery permanently strands the
table"). Chain, in one paragraph: a dense chaos-fault burst saturated `rustfs`'s I/O queue right
as `ch1`'s post-freeze reload ran CAS recovery for `ca_stress`; the recovery seal `PUT`
(`CasRefLedger.cpp:392-407`) exhausted `CasRequestControl`'s ~90s per-request envelope and threw
`NETWORK_ERROR` via `throwCasWriteRetryLater`; ClickHouse's `AsyncLoader` recorded the table's
`load table` job as `FAILED` — terminally. Every later touch (`SELECT`, `ATTACH`, even `DETACH`)
rethrows the cached exception in ~3ms: `tryGetTable` (`DatabasesCommon.cpp:430`) →
`waitTableStarted` (`DatabaseOrdinary.cpp:629`) → `waitLoad` → `ASYNC_LOAD_WAIT_FAILED`
(`AsyncLoader.cpp:473`). `DETACH` hits the same wait *before* reaching
`DatabaseAtomic::detachTable`, so the state-erasing plumbing (`eraseAsyncLoadState`, called from
`detachTableUnlocked`) is unreachable — a catch-22. The only recovery is a full server restart.

Upstream research (2026-07-20, against `ClickHouse/ClickHouse` master):

- `AsyncLoader` has **no** retry/requeue/reset for `FAILED` jobs; terminality is documented in the
  `AsyncLoader.h` contract. No issue or PR proposes changing that.
- The `DETACH` catch-22 exists in master too, and nobody has filed it.
- Related open issues describing the stuck state: #88934, #67521. No maintainer-stated recovery
  direction ("repair it from another replica" was the only reply).
- Since 26.2 (PR #96283, issue #94039) there is an opt-in database-level setting
  `lazy_load_tables` (default `false`, present in our fork): tables attach as a lightweight
  `StorageTableProxy`; the real storage is constructed on first access
  (`StorageTableProxy::getNested`), and the construction closure is discarded **only after
  success** — on exception the proxy stays and the next access retries. This is the only
  retry-on-touch semantics anywhere upstream. Plain `ReplicatedMergeTree` with a CAS storage
  policy is NOT excluded by `shouldLazyLoad` (`DatabaseOrdinary.cpp:421-443` excludes only
  views/MVs/dictionaries/TimeSeries/table-functions/`FORCE_RESTORE`).

Two orthogonal problems fall out of the RCA, and the fix has one layer per problem:

1. A short S3 blip (seconds-minutes) at recovery time should be ridden out, not converted into a
   failed load. Any finite retry, however, can be outlasted by a real outage.
2. After a load DOES fail, the table must be recoverable without a server restart.

## Layer 1 — bounded retry inside `ensureRefTableRecovered` (CAS code only) {#layer-1}

**Where.** `CasRefLedger::ensureRefTableRecovered`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:202-410`).
Entirely our file; zero generic/upstream code touched. This is the single funnel every
`CasRefLedger` touch-point calls first, so the fix covers all callers uniformly, not just
table load.

**What is retried.** The WHOLE recovery attempt (namespace `LIST` → snapshot/log `GET`s → replay →
seal `PUT`), not individual calls. `NETWORK_ERROR` can strand recovery from any of the three S3
surfaces (`backend.list` at `:261`, `backend.get` at `:306`/`:323`, seal `putIfAbsentControlled`
at `:395`); a retry scoped to the seal alone (the earlier BACKLOG proposal) covers one of three.
On a transient failure the next attempt restarts from a fresh `LIST` — the same shape as the
existing restart-on-vanish loop (`for (attempt ...)` at `:238`), which the new retry wraps.
Every step is idempotent: the reads trivially; the seal because it is a conditional `PUT` with a
deterministic `seal_id` — if a previous attempt actually landed, resolve-before-reissue observes
identical bytes and returns `Committed`.

**Retryable vs not.**

- Retryable: `NETWORK_ERROR` (everything `throwCasWriteRetryLater` produces, including the
  `outcome != Committed` non-exception path from this incident) and transport-class S3 errors
  thrown by direct `backend.list`/`backend.get`.
- NOT retryable (fail fast, unchanged): `CORRUPTED_DATA`, format/decode errors, `LOGICAL_ERROR`,
  and the `kRefRecoveryMaxRestarts` runaway brake — anything signalling damaged data rather than
  an unreachable backend.

**Budget and backoff.** Exponential backoff 1s → 2s → 4s → ... capped at 30s per wait; total
budget configurable (a pool-level knob in the CAS disk configuration, next to the existing
request-control settings), default **120s**. Rationale: sits on top of the existing ~90s per-request
envelope (worst-case first touch blocks ~3.5min before failing), long enough to outlast the
observed `rustfs` I/O-queue drain (~1-2min in the v11 incident), short enough that a genuinely
down backend does not pin threads for long — and with Layer 2 in place, budget exhaustion is no
longer fatal (the next touch starts over).

**Interruptible waiting.** Not a bare `sleep`: `condition_variable::wait_for` against the pool's
shutdown event, so server shutdown/pool teardown never waits out the retry tail. (This is backoff
against external I/O failure, not masking a race — the "no sleep for races" rule is not
violated.)

**Concurrency.** The retry loop lives in the same unlocked window the seal `PUT` already occupies
(`state_mutex` released at ~`:379`, reacquired after); `recovery_in_progress` (set/cleared at
`:221-234`) already serializes same-table concurrent callers across that window. The window gets
longer; no invariant changes. The existing exception-safety obligation (relock before letting an
exception propagate, so the `SCOPE_EXIT` runs locked) is preserved.

**Observability.** `LOG_WARNING` per retry (attempt number, elapsed, budget, error) + new
ProfileEvent `CasRefRecoveryRetries` next to the existing `CasRefRecoveryRestarts`.

**Tests (gtest, mock backend, following existing `Cas*` gtest patterns).**

- Backend fails transiently N times then works → recovery succeeds; `CasRefRecoveryRetries == N`.
- Backend fails longer than the budget → `NETWORK_ERROR` propagates (bounded, not infinite).
- Backend throws `CORRUPTED_DATA` → immediate failure, zero retries.

## Layer 2 — `lazy_load_tables` for CAS databases (configuration only) {#layer-2}

**No new C++.** Uses the existing upstream mechanism.

**Changes.**

1. Soak stand (`utils/ca-soak/`): `ca_stress` moves into a dedicated database created as
   `CREATE DATABASE ... ENGINE = Atomic SETTINGS lazy_load_tables = 1` (harness script edits;
   all references updated).
2. CAS deployment documentation: recommend `lazy_load_tables = 1` for databases holding CAS
   tables, with the caveat spelled out: a lazily-loaded table does not start (replication queue,
   merges) until its first access.
3. Verification test (integration, `with_rustfs`, standard CA pattern) reproducing the v11
   scenario end-to-end:
   - create lazy database + CAS table, insert data;
   - restart server (tables now lazy proxies);
   - stop `rustfs` → `SELECT` fails with a per-query error (NOT a permanently cached one);
   - `DETACH TABLE` + `ATTACH TABLE` works even while S3 is down (no catch-22);
   - start `rustfs` → next `SELECT` succeeds with no server restart.

**Risk noted for verification.** If the test shows `StorageTableProxy` does not deliver
retry-on-touch for our exact path (for example, an interaction specific to
`StorageReplicatedMergeTree::startup` or the CAS disk), fix point-wise then; code reading says
the path is clean.

## Deliverable 3 — draft upstream issue (document only, NOT filed) {#upstream-issue}

File: `docs/superpowers/reports/2026-07-20-upstream-issue-draft-asyncloader-stuck-table.md`,
in English, no CAS specifics. Content:

- Title: "Table whose async load job failed is permanently stuck until server restart — even
  `DETACH TABLE` cannot recover it".
- Minimal CAS-free repro: any engine whose constructor throws a transient error during async
  load — e.g. a table on an S3 disk while S3 is briefly unreachable at `ATTACH`/startup time.
- Root-cause chain with master file:line references: `tryGetTable` → `waitTableStarted` →
  `waitLoad` rethrow; the `DETACH` catch-22 (`eraseAsyncLoadState` exists in
  `detachTableUnlocked` but is unreachable because table resolution throws first).
- References: #88934, #67521, #96283 (`lazy_load_tables`).
- Suggested directions: allow `DETACH` of a `FAILED`-load table (reaches the existing erase
  plumbing), and/or retry-on-touch for failed load jobs.

The issue is a draft for the user to review and file manually; nothing is published by this work.

## Out of scope {#out-of-scope}

- The upstream PR actually fixing the `DETACH` catch-22 (candidate follow-up, separate effort).
- Changing the soak chaos schedule: the dense fault burst stays as-is — it is now an intentional
  compound-fault scenario that Layers 1+2 must survive.
- Any change to generic ClickHouse code in the fork.

## Acceptance {#acceptance}

- Layer 1 gtests green; existing `Cas*:CA*` gtest gate green.
- Layer 2 integration test green, including the DETACH-while-down and self-heal-after-recovery
  steps.
- A rerun of the 5h soak (v12) with both layers in place survives the same seed's fault schedule
  (or fails for an unrelated, newly-RCA'd reason).
- Draft issue file exists and is self-contained enough to file as-is.
