# CAS Lease / View-Sync Decouple Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the S3-heavy retired-view refresh off the mount-lease renewal thread so the lease renewal cadence is independent of S3 latency and can no longer expire on a live server.

**Architecture:** Split the merged mount heartbeat's two jobs. The renewal path (`MountLeaseKeeper::renewOnce` → `prepareRenew`) reads only cheap in-memory values — including the *currently installed* GC round via a new `Store::observedGcRound()`. A new Store-owned background poller thread runs the S3 view refresh (`Store::syncRetiredView`, today's `refreshViewForBeat` body, renamed) on its own cadence. The overloaded `beat` vocabulary on the view side is renamed to `syncRetiredView` / `RetiredViewAdvance`; the lease side keeps its (correct) heartbeat names.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core`), GoogleTest (`unit_tests_dbms`), CMake/Ninja.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-06-cas-lease-view-sync-decouple-design.md`. This plan is P3.1 Task 5.
- Branch: work on `cas-gc-rebuild` (where P3.1 Tasks 1–4 already live); add new commits, never rebase/amend, never commit to `master`.
- No new `ErrorCodes` numbers (fork-merge hygiene) — this plan introduces none.
- Allman braces (opening brace on its own line); enforced by the CI style check.
- Never use `sleep` in C++ to fix a race. Bounded *condition* waits used as explicit liveness assertions on an intentionally-running thread are acceptable (the codebase already does this in `CasStoreBeat.DrainBlocksAckWhileMutationInFlight`).
- CAS is pre-release with no persisted event data — the event rename needs zero compatibility scaffolding.
- Build in `build/` (SANITIZE=OFF). `build_asan` turns any `LOGICAL_ERROR` throw into an unconditional `abort()`, which breaks the deliberate-throw Cas tests — do not use it here.
- Build command: `ninja -C build unit_tests_dbms > build/build_task<N>.log 2>&1` (NO `-j`, NO `nproc`; let ninja decide). Always redirect to the log and have a subagent summarize it.
- Test command: `build/src/unit_tests_dbms --gtest_filter='Cas*' > build/test_task<N>.log 2>&1`. Redirect to a per-task log; have a subagent summarize it.
- Naming (verbatim from the spec §8):
  - `Store::refreshViewForBeat()` → `Store::syncRetiredView()`
  - new `Store::observedGcRound()` — cheap installed-round reader for the renewal path
  - new `Store::startRetiredViewSync` / `stopRetiredViewSync` / `retiredViewSyncLoop` / `retired_view_sync_thread`
  - `CasEventType::MountBeat` → `CasEventType::RetiredViewAdvance`
  - event string `"mount_beat"` → `"retired_view_advance"`
  - the lease side is UNCHANGED: `MountLeaseKeeper`, `renewOnce`, `mount_renew_period`, `SingleWriterSlot`.

---

## File Structure

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` — method decls + comments + new syncer thread members.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` — the rename, `observedGcRound`, `renewWatermarkOnce` redefinition, keeper-ctor rewiring, remount prime, and the syncer thread (start/stop/loop) wired into `open`/`tryRemountOnce`/`~Store`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h` / `CasEvent.cpp` — the event enum + string rename.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h` / `CasServerRoot.cpp` — doc-comment rewording only (the `observed_round_fn` semantics change; no signature change).
- `src/Disks/tests/gtest_cas_store.cpp` — rename the direct `refreshViewForBeat` caller and the `MountBeat` assertions; add the decouple tests.
- Docs: `docs/superpowers/cas/08-testing-and-soak.md`, `docs/superpowers/cas/ROADMAP.md`, `docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md` — `mount_beat` prose → `retired_view_advance`.

---

## Task 1: Rename the view side (`refreshViewForBeat`→`syncRetiredView`, `MountBeat`→`RetiredViewAdvance`)

Pure rename — no behavior change. The renewal wiring still calls the (renamed) view refresh via `observed_round_fn`; that is untouched here and rewired in Task 2. After this task the full `Cas*` suite passes exactly as before, just under the new names.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h:381-390` (decl + doc comment), `CasStore.cpp:270`, `CasStore.cpp:504`, `CasStore.cpp:580-646` (definition + internal log/event strings), `CasStore.cpp:375-378` (the `tryRemountOnce` doc comment mentioning `refreshViewForBeat`)
- Modify: `CasEvent.h:26`, `CasEvent.cpp:63`
- Modify: `src/Disks/tests/gtest_cas_store.cpp:1644-1676` (event assertions + test name) and `:1804` (direct call)
- Modify docs: `docs/superpowers/cas/08-testing-and-soak.md`, `docs/superpowers/cas/ROADMAP.md`, `docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md`

**Interfaces:**
- Consumes: nothing new.
- Produces: `uint64_t Store::syncRetiredView();` (was `refreshViewForBeat`, identical body/semantics/return); `CasEventType::RetiredViewAdvance` (was `MountBeat`) with string `"retired_view_advance"` (was `"mount_beat"`).

- [ ] **Step 1: Rename the event enum and string**

In `CasEvent.h:26`, change `MountBeat` to `RetiredViewAdvance`:

```cpp
    GateRevalidate, GateResurrect, WatermarkRenew, RetiredViewAdvance, MountRemount,
```

In `CasEvent.cpp:63`, change the case:

```cpp
        case CasEventType::RetiredViewAdvance:    return "retired_view_advance";
```

- [ ] **Step 2: Rename the method declaration + doc comment in `CasStore.h`**

Replace the declaration block at `CasStore.h:381-390` (the `refreshViewForBeat` doc + decl). Keep the doc content; only the name and the "beat" wording change:

```cpp
    /// Retired-view sync (spec 2026-07-06-cas-lease-view-sync-decouple; formerly the ack-floor
    /// "beat"): probe `gc/state`; when the published round advanced past the installed view, load the
    /// retired view and install it under the exclusive side of `view_gate` — the DRAIN: it waits out
    /// every in-flight `mutateShard` (each holds the shared side for its whole call, gate evaluation
    /// through CAS response). Returns the round the view is installed at. Any read failure leaves the
    /// view and the returned round UNCHANGED (fail-closed for the ack: never claim a view that was not
    /// actually loaded). Runs on the dedicated retired-view syncer thread (`retiredViewSyncLoop`) and,
    /// synchronously, at open/remount and in tests. It is NOT on the lease-renewal path — the renewal
    /// advertises the already-installed round via `observedGcRound()`.
    uint64_t syncRetiredView();
```

- [ ] **Step 3: Rename the definition in `CasStore.cpp`**

At `CasStore.cpp:580`, change the signature line:

```cpp
uint64_t Store::syncRetiredView()
```

Inside the body, update the three log-message prefixes and the event emission (leave all logic identical). The GET-probe log (`~593`), the undecodable log (`~607`), and the refresh-failed log (`~627`) currently read `"CAS beat: ..."` — change each to `"CAS retired-view sync: ..."`. In the event block (`~635-643`), change the type and reason:

```cpp
            e.type = CasEventType::RetiredViewAdvance;
            e.round = retire_view.round();
            e.outcome = "ok";
            e.reason = "retired-view sync installed a newer view; observed_gc_round advances with it";
```

- [ ] **Step 4: Rename the two internal call sites + the remount doc comment**

At `CasStore.cpp:270` and `CasStore.cpp:504`, the keeper-construction lambdas currently read `[raw] { return raw->refreshViewForBeat(); }`. Rename the method call (the rewiring to `observedGcRound` happens in Task 2):

```cpp
                [raw] { return raw->minActive(); }, [raw] { return raw->syncRetiredView(); },
```

At `CasStore.h:375-378`, the `tryRemountOnce` doc comment says "loads the retired view through `refreshViewForBeat`" — change that phrase to "through `syncRetiredView`".

- [ ] **Step 5: Update the tests that reference the old names**

In `src/Disks/tests/gtest_cas_store.cpp`:
- Rename the test at `:1644` from `ViewAdvanceEmitsMountBeatEvent` to `ViewAdvanceEmitsRetiredViewAdvanceEvent`; in its body change both `CasEventType::MountBeat` references (`:1661`, `:1674`) to `CasEventType::RetiredViewAdvance`, and the comment/message wording from `mount_beat` to `retired_view_advance`.
- At `:1804`, change `store->refreshViewForBeat()` to `store->syncRetiredView()`.

- [ ] **Step 6: Update the `mount_beat` prose in docs**

In each of `docs/superpowers/cas/08-testing-and-soak.md`, `docs/superpowers/cas/ROADMAP.md`, and `docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md`, replace the literal `` `mount_beat` `` mentions with `` `retired_view_advance` `` (prose only — these describe the event this task renames).

- [ ] **Step 7: Build**

Run: `ninja -C build unit_tests_dbms > build/build_task1.log 2>&1`
Expected: build succeeds. Have a subagent summarize `build/build_task1.log` and report only errors/warnings.

- [ ] **Step 8: Run the Cas suite to verify the rename is behavior-preserving**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*' > build/test_task1.log 2>&1`
Expected: the same pass count as before the change (all `Cas*` green), including `CasStoreBeat.AckAdvancesOnlyAfterViewLoad`, `CasStoreBeat.ViewAdvanceEmitsRetiredViewAdvanceEvent`, `CasStoreBeat.GcStateReadFailureLeavesAckUnchanged`, `CasStoreBeat.DrainBlocksAckWhileMutationInFlight`. Have a subagent summarize `build/test_task1.log` (pass/fail counts + any failure detail).

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp \
        src/Disks/tests/gtest_cas_store.cpp \
        docs/superpowers/cas/08-testing-and-soak.md \
        docs/superpowers/cas/ROADMAP.md \
        docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md
git commit -m "$(cat <<'EOF'
CAS: rename the view-side "beat" — refreshViewForBeat→syncRetiredView, MountBeat→retired_view_advance

Pure rename ahead of the lease/view-sync decouple (P3.1 Task 5). The overloaded
"beat" named both the lease heartbeat and the unrelated retired-view refresh;
this renames only the view side. No behavior change — the renewal still calls
syncRetiredView via observed_round_fn (rewired next task). Cas* suite unchanged.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
EOF
)"
```

---

## Task 2: Decouple the renewal path (`observedGcRound` + compose `renewWatermarkOnce`)

Make the production renewal path read the cheap installed round instead of running the S3 view refresh. Preserve the (test-only) `renewWatermarkOnce` contract by redefining it as the composed step, so the entire GC test surface keeps advancing the ack. Add the synchronous view prime to `tryRemountOnce` (its `doStart` no longer loads the view). Reword the keeper doc comments.

> **Note for the reviewer:** after this task the production OPEN path primes the view once (`CasStore.cpp:140`) and then does NOT advance it in the background (renewal no longer syncs, and the dedicated syncer thread lands in Task 3). The branch is not shippable until Task 3. Each task still builds and passes the suite because the background syncer is production-only (`background_watermark`) and unit tests drive `syncRetiredView` through the composed `renewWatermarkOnce`.

**Files:**
- Modify: `CasStore.h` (add `observedGcRound` decl near `minActive` at `:275`; add a `renewLeaseOnlyForTest` seam near the other `*ForTest` seams ~`:447`), `CasStore.cpp` (`observedGcRound` def; rewire keeper ctors at `:270` and `:504`; redefine `renewWatermarkOnce` at `:648`; add the synchronous `syncRetiredView()` prime in `tryRemountOnce` before `mount_keeper->start()` at `:516`)
- Modify: `CasServerRoot.h:300-306,344`, `CasServerRoot.cpp:629-636` (doc comments only)
- Modify: `CasStore.cpp:305-311` (the `background_watermark` comment: renewal no longer runs the sync)
- Test: `src/Disks/tests/gtest_cas_store.cpp` (new `CasLeaseViewDecouple` tests)

**Interfaces:**
- Consumes: `Store::syncRetiredView()` (Task 1).
- Produces:
  - `uint64_t Store::observedGcRound() const;` — returns `retire_view.round()` (the currently installed round; cheap, race-safe via `RetireView`'s own internal `shared_mutex`).
  - `void Store::renewWatermarkOnce();` — redefined as `syncRetiredView(); mount_keeper->renewOnce();` (test/manual composed driver; unchanged public signature).
  - `void Store::renewLeaseOnlyForTest();` — test seam that calls `mount_keeper->renewOnce()` WITHOUT syncing (drives the isolated renewal path).

- [ ] **Step 1: Write the failing tests**

Add to `src/Disks/tests/gtest_cas_store.cpp` (near the other `CasStoreBeat` tests). These pin the decoupling: the isolated renewal path advertises the *installed* round, never the freshly-published one, and does not depend on a view load.

```cpp
TEST(CasLeaseViewDecouple, RenewAdvertisesInstalledRoundNotPublished)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    /// Publish a NEWER round but do NOT sync the view. The isolated renewal path must advertise the
    /// round the view is currently INSTALLED at (0), proving it does not load the view.
    GcState st;
    st.round = 5;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->renewLeaseOnlyForTest();

    const auto got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got.has_value());
    const MountLease after_renew = decodeMountLease(got->bytes);
    EXPECT_EQ(after_renew.observed_gc_round, 0u) << "renewal must advertise the installed round, not the published one";
    EXPECT_GT(after_renew.seq, 1u) << "the lease was renewed (seq advanced)";
    EXPECT_EQ(store->retireView().round(), 0u) << "renewal must not install a newer view";

    /// A sync installs round 5; the NEXT isolated renewal then advertises it.
    store->syncRetiredView();
    EXPECT_EQ(store->retireView().round(), 5u);
    store->renewLeaseOnlyForTest();
    const auto got2 = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(decodeMountLease(got2->bytes).observed_gc_round, 5u);
}

TEST(CasLeaseViewDecouple, RenewWatermarkOnceComposesSyncThenRenew)
{
    /// renewWatermarkOnce is the composed test driver: it syncs the view THEN renews, so a single
    /// call still makes observed_gc_round follow the freshly-published round (the contract the GC
    /// pipeline tests rely on).
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    GcState st;
    st.round = 7;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->renewWatermarkOnce();

    const auto got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeMountLease(got->bytes).observed_gc_round, 7u);
    EXPECT_EQ(store->retireView().round(), 7u);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasLeaseViewDecouple.*' > build/test_task2.log 2>&1`
Expected: FAIL — `renewLeaseOnlyForTest` does not compile/exist yet (and, once the seam exists but before rewiring, `RenewAdvertisesInstalledRoundNotPublished` would fail because the old wiring loads the view and advertises 5). Have a subagent confirm the failure reason from `build/test_task2.log`.

- [ ] **Step 3: Add `observedGcRound` (declaration + definition)**

In `CasStore.h`, right after the `minActive()` declaration (`:275`), add:

```cpp
    /// The GC round the retired view is CURRENTLY INSTALLED at (spec 2026-07-06-decouple). Cheap,
    /// in-memory — the value the lease renewal advertises as `observed_gc_round`. Race-safe against
    /// the retired-view syncer via `RetireView`'s own internal shared_mutex; takes no Store lock.
    uint64_t observedGcRound() const;
```

In `CasStore.cpp`, add the definition next to `minActive` (after `CasStore.cpp:439`):

```cpp
uint64_t Store::observedGcRound() const
{
    return retire_view.round();
}
```

- [ ] **Step 4: Add the `renewLeaseOnlyForTest` seam**

In `CasStore.h`, near the other `*ForTest` seams (after `mutateShardForTest`, ~`:442`), add:

```cpp
    /// Test seam (spec 2026-07-06-decouple): drive the ISOLATED lease renewal — renewOnce WITHOUT a
    /// preceding view sync — so a test can prove the renewal path advertises only the installed round
    /// and never loads the view. Production renewal runs this via the keeper's background thread;
    /// `renewWatermarkOnce` is the composed (sync+renew) driver.
    void renewLeaseOnlyForTest()
    {
        if (!mount_keeper)
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renewLeaseOnlyForTest on a read-only Store");
        mount_keeper->renewOnce();
    }
```

(The `Exception`/`ErrorCodes::LOGICAL_ERROR` are already used in this header's TU via `CasStore.cpp`; the inline seam mirrors `renewWatermarkOnce`'s existing guard. If the header does not already see these symbols, define the body in `CasStore.cpp` instead and leave only the declaration here — match whatever the sibling `renewWatermarkOnce` does.)

> Implementer note: `renewWatermarkOnce` is defined out-of-line in `CasStore.cpp`, so if `Exception` is not visible in the header, declare `void renewLeaseOnlyForTest();` here and define it in `CasStore.cpp` beside `renewWatermarkOnce`.

- [ ] **Step 5: Rewire the two keeper constructions to the cheap reader**

At `CasStore.cpp:270` (open) and `CasStore.cpp:504` (remount), change the observed-round lambda from the sync to the cheap reader:

```cpp
                [raw] { return raw->minActive(); }, [raw] { return raw->observedGcRound(); },
```

- [ ] **Step 6: Redefine `renewWatermarkOnce` as the composed driver**

Replace the body at `CasStore.cpp:648-657`:

```cpp
void Store::renewWatermarkOnce()
{
    /// Composed test/manual driver (spec 2026-07-06-decouple): sync the retired view, THEN renew the
    /// lease. In production these are two independent threads (the syncer + the keeper's renewal loop);
    /// this one-call composition preserves the pipeline-test contract that a single renewWatermarkOnce
    /// makes `observed_gc_round` follow the freshly-committed gc/state.round. A read-only open never
    /// anchored the keeper; there is nothing to renew (fail closed rather than fabricate one).
    if (!mount_keeper)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS heartbeat: renewWatermarkOnce on a read-only Store");
    syncRetiredView();
    mount_keeper->renewOnce();
}
```

- [ ] **Step 7: Add the synchronous view prime to `tryRemountOnce`**

At `CasStore.cpp:516`, `open`'s prime at line 140 has no counterpart on the remount path, so the fresh keeper's `doStart` would read a stale installed round. Insert a synchronous sync immediately before `mount_keeper->start();`:

```cpp
        /// Prime the retired view for the fresh incarnation (open does this at Store::open via the
        /// initial retire_view.refresh(); the remount path has no such prime). doStart then reads the
        /// freshly-installed round via observedGcRound(), so the first anchored ack is current — the
        /// open-ordering the model's WOpen requires.
        syncRetiredView();
        mount_keeper->start();
```

Update the now-stale comment just below (currently "its payload runs `refreshViewForBeat`, so the retired view is LOADED") to reflect that the explicit `syncRetiredView()` above loads it and `doStart` reads it via `observedGcRound()`.

- [ ] **Step 8: Reword the keeper doc comments (no signature change)**

In `CasServerRoot.h` (`:300-306` and the `:344` member) and `CasServerRoot.cpp:629-636` (`prepareRenew`), the wording says the `observed_round_fn` "runs the BEAT" and "one beat renews all three". Change to: the renewal PUT stamps the clock, the build-watermark floor, and the **last-installed** GC-round ack (`observed_round_fn` now *reads* the installed round, it does not load the view). Keep `value3 = observed_round_fn()` in `prepareRenew` unchanged — only the comment changes.

Also update the `Store::renewWatermarkOnce` reference in `CasStore.h:278-283` and the `background_watermark` comment at `CasStore.cpp:305-311` ("one beat now renews the lease, the floor and the acked round together") to say the renewal advertises the last-installed round and the retired-view syncer (Task 3) advances it.

- [ ] **Step 9: Build**

Run: `ninja -C build unit_tests_dbms > build/build_task2.log 2>&1`
Expected: build succeeds. Subagent-summarize the log.

- [ ] **Step 10: Run the new tests + the full Cas suite**

Run: `build/src/unit_tests_dbms --gtest_filter='CasLeaseViewDecouple.*:Cas*' > build/test_task2.log 2>&1`
Expected: PASS — the two new tests pass, AND the full `Cas*` suite stays green (crucially the GC pipeline tests via `runRoundsUntilAbsent` and the four `CasStoreBeat.*` tests, all of which route through the composed `renewWatermarkOnce`). Subagent-summarize pass/fail counts and any failure detail.

- [ ] **Step 11: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "$(cat <<'EOF'
CAS: renewal reads the installed GC round (observedGcRound), not the S3 view refresh

P3.1 Task 5 (decouple, part 2). MountLeaseKeeper's observed_round_fn now reads
the currently-installed round in memory (observedGcRound) instead of running
syncRetiredView on the renewal thread, so the lease-renewal cadence no longer
depends on S3. renewWatermarkOnce is redefined as the composed test driver
(syncRetiredView + renewOnce), preserving the GC pipeline test contract;
tryRemountOnce gains a synchronous view prime (open already primes at open).
The dedicated background syncer lands in Task 3.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
EOF
)"
```

---

## Task 3: Add the retired-view syncer thread + wire into open / remount / teardown

Add the dedicated background poller that runs `syncRetiredView()` on `mount_renew_period`, gated by `background_watermark` (production only). Start it on writable open (after the fence is armed) and stop+join it in the destructor before its dependencies are destroyed. This restores background view advancement for the OPEN path that Task 2 removed from the renewal thread.

**Files:**
- Modify: `CasStore.h` (thread members + `startRetiredViewSync`/`stopRetiredViewSync`/`retiredViewSyncLoop` decls, next to the self-remount machinery ~`:609-620`)
- Modify: `CasStore.cpp` (loop def; start in `open` ~`:312`; stop+join in `~Store` ~`:322`)
- Test: `src/Disks/tests/gtest_cas_store.cpp` (syncer lifecycle + liveness)

**Interfaces:**
- Consumes: `Store::syncRetiredView()` (Task 1), `PoolConfig::background_watermark`, `PoolConfig::mount_renew_period`.
- Produces:
  - `void Store::startRetiredViewSync(std::chrono::milliseconds period);`
  - `void Store::stopRetiredViewSync();` (idempotent)
  - `void Store::retiredViewSyncLoop(std::chrono::milliseconds period);` (private)
  - members: `ThreadFromGlobalPool retired_view_sync_thread;`, `std::mutex retired_view_sync_mutex;`, `std::condition_variable retired_view_sync_cv;`, `bool retired_view_sync_stop = false;`

- [ ] **Step 1: Write the failing tests**

Add to `src/Disks/tests/gtest_cas_store.cpp`. The lifecycle test is fully deterministic. The liveness test starts the real thread, publishes a round, and asserts progress via a bounded *condition* wait (the same pattern the codebase already uses in `CasStoreBeat.DrainBlocksAckWhileMutationInFlight`).

```cpp
TEST(CasRetiredViewSyncer, StartStopIsCleanNoOp)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    /// Starting then stopping the syncer must be a clean lifecycle: the thread joins, no hang, no throw.
    store->startRetiredViewSync(std::chrono::milliseconds(5));
    store->stopRetiredViewSync();
    store->stopRetiredViewSync();   /// idempotent
    SUCCEED();
}

TEST(CasRetiredViewSyncer, RunningSyncerAdvancesPublishedRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    GcState st;
    st.round = 4;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->startRetiredViewSync(std::chrono::milliseconds(2));

    /// Bounded liveness assertion: the running syncer must install round 4 on its own. Poll the real
    /// condition (not a fixed sleep); fail if it never advances within a generous bound.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (store->retireView().round() != 4u && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    store->stopRetiredViewSync();
    EXPECT_EQ(store->retireView().round(), 4u) << "the syncer thread must advance the installed round on its own";
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRetiredViewSyncer.*' > build/test_task3.log 2>&1`
Expected: FAIL to compile — `startRetiredViewSync`/`stopRetiredViewSync` do not exist. Subagent-confirm from the log.

- [ ] **Step 3: Add the thread members + method declarations**

In `CasStore.h`, right after the self-remount machinery block (`:609-620`, ending at `ThreadFromGlobalPool remount_thread;`), add:

```cpp
    /// Retired-view syncer (spec 2026-07-06-cas-lease-view-sync-decouple): a dedicated background
    /// thread that runs `syncRetiredView` on `mount_renew_period`, decoupled from the lease-renewal
    /// thread so slow S3 view work can never delay a lease renewal past its TTL. Gated on
    /// `background_watermark` like every background thread (production only; unit tests drive
    /// `syncRetiredView` explicitly via `renewWatermarkOnce`). Started on writable open after the
    /// fence is armed; stopped+joined in the destructor before `retire_view`/`pool_backend` die.
    void startRetiredViewSync(std::chrono::milliseconds period);
    void stopRetiredViewSync();   /// idempotent
    void retiredViewSyncLoop(std::chrono::milliseconds period);
    std::mutex retired_view_sync_mutex;
    std::condition_variable retired_view_sync_cv;
    bool retired_view_sync_stop = false;   /// guarded by retired_view_sync_mutex
    ThreadFromGlobalPool retired_view_sync_thread;
```

- [ ] **Step 4: Implement the loop + start/stop**

In `CasStore.cpp`, add near the self-remount definitions (after `scheduleRemount`, ~`:578`). This mirrors `SingleWriterSlot::backgroundLoop`: a `wait_for(period)` loop that runs the body and never lets the body's exception escape (the body already swallows S3 failures; the outer `catch(...)` is the backstop).

```cpp
void Store::startRetiredViewSync(std::chrono::milliseconds period)
{
    std::lock_guard g(retired_view_sync_mutex);
    if (retired_view_sync_thread.joinable())
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS retired-view syncer already running");
    retired_view_sync_stop = false;
    retired_view_sync_thread = ThreadFromGlobalPool([this, period] { retiredViewSyncLoop(period); });
}

void Store::stopRetiredViewSync()
{
    ThreadFromGlobalPool to_join;
    {
        std::lock_guard g(retired_view_sync_mutex);
        if (!retired_view_sync_thread.joinable())
            return;
        retired_view_sync_stop = true;
        retired_view_sync_cv.notify_all();
        to_join = std::move(retired_view_sync_thread);
    }
    to_join.join();
}

void Store::retiredViewSyncLoop(std::chrono::milliseconds period)
{
    std::unique_lock lock(retired_view_sync_mutex);
    while (!retired_view_sync_stop)
    {
        if (retired_view_sync_cv.wait_for(lock, period, [this] { return retired_view_sync_stop; }))
            break;
        lock.unlock();
        try
        {
            syncRetiredView();
        }
        catch (...)
        {
            tryLogCurrentException(getLogger("CasStore"),
                "CAS retired-view sync: background sync failed; the installed view stays put and retries");
        }
        lock.lock();
    }
}
```

- [ ] **Step 5: Start the syncer on writable open**

In `CasStore.cpp`, immediately after the `startBackground` gate at `:312-313`, start the syncer under the same gate:

```cpp
        if (store->config.background_watermark)
            store->mount_keeper->startBackground(store->config.mount_renew_period);
        /// The retired-view syncer advances the installed round in the background, off the renewal
        /// thread (spec 2026-07-06-decouple). Same production-only gate as the renewer.
        if (store->config.background_watermark)
            store->startRetiredViewSync(store->config.mount_renew_period);
```

(The remount path does NOT restart the syncer: the syncer reads `gc/state` + installs the view and is not tied to a mount incarnation, so the thread started at open runs for the Store's whole life. `tryRemountOnce` already got its synchronous prime in Task 2.)

- [ ] **Step 6: Stop the syncer in the destructor**

In `CasStore.cpp`, in `~Store()` (`:321`), stop the syncer FIRST — before `mount_keeper->stop()` and before any member is destroyed — since its body touches `retire_view`, `pool_backend`, `view_gate` and the event sink. Add right after the remount-loop stop block (after `:330`, before the `if (mount_keeper)` block):

```cpp
    /// Stop the retired-view syncer before tearing down: its body touches retire_view / pool_backend /
    /// view_gate / the event sink, which are destroyed with this Store.
    stopRetiredViewSync();
```

- [ ] **Step 7: Build**

Run: `ninja -C build unit_tests_dbms > build/build_task3.log 2>&1`
Expected: build succeeds. Subagent-summarize the log.

- [ ] **Step 8: Run the syncer tests + the full Cas suite**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRetiredViewSyncer.*:Cas*' > build/test_task3.log 2>&1`
Expected: PASS — both new tests pass and the full `Cas*` suite stays green. Subagent-summarize pass/fail counts and any failure detail.

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "$(cat <<'EOF'
CAS: dedicated retired-view syncer thread (off the lease-renewal path)

P3.1 Task 5 (decouple, part 3). Adds a Store-owned background poller that runs
syncRetiredView on mount_renew_period, gated by background_watermark (production
only), started on writable open after the fence is armed and stopped+joined in
the destructor before its dependencies die. Restores background view
advancement for the open path now that renewal reads the installed round.
Slow S3 view work can no longer delay a lease renewal past its TTL.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
EOF
)"
```

---

## Self-Review

**1. Spec coverage:**
- §3.1 lease renewal reads cheap values → Task 2 (`observedGcRound`, keeper rewiring). ✓
- §3.2 dedicated syncer thread + bounded-lag ack → Task 3 (thread) + Task 2 (composed advance). ✓
- §3.3 gating + open-ordering (synchronous sync before start) → open primes at `:140` (unchanged, noted in Task 2/3); remount prime added Task 2 Step 7; syncer gated Task 3 Step 5. ✓
- §4.1 `CasStore` changes → Tasks 1–3. ✓  §4.2 event rename → Task 1. ✓  §4.3 `CasServerRoot` doc → Task 2 Step 8. ✓  §4.4 docs → Task 1 Step 6. ✓
- §6 error handling (syncer never throws out of loop; observedGcRound race-safe; join order) → Task 3 Steps 4/6. ✓
- §7 tests (rename; renewal view-independent; syncer advances) → Task 1 Step 8, Task 2 Step 1, Task 3 Step 1. ✓
- §8 naming table → Global Constraints + Tasks. ✓

**2. Placeholder scan:** No TBD/TODO; every code step shows the code; every test step shows the test and the exact `--gtest_filter`. ✓

**3. Type consistency:** `syncRetiredView()` returns `uint64_t` everywhere (Task 1); `observedGcRound() const` returns `uint64_t` and matches the `std::function<uint64_t()>` `observed_round_fn` param (Task 2); syncer method names (`startRetiredViewSync`/`stopRetiredViewSync`/`retiredViewSyncLoop`) and member names (`retired_view_sync_thread`/`_mutex`/`_cv`/`_stop`) are used identically in decls (Task 3 Step 3), defs (Step 4), and call sites (Steps 5/6). Event `CasEventType::RetiredViewAdvance` / `"retired_view_advance"` consistent between `CasEvent.h`/`CasEvent.cpp`/`CasStore.cpp`/tests (Task 1). ✓
