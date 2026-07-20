# CAS Table Load Stuck Forever in AsyncLoader — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A transient S3 failure during CAS ref-table startup recovery must no longer permanently strand a ClickHouse table until server restart.

**Architecture:** Two independent layers plus one document. Layer 1 (CAS code only) wraps the whole recovery attempt in `CasRefLedger::ensureRefTableRecovered` in a bounded retry-with-backoff so a seconds-to-minutes S3 blip is ridden out instead of becoming a failed load. Layer 2 (configuration only) puts CAS tables in a database with the existing upstream `lazy_load_tables=1` setting, so even a load that ultimately fails is retried on the next table access instead of being cached FAILED forever. Deliverable 3 is a draft upstream issue for the generic `AsyncLoader`/`DETACH` catch-22 (not filed).

**Tech Stack:** C++ (ClickHouse fork, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), GoogleTest (`src/Disks/tests/`), Python soak harness (`utils/ca-soak/`), ClickHouse Python integration tests (`tests/integration/`, `with_rustfs`).

## Global Constraints

- Branch: the current tip is `cas-ref-admits-incremental-budget` (the old `cas-gc-rebuild` reference in the spec is stale — the branch was renamed/continued and an upstream `antalya-26.6` merge plus new CAS work landed on top; the spec/plan commits are ancestors of the current HEAD). This stuck-table-load work is an independent feature: create a DEDICATED branch off the current tip for it (do NOT pile it onto the unrelated `admits()` incremental-budget work in flight). Never rebase or amend; add new commits only. Never commit to `master`. Every PR targets `master` directly (no stacked PRs).
- NO `git push` without a fresh explicit per-instance authorization from the user.
- Allman braces in all C++ (opening brace on its own line) — enforced by CI style check.
- Never use `sleep` in C++ to fix a race condition. (Layer 1's backoff is against external I/O failure, not a race; it is interruptible — this is allowed and is called out explicitly where it appears.)
- Say "exception" not "crash" for logical errors; write "ASan" not "ASAN"; wrap SQL/class/function literals in backticks in prose.
- Do not add `no-*` test tags unless strictly necessary. Use `./tests/queries/0_stateless/add-test` only for stateless tests (not used here).
- When building (ninja): redirect output to a build-dir log file, never pass `-j`/`nproc`, and summarize the log via a subagent.
- When running tests: redirect to a uniquely named log file under the build dir and summarize via a subagent.
- Temporary files go in a `tmp/` subdirectory of the CWD, never `/tmp`.
- No CA-specific fields added to generic `Replicated`/`Keeper`/`AsyncLoader`/`Database` code (Layer 1 stays entirely inside `ContentAddressed/`; Layer 2 adds zero C++).
- Spec: `docs/superpowers/specs/2026-07-20-cas-table-load-stuck-asyncloader-design.md`. RCA: `utils/ca-soak/scenarios/BACKLOG.md` ("PRODUCT BUG (availability, MEDIUM-HIGH) — a transient S3-backend NETWORK_ERROR during CAS table-startup recovery...").

---

## Task 1: Layer 1 — bounded retry inside `ensureRefTableRecovered`

> **Re-verified 2026-07-20 against the current tip** (post upstream `antalya-26.6` merge + CAS changes). Line numbers below are current-as-of-re-check but drift with every merge — always locate by content (grep), not by absolute line. AsyncLoader still has NO retry/requeue for FAILED jobs and the `DETACH` catch-22 is intact (`DatabasesCommon.cpp:430` → `DatabaseOrdinary.cpp:629` `waitTableStarted` → `AsyncLoader.cpp:473` rethrow), so both layers and the upstream draft remain valid.

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (register new event `CasRefRecoveryRetries` next to `CasRefRecoveryRestarts` at line ~773 — NOTE the house style was recently rewritten to operator-facing, no internal citations; match it, see Step 1)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h:65-106` (add three recovery-retry fields to `struct CasRequestBudget`; `retry_max_backoff_ms` is at line ~105)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (add `recovery_retry_sleep_fn` member)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:35` (extern event), `:116-119` (`setCasRetrySleepForTest`), `:222-476` (`ensureRefTableRecovered` — the retry wrap)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:45` (extern event, mirror of ref-ledger)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` — (a) UPDATE the existing `TEST(RefWriterRecoverySeal, SealPutFailureFailsRecoveryClosed)` at line ~2859 to the new retry-then-succeed contract (see Step 4b — its `fault_count=1` transient seal failure is now retried on the same touch); (b) add a new `TEST(RefWriterRecoveryRetry, ...)` group after the last seal test `WriteAfterSealSelectedAsGreatestSnapshotCommits` (~line 3175, before `TEST(RefWriterListRefs, ...)` at ~3233)

**Interfaces:**
- Consumes: existing `RefWriterTestBackend` with `fault_key_substr` (String) + `fault_count` (int) fields (throws `Poco::TimeoutException` on the next `fault_count` `putIfAbsent` calls whose key contains the substring); `PoolConfig.boot_ms_fn` (injectable `std::function<uint64_t()>` monotonic clock, reaches `CasRefLedger` as `boot_ms_now_fn`); `PoolConfig.cas_request_budget` (a `CasRequestBudget`); `store->setCasRetrySleepForTest(std::function<void(uint64_t)>)`; `ProfileEvents::global_counters[ProfileEvents::CasRefRecoverySealPublished]`.
- Produces: `ProfileEvents::CasRefRecoveryRetries` (new counter); `CasRequestBudget::recovery_retry_budget_ms` (default `120000`), `::recovery_retry_initial_backoff_ms` (default `1000`), `::recovery_retry_max_backoff_ms` (default `30000`); `setCasRetrySleepForTest` now also drives the recovery-loop backoff sleep (in addition to the request controller's).

### Behavioural contract (read before coding)

`ensureRefTableRecovered` currently holds `rt.state_mutex` for the whole `LIST`+replay, releasing it only for the seal `PUT` (which re-acquires before rethrowing). On any `NETWORK_ERROR` — from `backend.list`/`backend.get` (thrown under the lock) or the seal path (`throwCasWriteRetryLater`, thrown with the lock re-acquired) — the exception currently propagates and the load fails permanently. The new outer loop:

1. Runs INSIDE the `recovery_in_progress` single-flight guard (so concurrent same-table callers still wait, unchanged).
2. Catches only `Exception` with `code() == ErrorCodes::NETWORK_ERROR`. Anything else (`CORRUPTED_DATA`, decode errors, `LOGICAL_ERROR`, the `kRefRecoveryMaxRestarts` brake) rethrows immediately — unchanged fail-fast.
3. On a retryable error: if `boot_ms_now_fn() - start >= recovery_retry_budget_ms` OR `!fence_ok_fn()` → rethrow (budget exhausted / mount fence lost; do not retry into a dead mount). Otherwise increment `CasRefRecoveryRetries`, `LOG_WARNING`, release the lock, `recovery_retry_sleep_fn(backoff)`, re-acquire the lock, and re-run recovery from a fresh `LIST`.
4. Backoff is capped-exponential: `min(recovery_retry_max_backoff_ms, recovery_retry_initial_backoff_ms << retry_num)`.
5. The sleep MUST happen with `rt.state_mutex` released (a held mutex would stall `wedgedRefLaneCount`'s whole-store walk — the same reason the seal `PUT` unlocks).
6. A failed attempt may have already assigned `rt.state`/`rt.cleanup_markers`; the next attempt re-runs `replay` and reassigns them before `rt.recovered` is ever set, so partial state from a failed attempt is overwritten cleanly.

- [ ] **Step 1: Register the `CasRefRecoveryRetries` ProfileEvent**

In `src/Common/ProfileEvents.cpp`, immediately after the `CasRefRecoveryRestarts` line (~773) add. NOTE: the descriptions in this file were recently rewritten to a uniform operator-facing style (commit "rewrite ProfileEvents descriptions for operators (remove internal citations)") — no internal identifiers, no code-symbol citations, and a closing "non-zero values indicate …" clause. Match that style exactly (the neighbouring `CasRefRecoveryRestarts` now reads: *"Counts CAS ref-table recovery retries after a snapshot or log vanished during reading; non-zero values indicate concurrent cleanup or backend inconsistency."*). Use:

```cpp
    M(CasRefRecoveryRetries, "Counts CAS ref-table recovery attempts retried after a transient object-store error before the table's load fails; non-zero values indicate transient object-store disruption during table startup.", ValueType::Number) \
```

(Do NOT reword the existing `CasRefRecoveryRestarts` line — it stays as-is; the two are distinct counters: `Restarts` = snapshot/log vanished mid-read, `Retries` = a whole recovery attempt hit a transient object-store error and was re-driven.)

- [ ] **Step 2: Add the extern declarations**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` next to line 35 (`extern const Event CasRefRecoveryRestarts;`) add:

```cpp
    extern const Event CasRefRecoveryRetries;
```

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` next to line 45 (its `extern const Event CasRefRecoveryRestarts;`) add the same line:

```cpp
    extern const Event CasRefRecoveryRetries;
```

- [ ] **Step 3: Add the recovery-retry budget fields**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h`, inside `struct CasRequestBudget` (after `retry_max_backoff_ms` at line 105, before the closing `};`), add:

```cpp
    /// Recovery-level retry (`CasRefLedger::ensureRefTableRecovered`): a whole ref-table recovery
    /// attempt (LIST + snapshot/log GETs + seal PUT) that fails with a transient NETWORK_ERROR is
    /// retried, with capped-exponential backoff, until this total wall-clock budget is spent — then the
    /// error propagates and the table's load fails for this touch (Layer 2's `lazy_load_tables` makes
    /// the NEXT touch retry). This sits ON TOP of the per-request `operation_deadline_ms` envelope
    /// above: one recovery attempt may itself burn ~90s inside a single seal PUT. Independent of the
    /// mount-lease invariants validated in `validateCasRequestBudget` — not part of that inequality set.
    uint64_t recovery_retry_budget_ms = 120000;
    uint64_t recovery_retry_initial_backoff_ms = 1000;
    uint64_t recovery_retry_max_backoff_ms = 30000;
```

- [ ] **Step 4: Add the injectable recovery-sleep member and a backoff helper**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`, find the private members section (near the other `std::function<...>` callbacks, e.g. `fence_ok_fn`). Add:

```cpp
    /// Backoff sleep used by `ensureRefTableRecovered`'s transient-retry loop. Default is an
    /// interruptible slice-sleep (bails early if `fence_ok_fn` drops, e.g. on shutdown/lease loss);
    /// `setCasRetrySleepForTest` overrides it (a unit test injects a clock-advancing no-op).
    std::function<void(uint64_t)> recovery_retry_sleep_fn;
```

- [ ] **Step 4b: Update the existing test that asserts the OLD fail-closed-on-transient behavior**

`TEST(RefWriterRecoverySeal, SealPutFailureFailsRecoveryClosed)` (`gtest_cas_ref_writer.cpp:~2859`) currently seeds a SINGLE transient seal-PUT failure (`fault_count = 1`, an ambiguous/Unresolved outcome → `NETWORK_ERROR`) and asserts that the first `listRefs` THROWS `NETWORK_ERROR`, and that a SECOND manual touch then recovers. That is exactly the behavior Layer 1 intentionally changes: a transient seal failure is now retried WITHIN the same touch. After Layer 1 this test's first `listRefs` will SUCCEED (the retry re-lists and re-seals, `fault_count` having decremented to 0), so its `expectThrowsCode(NETWORK_ERROR, ...)` line will fail. Update it to the new contract — the transient failure is absorbed by the recovery retry and the FIRST touch already recovers and seals:

Replace the body from the fault setup onward (the `backend->fault_count = 1;` line and everything after it, through the end of the test) with:

```cpp
    const RefTxnId seal_id{2, UINT64_MAX};
    backend->fault_key_substr = layout.refSnapshotKey(ns, seal_id);
    backend->fault_count = 1;   /// one transient ambiguous seal PUT -> retried within this same touch

    store->setCasRetrySleepForTest([](uint64_t) {});   /// no real wait on the backoff

    using ProfileEvents::global_counters;
    const auto retries_before = global_counters[ProfileEvents::CasRefRecoveryRetries].load();

    /// The transient seal failure is retried inside recovery, so the FIRST touch already recovers and
    /// seals (previously this threw NETWORK_ERROR and only a second touch re-sealed).
    EXPECT_EQ(store->listRefs(ns).size(), 2u);
    EXPECT_EQ(global_counters[ProfileEvents::CasRefRecoveryRetries].load(), retries_before + 1);
    EXPECT_TRUE(backend->get(layout.refSnapshotKey(ns, seal_id)).has_value())
        << "recovery must have retried past the transient failure and sealed on the first touch";
```

Also update the test's doc comment (the `/// A second touch: ...` block) to describe the new single-touch retry behavior, and rename the test to `SealPutTransientFailureIsRetriedThenSeals` (update the `TEST(RefWriterRecoverySeal, ...)` name; grep the file to confirm no other reference to the old name exists). This test now becomes the fail-closed→retry contract's canonical coverage; the standalone `TransientSealFailureIsRetriedThenSucceeds` added below is complementary (it also asserts the seal-published counter and uses the fake-clock budget path).

- [ ] **Step 5: Write the failing test — retry succeeds after N transient seal failures**

In `src/Disks/tests/gtest_cas_ref_writer.cpp`, after the last `RefWriterRecoverySeal` test `WriteAfterSealSelectedAsGreatestSnapshotCommits` (~line 3175) and before `TEST(RefWriterListRefs, ...)` (~line 3233), add a new `RefWriterRecoveryRetry` group. Reuse the seal fixture helpers already in the file (`seedSealFixtureDeadEpochs`, `seedUncleanPredecessorMount`, `sealTestTinyBudget`, `openPoolWithConfig`):

```cpp
TEST(RefWriterRecoveryRetry, TransientSealFailureIsRetriedThenSucceeds)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/retry_ok"};

    seedSealFixtureDeadEpochs(*backend, layout, ns);
    seedUncleanPredecessorMount(*backend, layout, /*epoch=*/2);

    uint64_t fake_now = 1'000'000;

    PoolConfig config;
    config.server_id = UInt128(1);
    config.mount_lease_ttl_ms = std::chrono::milliseconds(500);
    config.cas_request_budget = sealTestTinyBudget();
    config.cas_request_budget.recovery_retry_budget_ms = 120000;
    config.cas_request_budget.recovery_retry_initial_backoff_ms = 1000;
    config.cas_request_budget.recovery_retry_max_backoff_ms = 30000;
    config.materialization_grace_ms = 1000;
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    config.wait_sleep_fn = [](uint64_t) {};
    auto store = openPoolWithConfig(backend, config);
    ASSERT_TRUE(store);
    ASSERT_EQ(store->liveWriterEpoch(), 3u);

    /// Recovery-loop backoff advances the fake clock so the budget check is deterministic.
    store->setCasRetrySleepForTest([&fake_now](uint64_t ms) { fake_now += ms; });

    /// Fail the seal snapshot PUT twice with a transient (timeout) error; the third attempt lands.
    const RefTxnId seal_id{2, UINT64_MAX};
    backend->fault_key_substr = layout.refSnapshotKey(ns, seal_id);
    backend->fault_count = 2;

    using ProfileEvents::global_counters;
    const auto retries_before = global_counters[ProfileEvents::CasRefRecoveryRetries].load();
    const auto sealed_before = global_counters[ProfileEvents::CasRefRecoverySealPublished].load();

    EXPECT_EQ(store->listRefs(ns).size(), 2u) << "recovery must succeed after retrying past the faults";

    EXPECT_EQ(global_counters[ProfileEvents::CasRefRecoveryRetries].load(), retries_before + 2);
    EXPECT_EQ(global_counters[ProfileEvents::CasRefRecoverySealPublished].load(), sealed_before + 1);
}
```

- [ ] **Step 6: Run the test to verify it FAILS**

Build only the gtest target and run the new test. Redirect to a log; summarize via subagent.

```bash
mkdir -p tmp
ninja -C build unit_tests_dbms > build/build_gtest_cas_ref_writer.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='RefWriterRecoveryRetry.*' > build/test_recovery_retry.log 2>&1
```

Expected: FAIL — before the retry loop exists, the first transient seal fault propagates as `NETWORK_ERROR` out of `listRefs`, so the `EXPECT_EQ(...size(), 2u)` line throws instead of returning 2.

- [ ] **Step 7: Extend `setCasRetrySleepForTest` to also drive the recovery loop**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:116-119`, replace:

```cpp
void CasRefLedger::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    ref_request_controller->setSleepFnForTest(std::move(sleep_fn));
}
```

with:

```cpp
void CasRefLedger::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    ref_request_controller->setSleepFnForTest(sleep_fn);
    recovery_retry_sleep_fn = std::move(sleep_fn);
}
```

- [ ] **Step 8: Initialise `recovery_retry_sleep_fn` to the interruptible default**

In the `CasRefLedger` constructor body (`CasRefLedger.cpp`, the constructor whose signature matches `CasRefLedger.h:39-56`), after the members are set up, assign the production default. Add:

```cpp
    /// Default backoff sleep for the recovery retry loop: sleep in short slices and stop early if the
    /// mount fence drops (shutdown / lease loss), so teardown never waits out a full 30s backoff. This
    /// is backoff against external object-store I/O failure, NOT masking a race — the loop only retries
    /// a transient NETWORK_ERROR.
    recovery_retry_sleep_fn = [this](uint64_t total_ms)
    {
        constexpr uint64_t slice_ms = 200;
        uint64_t slept = 0;
        while (slept < total_ms && fence_ok_fn())
        {
            const uint64_t chunk = std::min(slice_ms, total_ms - slept);
            sleepForMilliseconds(chunk);
            slept += chunk;
        }
    };
```

`sleepForMilliseconds` lives in `<base/sleep.h>` — add `#include <base/sleep.h>` to the `.cpp` includes (it is not yet included in the CA dir). This raw backoff sleep is legitimate under the "never sleep in C++ to fix a race" rule and there is direct precedent: `CasRequestControl.cpp`'s own default inter-attempt backoff `threadSleepMs` (around line 138) carries the comment *"NOT a race-fix sleep: it is deliberate, bounded, ..."* and uses `std::this_thread::sleep_for`. Mirror that justification in the comment here (the slice-loop + `fence_ok_fn()` check additionally makes it interruptible, which `threadSleepMs` is not). If a reviewer or style check flags the raw sleep, point at that precedent.

- [ ] **Step 9: Wrap the recovery body in the retry loop**

In `ensureRefTableRecovered` (`CasRefLedger.cpp`; the function now starts at line ~222), the "recovery body" is the ENTIRE inner restart-on-vanish loop `for (uint64_t attempt = 0; ; ++attempt) { ... }` — it starts at line ~258 and its closing brace is at line ~469 (the loop body includes the `LIST`/`GET`/`replay`/seal `PUT`, then `rt.recovered = true;` at ~441, the tail-counter seeding just after, and finally `break;` at ~468 which exits this inner loop on success). That whole inner `for` loop lives inside the `{ std::unique_lock lock(rt.state_mutex); ... }` scope whose closing brace is at line ~470 (with `enforceRefTableCacheBudget(ns)` at ~476, outside the lock). Wrap the WHOLE inner `for` loop (~258-469) — NOT just its body — in an outer retry loop, keeping the wrap inside the `lock` scope so `lock` is reachable in the catch. Concretely, immediately BEFORE the `for (uint64_t attempt = 0; ...)` line (~258), insert:

```cpp
        const uint64_t recovery_start_ms = boot_ms_now_fn();
        uint64_t recovery_retry_num = 0;
        /// The inner vanish-race brake below (kRefRecoveryMaxRestarts) also surfaces as NETWORK_ERROR
        /// via throwCasWriteRetryLater, but it is a DELIBERATELY terminal "gave up on a pathological
        /// cleanup race" signal, NOT a transient object-store outage. This latch keeps the outer retry
        /// loop from re-driving that brake for the whole budget: when it trips we rethrow immediately.
        bool vanish_brake_tripped = false;
        for (;;)
        {
            try
            {
```

Then, at the inner vanish-race brake (the `if (attempt > kRefRecoveryMaxRestarts)` block at line ~263), set the latch on the line IMMEDIATELY before its `throwCasWriteRetryLater(...)` call:

```cpp
            if (attempt > kRefRecoveryMaxRestarts)
            {
                vanish_brake_tripped = true;
                throwCasWriteRetryLater(fmt::format(
```

(the existing `throwCasWriteRetryLater(fmt::format(...))` call and its message stay exactly as they are — only add the `vanish_brake_tripped = true;` line and the surrounding braces if the `if` was previously brace-less; match the existing brace style).

and immediately AFTER the inner `for` loop's own closing brace (line ~469, i.e. between that `}` and the `}` at ~470 that closes the `lock` scope), insert:

```cpp
                break;   /// recovery succeeded -> exit the outer retry loop
            }
            catch (const Exception & e)
            {
                if (e.code() != ErrorCodes::NETWORK_ERROR || vanish_brake_tripped)
                    throw;   /// not a transient object-store failure (or the terminal vanish-race brake) -- fail fast

                const uint64_t elapsed_ms = boot_ms_now_fn() - recovery_start_ms;
                if (elapsed_ms >= cas_request_budget.recovery_retry_budget_ms || !fence_ok_fn())
                    throw;   /// budget spent, or mount fence lost -- permanent for this touch

                const uint64_t backoff_ms = std::min(
                    cas_request_budget.recovery_retry_max_backoff_ms,
                    cas_request_budget.recovery_retry_initial_backoff_ms << recovery_retry_num);
                ++recovery_retry_num;
                ProfileEvents::increment(ProfileEvents::CasRefRecoveryRetries);
                LOG_WARNING(log, "CAS ref-table recovery for namespace '{}' hit a transient object-store "
                    "error ({}); retry #{} after {}ms backoff (elapsed {}ms / budget {}ms)",
                    ns.string(), e.message(), recovery_retry_num, backoff_ms, elapsed_ms,
                    cas_request_budget.recovery_retry_budget_ms);

                lock.unlock();
                recovery_retry_sleep_fn(backoff_ms);
                lock.lock();
                /// loop: re-run recovery from a fresh LIST (fresh snapshot/log/replay/seal)
            }
        }
```

Notes for the implementer:
- Verify the indentation is consistent with the surrounding code (the body is now one level deeper — reindent the wrapped region or accept the extra indent; match the file's existing style, tabs/spaces as used).
- Confirm `log` is the member logger available in this method (grep `LOG_` uses inside `CasRefLedger.cpp` to confirm the logger identifier; if it is `getLogger(...)` per-call, mirror that).
- Confirm `ErrorCodes::NETWORK_ERROR` is declared in this .cpp's `namespace ErrorCodes` block; if not, add `extern const int NETWORK_ERROR;` there.
- The `SCOPE_EXIT` that clears `recovery_in_progress` still wraps the whole loop (it was declared before the loop) — it runs once on final exit, correct.

- [ ] **Step 10: Run the test to verify it PASSES**

```bash
ninja -C build unit_tests_dbms > build/build_gtest_cas_ref_writer.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='RefWriterRecoveryRetry.*' > build/test_recovery_retry.log 2>&1
```

Expected: PASS (`TransientSealFailureIsRetriedThenSucceeds`), `CasRefRecoveryRetries` bumped by 2. Summarize the log via subagent.

- [ ] **Step 11: Add the budget-exhaustion and non-retryable tests**

Append to `gtest_cas_ref_writer.cpp` in the same group:

```cpp
TEST(RefWriterRecoveryRetry, TransientFailureLongerThanBudgetPropagates)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/retry_budget"};

    seedSealFixtureDeadEpochs(*backend, layout, ns);
    seedUncleanPredecessorMount(*backend, layout, /*epoch=*/2);

    uint64_t fake_now = 1'000'000;

    PoolConfig config;
    config.server_id = UInt128(1);
    config.mount_lease_ttl_ms = std::chrono::milliseconds(500);
    config.cas_request_budget = sealTestTinyBudget();
    config.cas_request_budget.recovery_retry_budget_ms = 5000;   /// small, deterministic
    config.cas_request_budget.recovery_retry_initial_backoff_ms = 1000;
    config.cas_request_budget.recovery_retry_max_backoff_ms = 30000;
    config.materialization_grace_ms = 1000;
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    config.wait_sleep_fn = [](uint64_t) {};
    auto store = openPoolWithConfig(backend, config);
    ASSERT_TRUE(store);
    store->setCasRetrySleepForTest([&fake_now](uint64_t ms) { fake_now += ms; });

    const RefTxnId seal_id{2, UINT64_MAX};
    backend->fault_key_substr = layout.refSnapshotKey(ns, seal_id);
    backend->fault_count = 1000;   /// never stops failing within the budget

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->listRefs(ns); });
}

TEST(RefWriterRecoveryRetry, NonNetworkErrorIsNotRetried)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/retry_fatal"};

    seedSealFixtureDeadEpochs(*backend, layout, ns);
    seedUncleanPredecessorMount(*backend, layout, /*epoch=*/2);

    uint64_t fake_now = 1'000'000;

    PoolConfig config;
    config.server_id = UInt128(1);
    config.mount_lease_ttl_ms = std::chrono::milliseconds(500);
    config.cas_request_budget = sealTestTinyBudget();
    config.materialization_grace_ms = 1000;
    config.boot_ms_fn = [&fake_now] { return fake_now; };
    config.wait_sleep_fn = [](uint64_t) {};
    auto store = openPoolWithConfig(backend, config);
    ASSERT_TRUE(store);

    size_t sleep_calls = 0;
    store->setCasRetrySleepForTest([&sleep_calls](uint64_t) { ++sleep_calls; });

    /// The seal snapshot key already holds a DIFFERENT valid body -> putIfAbsentControlled throws
    /// CORRUPTED_DATA (a real cross-process conflict), which must NOT be retried.
    const RefTxnId seal_id{2, UINT64_MAX};
    RefTableSnapshot foreign;
    foreign.snapshot_id = seal_id;
    foreign.sealed_from = RefTxnId{2, 1};
    backend->putIfAbsent(layout.refSnapshotKey(ns, seal_id),
        sealObject(FormatId::RefSnapshot, encodeRefTableSnapshot(foreign)));

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { store->listRefs(ns); });
    EXPECT_EQ(sleep_calls, 0u) << "a non-transient error must fail fast with zero backoff sleeps";
}
```

Implementer note: confirm `expectThrowsCode`, `sealObject`, `encodeRefTableSnapshot`, `RefTableSnapshot` construction, and the foreign-seed approach match existing usages in this file (the `RefWriterRecoverySeal` group at ~2860 already seeds a foreign seal to prove the CORRUPTED_DATA path — mirror that exact seeding if the direct `putIfAbsent` shape differs). Adjust field names to the real `RefTableSnapshot` definition if needed.

Also append this test, which proves the outer retry loop does NOT re-drive the deliberately-terminal vanish-race brake (the `vanish_brake_tripped` latch). Model the fixture on `RefWriterRecovery.RestartOnVanishConvergesOnNewerSnapshot` (line ~501) — reuse its exact snapshot-seeding setup; the only change is re-arming the vanish so it fires more than `kRefRecoveryMaxRestarts` (3) times:

```cpp
TEST(RefWriterRecoveryRetry, VanishBrakeStaysTerminalNotRetried)
{
    auto backend = std::make_shared<RefWriterTestBackend>();
    const Layout layout("p");
    const RootNamespace ns{"srv1/retry_vanish"};

    /// Seed the same snapshot+tail fixture RestartOnVanishConvergesOnNewerSnapshot uses, so a single
    /// greatest snapshot key is selected on every recovery LIST. (Copy that test's seeding verbatim;
    /// `snap_x` below is that greatest-snapshot txn id.)
    const RefTxnId snap_x = seedRecoverableSnapshotFixture(*backend, layout, ns);   // <-- match the real helper

    PoolConfig config;
    config.server_id = UInt128(1);
    config.cas_request_budget = sealTestTinyBudget();
    config.cas_request_budget.recovery_retry_budget_ms = 120000;
    config.materialization_grace_ms = 1000;
    config.wait_sleep_fn = [](uint64_t) {};
    auto store = openPoolWithConfig(backend, config);
    ASSERT_TRUE(store);

    size_t sleep_calls = 0;
    store->setCasRetrySleepForTest([&sleep_calls](uint64_t) { ++sleep_calls; });

    /// Re-arm the vanish so the SAME selected snapshot key keeps disappearing between LIST and GET,
    /// past the kRefRecoveryMaxRestarts (3) inner brake.
    const String vkey = layout.refSnapshotKey(ns, snap_x);
    int fires = 0;
    std::function<void()> rearm = [&]()
    {
        if (++fires < 5)
        {
            backend->vanish_once_keys.insert(vkey);
            backend->on_vanish_fire = rearm;
        }
    };
    backend->vanish_once_keys.insert(vkey);
    backend->on_vanish_fire = rearm;

    using ProfileEvents::global_counters;
    const auto retries_before = global_counters[ProfileEvents::CasRefRecoveryRetries].load();

    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->listRefs(ns); });

    EXPECT_EQ(global_counters[ProfileEvents::CasRefRecoveryRetries].load(), retries_before)
        << "the vanish-race brake is terminal; the outer transient-retry loop must NOT re-drive it";
    EXPECT_EQ(sleep_calls, 0u) << "no backoff sleep for the terminal vanish brake";
}
```

Implementer note: the helper name `seedRecoverableSnapshotFixture` is illustrative — use whatever seeding `RestartOnVanishConvergesOnNewerSnapshot` actually performs inline (it may seed directly rather than via a helper); the essential requirement is a fixture where one greatest-snapshot key is re-selected each LIST so the re-armed vanish trips the inner brake. If the harness cannot sustain re-armed vanishes for a reason discovered during implementation, STOP and report — do not silently drop this test, as it is the only coverage of the latch.

- [ ] **Step 12: Run all four tests to verify PASS**

```bash
ninja -C build unit_tests_dbms > build/build_gtest_cas_ref_writer.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='RefWriterRecoveryRetry.*' > build/test_recovery_retry.log 2>&1
```

Expected: 4/4 PASS (`TransientSealFailureIsRetriedThenSucceeds`, `TransientFailureLongerThanBudgetPropagates`, `NonNetworkErrorIsNotRetried`, `VanishBrakeStaysTerminalNotRetried`). Summarize via subagent.

- [ ] **Step 13: Run the full CA gtest gate to check for regressions**

```bash
./build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:RefWriter*:RefLedger*' > build/test_ca_gate.log 2>&1
```

Expected: all green. The ONLY existing test whose behavior changes is `SealPutFailureFailsRecoveryClosed` (renamed `SealPutTransientFailureIsRetriedThenSeals` in Step 4b) — it now asserts the new single-touch retry contract and must pass in its updated form. The two CORRUPTED_DATA seal tests (`SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery`, `SealPutThrowsMidFlightSecondParkedCallerDoesNotHang`) are unaffected because Layer 1 only retries `NETWORK_ERROR`, never `CORRUPTED_DATA` — confirm they still pass unchanged. Summarize via subagent; if any OTHER test went red, that is a real regression — fix before committing.

- [ ] **Step 14: Commit**

```bash
git add src/Common/ProfileEvents.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: retry ref-table recovery on transient object-store errors before failing the load

Layer 1 of the stuck-table-load fix. A transient NETWORK_ERROR during
CasRefLedger::ensureRefTableRecovered (LIST/GET/seal-PUT) is now retried with
capped-exponential backoff up to cas_request_budget.recovery_retry_budget_ms
(default 120s) instead of propagating and failing the table's async load
permanently. Non-transient errors (CORRUPTED_DATA etc.) still fail fast. The
backoff sleep runs with state_mutex released and is interruptible via the mount
fence. New ProfileEvent CasRefRecoveryRetries."
```

---

## Task 2: Layer 2 — put the soak's CAS table in a `lazy_load_tables` database

**Files:**
- Modify: `utils/ca-soak/soak/cluster.py:235-243` (add a `database` kwarg to `Node`, thread it into `query`) and the `Cluster` constructor (`:485`) to pass it through
- Modify: `utils/ca-soak/soak/run.py:1102-1123` (`setup_cluster_and_table`) and `:1520-1525` (the phase-1 self-check path) to create the lazy database and point the workload cluster at it
- Create: `docs/en/... ` — NO. CAS deployment doc lives under `docs/superpowers/cas/`; add a note to the existing operational doc.
- Modify: `docs/superpowers/cas/08-testing-and-soak.md` (add a short "lazy_load_tables for CAS databases" subsection)
- Test: `utils/ca-soak/tests/test_workload_sql.py` (the soak's own pytest; add an assertion that the DDL path creates the lazy database)

**Interfaces:**
- Consumes: `Node.query(sql, timeout, settings)` builds its URL as `self.url` + `"?" + urlencode(settings)`; `Cluster` builds `Node`s internally; `setup_cluster_and_table` runs `CREATE`/`DROP` on `cluster.nodes()`.
- Produces: `Node(database=...)` (default `"default"`); the soak workload cluster addresses database `ca_soak`; the CAS table `ca_stress` lives in `ca_soak ENGINE = Atomic SETTINGS lazy_load_tables = 1`.

### Design note (read before coding)

The table name stays `ca_stress` (bare) everywhere in the harness — all SQL keeps working — by setting the *connection's default database* to `ca_soak`, rather than qualifying every call site. `system.parts WHERE table='ca_stress'` still matches (that filters the table-name column; the database is orthogonal). The zk path `/clickhouse/tables/ca_stress` is unchanged (the DDL template's `{table}` is still the bare name). Only the workload `Cluster` in `run.py` gets the non-default database; the scenario suite (separate entrypoint) is untouched.

- [ ] **Step 1: Write the failing test**

In `utils/ca-soak/tests/test_workload_sql.py`, add (adjust imports to the file's existing style):

```python
def test_lazy_database_ddl_is_emitted(monkeypatch):
    # setup_cluster_and_table must create the CAS table inside a lazy_load_tables database so a
    # transient S3 error at load is retried on next access instead of stranding the table.
    from soak import run
    captured = []

    class FakeNode:
        container = "ch1"
        def command(self, sql, timeout=None):
            captured.append(sql)
        def scalar(self, sql):
            return "0"

    class FakeCluster:
        node1 = FakeNode()
        def __init__(self, *a, **k): pass
        def nodes(self):
            return [FakeNode()]

    monkeypatch.setattr(run, "Cluster", FakeCluster)
    run.setup_cluster_and_table(seed=1, phase="test", ops=1, workers=1, checkpoint_every=1)

    joined = "\n".join(captured)
    assert "CREATE DATABASE IF NOT EXISTS ca_soak" in joined
    assert "lazy_load_tables = 1" in joined
```

- [ ] **Step 2: Run the test to verify it FAILS**

```bash
cd utils/ca-soak && python -m pytest tests/test_workload_sql.py::test_lazy_database_ddl_is_emitted -q > /home/mfilimonov/workspace/ClickHouse/master/build/test_soak_lazy_ddl.log 2>&1; cd -
```

Expected: FAIL — no `CREATE DATABASE ... lazy_load_tables` is emitted yet.

- [ ] **Step 3: Add the `database` kwarg to `Node` and thread it into `query`**

In `utils/ca-soak/soak/cluster.py`, change the `Node.__init__` (line 235) to accept and store `database`, defaulting to `"default"`:

```python
    def __init__(self, host: str, port: int, container: str | None = None, timeout: float = 300.0,
                 database: str = "default"):
        self.host = host
        self.port = port
        self.container = container
        self.timeout = timeout
        self.database = database
```

Change `query` (line 245) so the default database is always sent as a URL param, merged with any per-call settings:

```python
    def query(self, sql: str, timeout: float | None = None, settings: dict | None = None) -> str:
        params = {"database": self.database}
        if settings:
            params.update(settings)
        url = self.url + "?" + urllib.parse.urlencode(params)
        data = sql.encode("utf-8")
        req = urllib.request.Request(url, data=data, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=timeout or self.timeout) as resp:
                return resp.read().decode("utf-8").rstrip("\n")
        except urllib.error.HTTPError as e:
            body = ""
            try:
                body = e.read().decode("utf-8", "replace")
            except Exception:
                pass
            raise QueryError(self, e.code, body, sql) from e
```

Implementer note: `CREATE DATABASE ca_soak` must be issued while the connection's default database still resolves (a `database=ca_soak` URL param on a server where `ca_soak` does not yet exist makes the query fail with `UNKNOWN_DATABASE` before it runs). Therefore the `CREATE DATABASE` statement itself must be sent on a node whose `database` is `"default"`. Handle this in Step 5 by creating the database over a default-database node, then creating the table over the `ca_soak` node.

- [ ] **Step 4: Thread `database` through `Cluster`**

In `utils/ca-soak/soak/cluster.py`, find where `Cluster.__init__` (line ~485) constructs its `Node`s (grep `Node(` inside the class). Add a `database: str = "default"` kwarg to `Cluster.__init__` and pass it to every `Node(...)` it builds. Do NOT change the default — only `run.py` will pass `database="ca_soak"`.

- [ ] **Step 5: Create the lazy database in `setup_cluster_and_table`**

In `utils/ca-soak/soak/run.py`, add a module constant near `TABLE` (line 70):

```python
DB = "ca_soak"   # dedicated Atomic database with lazy_load_tables=1 so a transient S3 error at CAS
                 # table load is retried on next access instead of stranding the table (see
                 # docs/superpowers/specs/2026-07-20-cas-table-load-stuck-asyncloader-design.md)
```

In `setup_cluster_and_table` (line 1102), change the bring-up so it (a) builds the workload cluster pointed at `DB`, (b) creates the lazy database over a default-database handle first, then (c) drops+creates the table over the `DB`-scoped nodes. Replace the body from `cluster = Cluster()` through `return cluster, model, base_time` with:

```python
    cluster = Cluster(database=DB)
    # CREATE DATABASE must run against the default database (ca_soak does not exist yet on a fresh
    # stand); a ca_soak-scoped connection would fail UNKNOWN_DATABASE first. Use a throwaway
    # default-scoped cluster just for the CREATE DATABASE.
    bootstrap = Cluster()
    for node in bootstrap.nodes():
        node.command(f"CREATE DATABASE IF NOT EXISTS {DB} ENGINE = Atomic SETTINGS lazy_load_tables = 1")
    base_time = int(cluster.node1.scalar("SELECT toUnixTimestamp(now())")) - 60
    log(f"base_time={base_time} (needed for replay) seed={seed} phase={phase} "
        f"ops={ops} workers={workers} checkpoint_every={checkpoint_every}")
    model = Model(seed, base_time=base_time)
    ddl = DDL_TEMPLATE.format(table=TABLE)
    for node in cluster.nodes():
        t0 = time.monotonic()
        node.command(f"DROP TABLE IF EXISTS {TABLE} SYNC", timeout=900)
        dt = time.monotonic() - t0
        if dt > 30:
            log(f"setup DROP {TABLE} SYNC on {node.container} took {dt:.1f}s (large CA/S3 table)")
    for node in cluster.nodes():
        node.command(ddl)
    log(f"created {DB}.{TABLE} on both replicas (lazy_load_tables=1)")
    return cluster, model, base_time
```

- [ ] **Step 6: Apply the same lazy-DB bring-up to the phase-1 self-check path**

In `utils/ca-soak/soak/run.py` near line 1520-1525 (the second `ddl = DDL_TEMPLATE.format(table=TABLE)` site — the phase-1 compressed self-check), locate how it builds its cluster and creates the table. Apply the identical pattern: build the cluster with `database=DB`, create the lazy database over a default-scoped `bootstrap = Cluster()` first, then create the table. (Read the surrounding function to match its exact structure; if it already calls `setup_cluster_and_table`, no change is needed here — verify by reading lines ~1500-1530 and only edit if it constructs its own `Cluster()`/DDL inline.)

- [ ] **Step 7: Run the test to verify it PASSES**

```bash
cd utils/ca-soak && python -m pytest tests/test_workload_sql.py::test_lazy_database_ddl_is_emitted -q > /home/mfilimonov/workspace/ClickHouse/master/build/test_soak_lazy_ddl.log 2>&1; cd -
```

Expected: PASS. Also run the whole soak unit suite to catch fallout from the `Node.query`/`Cluster` change:

```bash
cd utils/ca-soak && python -m pytest tests/ -q > /home/mfilimonov/workspace/ClickHouse/master/build/test_soak_all.log 2>&1; cd -
```

Expected: no new failures. Summarize via subagent; fix any test that hard-coded the old single-`?` URL shape.

- [ ] **Step 8: Document the operational recommendation**

In `docs/superpowers/cas/08-testing-and-soak.md`, add a short subsection (with an explicit `{#lazy-load-cas-databases}` anchor per repo docs rule) explaining: databases holding CAS tables should be created with `lazy_load_tables = 1` so a transient object-store error during table startup surfaces as a per-query error and is retried on the next access, instead of stranding the table in a permanently-`FAILED` `AsyncLoader` job until server restart; note the caveat that a lazily-loaded table does not start replication/merges until first access; cross-reference the spec and the BACKLOG entry.

- [ ] **Step 9: Commit**

```bash
git add utils/ca-soak/soak/cluster.py utils/ca-soak/soak/run.py \
  utils/ca-soak/tests/test_workload_sql.py docs/superpowers/cas/08-testing-and-soak.md
git commit -m "ca-soak: host the CAS stress table in a lazy_load_tables database

Layer 2 of the stuck-table-load fix (configuration only, no C++). The soak's
ca_stress table now lives in a dedicated 'ca_soak' Atomic database created with
lazy_load_tables=1, so a transient S3 error during table startup is retried on
next access (StorageTableProxy) instead of caching a permanent AsyncLoader
FAILED job. The table name stays bare; the workload connection's default
database is set to ca_soak. Documents the same recommendation for CAS
deployments."
```

---

## Task 3: Layer 2 verification — integration test for self-heal after S3 recovery

**Files:**
- Create: `tests/integration/test_cas_lazy_load_recovery/__init__.py` (empty)
- Create: `tests/integration/test_cas_lazy_load_recovery/test.py`
- Create: `tests/integration/test_cas_lazy_load_recovery/configs/storage_conf.xml` (copy the CAS storage policy from `tests/integration/test_cas_insert_fault_recovery/configs/storage_conf.xml`)
- Create: `tests/integration/test_cas_lazy_load_recovery/configs/server_root_id_node1.xml` (copy the pattern from the same sibling test)

**Interfaces:**
- Consumes: `helpers.cluster.ClickHouseCluster` with `with_rustfs=True`, `with_zookeeper=True`, `stay_alive=True`; `node.query(...)`, `node.stop_clickhouse()`, `node.start_clickhouse()`; `cluster.pause_container("rustfs1")` / `cluster.unpause_container("rustfs1")` (verify the exact rustfs service name and the unpause method name by reading `helpers/cluster.py` around `pause_container` at line 4613 and the rustfs fields at 728-738).
- Produces: a passing integration test `test_cas_lazy_load_recovery` proving the v11 scenario self-heals.

### Design note

With `lazy_load_tables=1` the table attaches as a `StorageTableProxy` whose real storage is built on first access; the construction closure is discarded only on success. So: stop CH, start CH (table is now a lazy proxy, not yet built), pause rustfs, first `SELECT` fails with a *per-query* error (the proxy retries next time), unpause rustfs, next `SELECT` succeeds — no restart, no `DETACH`. The key assertion is that the failing `SELECT` error is NOT the permanently-cached `ASYNC_LOAD_WAIT_FAILED` and that a later `SELECT` succeeds with no intervening admin action.

- [ ] **Step 1: Write the test (single node is enough)**

Create `tests/integration/test_cas_lazy_load_recovery/test.py`:

```python
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "content_addressed_shared"


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml"],
        macros={"replica": "node1"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def _create(node):
    node.query("CREATE DATABASE IF NOT EXISTS lazy_db ENGINE = Atomic SETTINGS lazy_load_tables = 1")
    node.query(
        "CREATE TABLE IF NOT EXISTS lazy_db.t "
        "(k UInt64, v UInt64) "
        "ENGINE = ReplicatedMergeTree('/clickhouse/tables/lazy_t', '{replica}') "
        "ORDER BY k SETTINGS storage_policy = '%s', min_bytes_for_wide_part = 0" % STORAGE_POLICY
    )


def test_lazy_cas_table_self_heals_after_s3_recovery(start_cluster):
    node = cluster.instances["node1"]
    _create(node)
    node.query("INSERT INTO lazy_db.t SELECT number, number FROM numbers(100)")
    assert node.query("SELECT count() FROM lazy_db.t").strip() == "100"

    # Restart so the table re-attaches as a lazy proxy (not yet constructed).
    node.restart_clickhouse()

    # Make S3 unreachable, then force the first access -> per-query failure, NOT a stuck load.
    cluster.pause_container("rustfs1")
    try:
        failed = False
        try:
            node.query("SELECT count() FROM lazy_db.t")
        except Exception as e:
            failed = True
            msg = str(e)
            assert "ASYNC_LOAD_WAIT_FAILED" not in msg, (
                "lazy load must fail per-query, not cache a permanent AsyncLoader failure: " + msg
            )
        assert failed, "SELECT should have failed while S3 was unreachable"

        # Under lazy loading there is no permanently-FAILED load job to block on, so DETACH/ATTACH
        # work even while S3 is down -- this is exactly the catch-22 the lazy path sidesteps (with a
        # non-lazy database, DETACH would itself hang on the failed load job).
        node.query("DETACH TABLE lazy_db.t")
        node.query("ATTACH TABLE lazy_db.t")
    finally:
        cluster.unpause_container("rustfs1")

    # No restart, no DETACH: the very next access must retry and succeed.
    deadline = time.time() + 60
    last = None
    while time.time() < deadline:
        try:
            last = node.query("SELECT count() FROM lazy_db.t").strip()
        except Exception as e:
            last = "err: " + str(e)
        if last == "100":
            break
        time.sleep(2)
    assert last == "100", "table must self-heal on next access after S3 returns (last=%r)" % last
```

Implementer notes:
- Confirm `node.restart_clickhouse()` exists (grep `def restart_clickhouse` in `helpers/cluster.py`); if not, use `node.stop_clickhouse()` then `node.start_clickhouse()`.
- Confirm `cluster.pause_container` accepts the rustfs service name `"rustfs1"` (it may require the compose service name — read `pause_container` at 4613 and `self.rustfs_host` at 732). If `pause_container` cannot target rustfs, substitute the documented mechanism other CAS tests use to take S3 down (grep the CAS integration tests for `pause_container`/`stop`/network-block usage) and mirror it exactly.
- Confirm `unpause_container` is the real method name (grep it); if the API is `pause`/`unpause` on a docker handle, adapt.

- [ ] **Step 2: Create the config files**

Copy from the sibling test and trim to a single node:

```bash
mkdir -p tests/integration/test_cas_lazy_load_recovery/configs
: > tests/integration/test_cas_lazy_load_recovery/__init__.py
cp tests/integration/test_cas_insert_fault_recovery/configs/storage_conf.xml \
   tests/integration/test_cas_lazy_load_recovery/configs/storage_conf.xml
cp tests/integration/test_cas_insert_fault_recovery/configs/server_root_id_node1.xml \
   tests/integration/test_cas_lazy_load_recovery/configs/server_root_id_node1.xml
```

Verify `storage_conf.xml` defines a policy named `content_addressed_shared` (read it); if the sibling uses a different policy name, update `STORAGE_POLICY` in the test to match.

- [ ] **Step 3: Run the test to verify it PASSES**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master && \
python -m ci.praktika run "integration" --test test_cas_lazy_load_recovery > build/test_integration_lazy.log 2>&1
```

Expected: PASS. Summarize via subagent. If it fails because lazy loading does NOT deliver retry-on-touch for the `ReplicatedMergeTree`+CAS path (the risk called out in the spec), STOP and report to the user with the exact failure — that is a genuine finding, not a test bug.

- [ ] **Step 4: Commit**

```bash
git add tests/integration/test_cas_lazy_load_recovery/
git commit -m "ca-soak: integration test — lazy CAS table self-heals after transient S3 outage

Layer 2 verification. Reproduces the soak v11 scenario on a single with_rustfs
node: restart so the CAS table re-attaches as a lazy StorageTableProxy, pause
rustfs, assert the first SELECT fails per-query (NOT a cached
ASYNC_LOAD_WAIT_FAILED), unpause rustfs, assert the next SELECT self-heals with
no restart or DETACH."
```

---

## Task 4: Deliverable 3 — draft upstream issue

**Files:**
- Create: `docs/superpowers/reports/2026-07-20-upstream-issue-draft-asyncloader-stuck-table.md`

**Interfaces:**
- Consumes: the master-source facts already gathered (references below). Produces: a self-contained English draft, no CAS specifics, for the user to review and file manually.

- [ ] **Step 1: Write the draft**

Create `docs/superpowers/reports/2026-07-20-upstream-issue-draft-asyncloader-stuck-table.md` with a self-contained issue draft. It MUST contain, in English, with no CAS-specific content:

- Title: `Table whose async load job failed is permanently stuck until server restart — even DETACH TABLE cannot recover it`.
- Summary: with `async_load_databases=true` (default since 25.2, PR #74772), if a table's async `load table` job throws — e.g. an engine whose constructor does object-store I/O and hits a transient error — `AsyncLoader` marks the job `FAILED` terminally, and every later access rethrows the cached exception.
- Minimal CAS-free repro sketch: a `MergeTree`/`ReplicatedMergeTree` on an S3-backed disk while the S3 endpoint is briefly unreachable during startup/`ATTACH`; the table load fails and never retries.
- Root-cause chain with master `file:line`: `DatabaseWithOwnTablesBase::tryGetTable` (`src/Databases/DatabasesCommon.cpp:430`) → `waitTableStarted` (`src/Databases/DatabaseOrdinary.cpp:629`) → `waitLoad` → `ASYNC_LOAD_WAIT_FAILED` (`src/Common/AsyncLoader.cpp:473`). `DETACH` cannot recover it: `InterpreterDropQuery` resolves the table via `DatabaseCatalog::getDatabaseAndTable`, which calls `waitTableStarted` (`src/Interpreters/DatabaseCatalog.cpp:434-435`, comment "Wait for table to be started because we are going to return StoragePtr") and throws before reaching `DatabaseOrdinary::detachTableUnlocked` — where the state-erasing `eraseAsyncLoadState` actually lives. Catch-22. (Re-verified 2026-07-20 against the current tip after an upstream `antalya-26.6` merge that DID touch `AsyncLoader` — none of those changes add a FAILED-job retry or break this chain.)
- What upstream already has: `AsyncLoader` terminality is by design (`AsyncLoader.h` contract); the only retry-on-touch path is the opt-in `lazy_load_tables` (PR #96283, 26.2) via `StorageTableProxy`.
- Related issues: #88934, #67521.
- Suggested directions (either/both): (1) let `DETACH TABLE` of a load-`FAILED` table bypass `waitTableStarted` so it reaches `eraseAsyncLoadState`; (2) offer an explicit re-trigger (SYSTEM verb, or retry-on-touch) for a `FAILED` table load.
- A closing line stating this is a draft prepared for manual review/filing, not yet filed.

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/reports/2026-07-20-upstream-issue-draft-asyncloader-stuck-table.md
git commit -m "docs: draft upstream issue for the AsyncLoader stuck-FAILED-table / DETACH catch-22

Deliverable 3 of the stuck-table-load fix. A CAS-free description of the generic
ClickHouse problem: a table whose async load job fails is permanently stuck
until restart, and DETACH cannot recover it because table resolution waits on
the failed load job before reaching the state-erasing detach path. Draft only;
not filed."
```

---

## Notes for the executor

- Tasks are ordered by independence: Task 1 (C++, self-contained) and Task 4 (doc) have no cross-dependency; Task 3 verifies the mechanism Task 2 configures, so run Task 2 before Task 3. Any order that keeps 2-before-3 is fine.
- After Task 1, before starting Task 2, confirm the CA gtest gate (Step 13) is green — that is the guard that the retry wrap did not disturb existing recovery/seal behaviour.
- The 5h soak rerun (v12) that the spec's acceptance criteria mention is a post-merge validation step, not a task in this plan (it needs the built binary and a clean stand). Flag it to the user after all four tasks land.
