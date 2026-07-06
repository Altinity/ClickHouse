# CAS mount-lease fence recovery — Implementation Plan (P3.1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the mount-lease protocol recover from GC fence-outs instead of wedging: no permanent fail-closed on restart (S13), honest diagnosis of fence vs foreign writer, renewals that cannot be blocked past the TTL by the beat.

**Architecture:** Per spec `docs/superpowers/specs/2026-07-06-cas-mount-lease-fence-recovery-design.md` (TLA+ gate PASSED: `CaCasMountCore` `FenceCostsEpoch` + `NoPermanentWedge` + `W_RemountAfterFence`). Governing rule: a fence costs an epoch. All protocol edits mirror the proven model actions exactly.

**Tech Stack:** ClickHouse C++ (Allman), gtest (`src/Disks/tests/`), `build/` ninja. Base paths: `$CA = src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.

## Global Constraints

- Branch `cas-gc-rebuild`; new commits only; never `git add -A`; never `-j` with ninja; build logs redirected to unique files in `build/`.
- Gate for EVERY task: `ninja clickhouse unit_tests_dbms` exit 0 + `./src/unit_tests_dbms --gtest_filter='Cas*'` fully green (baseline 465 + this plan's additions).
- Allman braces. Say "exception", not "crash", for `LOGICAL_ERROR`s.
- Protocol semantics must match the TLA+ model verbatim: fenced same-epoch is NEVER adoptable/refreshable; recovery is ALWAYS a new epoch; `doStart` keeps ONE synchronous beat (resolved question — B applies to background renewals only).
- Existing behavior that must NOT change: `claimMount`'s reclaim branch (same uuid, different epoch, fenced/expired → immediate reclaim); the mount-audit events of Phase 2; the enriched refusal messages (extend, don't reword).

---

### Task 1: D — terminate of a never-started slot is a no-op

**Files:**
- Modify: `$CA/Core/CasSingleWriterSlot.cpp` (`doTerminate`, ~line 80)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (append)

**Interfaces:** none new.

- [ ] **Step 1: Failing test** (mirror the fixture style of existing `CasHeartbeat.*` tests — injected lambdas, no threads):

```cpp
TEST(CasHeartbeat, StopBeforeStartIsQuietNoOp)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    uint64_t now = 1000;
    MountLeaseKeeper keeper(backend, layout, "a", DB::UInt128{1}, 1,
        std::chrono::milliseconds(10'000), [&] { return now; }, [] { return uint64_t{0}; },
        [] { return uint64_t{0}; });
    /// start() never called (models Store::open failing before/inside doStart) —
    /// teardown must not throw "release before start"; there is nothing to release.
    EXPECT_NO_THROW(keeper.stop());
    /// A stop AFTER a successful start still performs the farewell (existing tests cover it);
    /// a DOUBLE terminate stays loud:
    EXPECT_NO_THROW(keeper.stop());
}
```

(Adapt the ctor argument list to the real one incl. the `CasEventSink` default — copy from an existing test.) NOTE: the second `stop()` expectation: `doTerminate` on an already-dead slot currently throws "double release" — for a never-started slot BOTH stops must be no-ops (nothing ever became releasable). Keep a genuinely-started double-stop loud (do not touch that branch).

- [ ] **Step 2: RED** — `ninja unit_tests_dbms > build_p31_task1_red.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasHeartbeat.StopBeforeStartIsQuietNoOp'` → the test fails on the first `stop()` throwing.

- [ ] **Step 3: Implement** — in `doTerminate` (CasSingleWriterSlot.cpp), replace the `seq == 0` throw:

```cpp
    if (seq == 0)
    {
        /// Never started (Store::open failed before/inside doStart) — nothing was claimed, so
        /// there is nothing to release. A quiet no-op; throwing here only turned an already-failing
        /// teardown into extra LOGICAL_ERROR noise (2026-07-06 S13 post-mortem).
        dead = true;
        return;
    }
```

(Keep the `dead` double-terminate throw ABOVE unchanged? NO — order matters: with `dead = true` set here, a second `stop()` would hit the `dead` throw. Move the `seq == 0` check BEFORE the `dead` check, and in it `return` quietly WITHOUT setting `dead` — a never-started slot is inert, both checks stay idempotent. Verify the final order: `if (seq == 0) return;` first, then the `dead` throw, then `dead = true; terminate();`.)

- [ ] **Step 4: GREEN + sweep** — `ninja clickhouse unit_tests_dbms > build_p31_task1.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -2`.

- [ ] **Step 5: Commit** — `git add` the two files; message: `CAS: terminate of a never-started SingleWriterSlot is a quiet no-op`.

---

### Task 2: E — fenced guards + the `FencedSelf` claim outcome

**Files:**
- Modify: `$CA/Core/CasServerRoot.h` (`MountClaimResult::Kind` gains `FencedSelf`)
- Modify: `$CA/Core/CasServerRoot.cpp` (`claimMount` same-epoch branch; keeper `claim()` fenced check)
- ~~Modify: `src/Common/ErrorCodes.cpp`~~ **USER DECISION 2026-07-06: no new ErrorCodes number (fork-merge pain — the numbered list conflicts with upstream constantly).** Instead: a CAS-local typed exception `Cas::MountFencedException : DB::Exception` (declared in `CasServerRoot.h` next to `MountClaimResult`, base code `ErrorCodes::ABORTED`); all throw sites use it; catch sites match BY TYPE (`catch (const MountFencedException &)`), never by code.
- Test: `src/Disks/tests/gtest_cas_mount.cpp` + `gtest_cas_heartbeat.cpp` (append)

**Interfaces:**
- Produces: `MountClaimResult::FencedSelf` (same uuid + same epoch + `gc_fenced` observed by `claimMount`); exception code `ErrorCodes::CAS_MOUNT_FENCED` thrown by `MountLeaseKeeper::claim` on an own-fenced observation. Task 4 (open retry) consumes both.

- [ ] **Step 1: Failing tests**

```cpp
TEST(CasClaimMount, SameEpochFencedIsNotRefreshable)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    /// mint for (uuid 1, epoch 1), then fence it in place (what computeHeartbeatFloor does):
    ASSERT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, 1, 1000, 10'000).kind,
              MountClaimResult::Claimed);
    {
        auto got = backend->get(layout.mountKey("a"));
        MountLease fenced = decodeMountLease(got->bytes);
        fenced.gc_fenced = true;
        fenced.seq += 1;
        ASSERT_EQ(backend->putOverwrite(layout.mountKey("a"), encodeMountLease(fenced), got->token).outcome,
                  PutOutcome::Done);
    }
    /// Same (uuid, epoch) re-claim must NOT refresh a fenced body — a fence costs an epoch:
    const auto r = claimMount(*backend, layout, "a", DB::UInt128{1}, 1, 2000, 10'000);
    EXPECT_EQ(r.kind, MountClaimResult::FencedSelf);
    /// The body on the backend is still the fenced one (no write happened):
    EXPECT_TRUE(decodeMountLease(backend->get(layout.mountKey("a"))->bytes).gc_fenced);
    /// A DIFFERENT epoch reclaims immediately (existing branch, unchanged):
    EXPECT_EQ(claimMount(*backend, layout, "a", DB::UInt128{1}, 2, 2000, 10'000).kind,
              MountClaimResult::Claimed);
}

TEST(CasMountAudit, KeeperAdoptRefusesFencedSelfWithTypedError)
{
    using namespace DB::Cas;
    /// mint (uuid 1, epoch 1), fence it, then a keeper for the SAME (uuid, epoch) adopts:
    /// must throw CAS_MOUNT_FENCED (recoverable-by-new-epoch), emit mount_conflict branch=fenced_by_gc.
    /// <setup mirrors KeeperForeignConflictRefusesAndNamesHolder; fence via direct putOverwrite as above>
    /// EXPECT: exception code == ErrorCodes::CAS_MOUNT_FENCED; message contains "fenced by GC";
    ///         captured event: MountConflict with detail["branch"] == "fenced_by_gc".
}
```

(Write the second test fully by mirroring `KeeperForeignConflictRefusesAndNamesHolder`'s fixture; assert the exception's `code()` — catch `const DB::Exception & e` and `EXPECT_EQ(e.code(), DB::ErrorCodes::CAS_MOUNT_FENCED)`.)

- [ ] **Step 2: RED** (compile fails on `FencedSelf`/`CAS_MOUNT_FENCED`).

- [ ] **Step 3: Implement**

`ErrorCodes.cpp`: append `CAS_MOUNT_FENCED` per the file's idiom (also `extern const int CAS_MOUNT_FENCED;` in the local `ErrorCodes` namespace of `CasServerRoot.cpp`).

`claimMount` (CasServerRoot.cpp, the same-epoch branch at `existing.writer_epoch == our_epoch`) — insert BEFORE the refresh write:

```cpp
    /// Same uuid + same epoch: it is OUR OWN claim — but a FENCED body is terminal for this
    /// (uuid, epoch): the GC dropped its ack from the floor when it fenced. Refreshing it in place
    /// would resurrect a fenced incarnation (TLA+ `FenceCostsEpoch` sabotage) — the caller must
    /// re-open with a fresh writer_epoch instead ("a fence costs an epoch").
    if (existing.writer_epoch == our_epoch)
    {
        if (existing.gc_fenced)
        {
            emitMountEvent(sink, CasEventType::MountConflict, srid, "fenced_by_gc", &existing,
                "own (uuid, epoch) mount slot is GC-fenced — terminal for this incarnation; "
                "recover with a fresh writer_epoch");
            return {.kind = MountClaimResult::FencedSelf, .body = existing};
        }
        ...existing refresh code...
    }
```

`MountLeaseKeeper::claim` — after the epoch check, before the adopt `putOverwrite`, add:

```cpp
    /// Same (uuid, epoch) but FENCED: the GC fenced our fresh lease before we adopted it (the
    /// lease expired mid-open — e.g. a slow first beat). Terminal for THIS epoch; the open path
    /// recovers by allocating a fresh writer_epoch and re-claiming (TLA+ `NoPermanentWedge`).
    if (observed.gc_fenced)
    {
        emitMountEvent(event_sink, CasEventType::MountConflict, srid, "fenced_by_gc", &observed,
            "own mount slot fenced by GC after lease expiry — recoverable with a fresh writer_epoch");
        throw Exception(ErrorCodes::CAS_MOUNT_FENCED,
            "CAS mount-lease: key '{}' was fenced by GC after lease expiry ({}) — "
            "recoverable: re-open with a fresh writer_epoch", key, describeMountHolder(observed));
    }
```

AND change the adopt `putOverwrite` FAILURE branch (currently the "touched while adopting" throw): re-read once and classify by body:

```cpp
    const PutResult res = backend->putOverwrite(key, body, got->token);
    if (res.outcome != PutOutcome::Done)
    {
        /// The slot moved between our GET and PUT. Diagnose by the CURRENT body, not the token
        /// (2026-07-06 root cause: the only same-(uuid,epoch)-preserving toucher is the GC fence).
        const auto reread = backend->get(key);
        if (reread)
        {
            const MountLease current = decodeMountLease(reread->bytes);
            if (current.server_uuid == server_uuid && current.gc_fenced)
            {
                emitMountEvent(event_sink, CasEventType::MountConflict, srid, "fenced_by_gc", &current,
                    "GC fenced our mount between the adopt's read and write — recoverable with a "
                    "fresh writer_epoch");
                throw Exception(ErrorCodes::CAS_MOUNT_FENCED,
                    "CAS mount-lease: key '{}' was fenced by GC inside the adopt window ({}) — "
                    "recoverable: re-open with a fresh writer_epoch", key, describeMountHolder(current));
            }
            emitMountEvent(event_sink, CasEventType::MountConflict, srid, "adopt", &current,
                "mount slot was touched while adopting our own mount slot — failing closed");
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS mount-lease: key '{}' was touched while adopting our own mount slot ({}) — failing closed",
                key, describeMountHolder(current));
        }
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' vanished while adopting our own mount slot — failing closed", key);
    }
```

- [ ] **Step 4: GREEN + full sweep.** Existing callers of `claimMount`/`claimMountAwaitingExpiry` treat unknown kinds as not-Claimed (verify: `Store::open` checks `!= Claimed` → double-start message; that stays correct until Task 4 teaches it `FencedSelf`).

- [ ] **Step 5: Commit** — `CAS: a fence costs an epoch — fenced same-epoch is not adoptable/refreshable (\`FencedSelf\`, \`CAS_MOUNT_FENCED\`)`.

---

### Task 3: A — honest renewal-failure classification

**Files:**
- Modify: `$CA/Core/CasSingleWriterSlot.h/.cpp` (virtual classification hook in `renewOnce`)
- Modify: `$CA/Core/CasServerRoot.cpp` (`MountLeaseKeeper` override; enrich `onRenewFailed` event)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (append)

**Interfaces:**
- Produces: `virtual void SingleWriterSlot::onRenewMismatch(const String & key)` (default: the current generic "touched by a foreign writer" throw); `MountLeaseKeeper::onRenewMismatch` override that re-reads and throws a classified exception (`CAS_MOUNT_FENCED` for own-fenced; the generic text ONLY for the genuinely-unexplained case).

- [ ] **Step 1: Failing test** — keeper started (mirror `RenewRereadsBothCallbacksAndBumpsSeq`'s setup), then fence the slot via direct `putOverwrite` (as in Task 2), then `keeper.renewOnce()`:

```cpp
    /// The renewal must classify the fence honestly — not "foreign writer":
    try
    {
        keeper.renewOnce();
        FAIL() << "renewOnce over a fenced slot must throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CAS_MOUNT_FENCED);
        EXPECT_TRUE(e.message().find("fenced by GC") != String::npos);
        EXPECT_TRUE(e.message().find("foreign writer") == String::npos);
    }
    /// and the capture sink saw mount_conflict branch=fenced_by_gc with the fenced body's identity.
```

- [ ] **Step 2: RED.**

- [ ] **Step 3: Implement** — in `SingleWriterSlot::renewOnce`, replace the inline throw with `onRenewMismatch(key);` + declare in the header:

```cpp
    /// Called when the token-guarded renew PUT hits PreconditionFailed. The base contract stays
    /// fail-closed and LOUD; subclasses may re-read and throw a more precisely classified
    /// exception (the mount keeper distinguishes a GC fence of our own expired lease from a
    /// genuine foreign writer). MUST throw — a renew mismatch never continues.
    virtual void onRenewMismatch(const String & mismatched_key);
```

Default implementation = the existing generic throw, verbatim. `MountLeaseKeeper::onRenewMismatch`: re-read; own uuid + fenced → emit `MountConflict` `branch=fenced_by_gc` (with body) + throw `CAS_MOUNT_FENCED` "fenced by GC after lease expiry (late renewal)"; own uuid + newer epoch → emit + throw LOGICAL_ERROR "superseded by a newer incarnation ({})"; foreign uuid → emit + throw LOGICAL_ERROR naming the holder; body absent or same-epoch-unfenced → the generic base throw (genuine single-writer violation, stays loud). NOTE: `backgroundLoop` catches ALL exceptions from `renewOnce` and calls `onRenewFailed` → `tripMountLost` + `scheduleRemount` — flow unchanged, verify by reading `backgroundLoop` and say so in the report.

- [ ] **Step 4: GREEN + sweep.** Also update the Task-3-of-phase-2 `onRenewFailed` event reason (it says "foreign/superseded touch") to "renew mismatch (see the preceding classified mount_conflict)".

- [ ] **Step 5: Commit** — `CAS: renewal mismatch is classified by BODY — a GC fence of our own lease is not a "foreign writer"`.

---

### Task 4: C — bounded fence-recovery retry in `Store::open`

**Files:**
- Modify: `$CA/Core/CasStore.cpp` (`Store::open` mount sequence ~:195-270; the same pattern in the remount path ~:420-470 if it constructs the keeper the same way — read it and mirror)
- Test: `src/Disks/tests/gtest_cas_store.cpp` or wherever `Store::open` unit tests live (grep `Store::open(` in src/Disks/tests; add next to relatives)

**Interfaces:**
- Consumes: `MountClaimResult::FencedSelf`, `ErrorCodes::CAS_MOUNT_FENCED` (Task 2), `allocateWriterEpoch` (existing).

- [ ] **Step 1: Failing test** — open a Store over a pool whose mount slot holds a FENCED body for THIS server's uuid at the epoch the open would first allocate... (simpler, deterministic setup): open a store once (epoch 1 claimed), stop it uncleanly (no farewell — just destroy/leak the mount body), fence the body directly, re-open with the same server uuid: the open must SUCCEED (via `FencedSelf` → fresh epoch → reclaim), and the final mount body must be unfenced with `writer_epoch > 1`. Use `openStoreForTest`-style helpers; if the helper always farewell-stops, write the mount body directly via the backend (encode a fenced lease for the store's uuid/epoch) before reopening — the assertions are what matter:

```cpp
    /// after the reopen:
    const MountLease final_lease = decodeMountLease(backend->get(layout.mountKey(srid))->bytes);
    EXPECT_FALSE(final_lease.gc_fenced);
    EXPECT_GT(final_lease.writer_epoch, fenced_epoch);
```

For the ADOPT-WINDOW case (fence between claim and adopt): use a hook backend (subclass `InMemoryBackend`, override `get` with an injectable `std::function<void()> before_return` — mirror `CountingBackend`'s subclassing style in `cas_test_helpers.h`) that, on the FIRST keeper-adopt `get` of the mount key, fences the slot after reading — so the keeper's `putOverwrite` hits `PreconditionFailed` → `CAS_MOUNT_FENCED` → the open loop retries with a fresh epoch → open still SUCCEEDS. Assert no exception escapes and the final lease is live at a higher epoch.

- [ ] **Step 2: RED** (open currently aborts).

- [ ] **Step 3: Implement** — wrap the claim→keeper-start sequence in `Store::open`:

```cpp
        /// Fence recovery (P3.1, TLA+ NoPermanentWedge): a fence of our fresh lease during open
        /// (expiry mid-open + GC round) is recoverable — a fence costs an epoch, so allocate a
        /// fresh writer_epoch and re-claim. Bounded like the expiry wait itself.
        const int max_fence_recoveries = 3;
        for (int fence_recovery = 0; ; ++fence_recovery)
        {
            const MountClaimResult claim = claimMountAwaitingExpiry(
                *store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
                [&now_ms]() { return now_ms(); }, ttl_ms, poll_interval_ms, margin_ms, sleep_ms,
                on_wait_start, emit_mount_event);
            if (claim.kind == MountClaimResult::FencedSelf && fence_recovery < max_fence_recoveries)
            {
                writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
                continue;
            }
            if (claim.kind != MountClaimResult::Claimed)
                throw Exception(ErrorCodes::ABORTED, "{}", mountDoubleStartMessage(srid, claim.body));

            ...synchronous first beat + keeper construction + setFenceCallbacks (existing code)...
            try
            {
                store->mount_keeper->start();
            }
            catch (const Cas::MountFencedException &)
            {
                if (fence_recovery >= max_fence_recoveries)
                    throw;
                store->mount_keeper.reset();
                writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid);
                continue;
            }
            break;
        }
```

ADAPT to the real surrounding code: `writer_epoch` is currently a local computed before this block — make it mutable; `allocateWriterEpoch`'s real signature — grep and use verbatim; whatever uses `writer_epoch` AFTER the loop (`armMountFence(our_uuid, writer_epoch, ...)`) must use the FINAL value. The keeper-construction code stays inside the loop so each retry builds a keeper at the current epoch. Check the remount path (`tryRemountOnce`, CasStore.cpp ~:420-470): it already allocates a fresh epoch per attempt — extend its keeper-start `catch` to treat `MountFencedException` (by type) like its existing retryable failures (read it first; smallest edit that makes the remount loop retry on the typed code).

- [ ] **Step 4: GREEN + full sweep.**

- [ ] **Step 5: Commit** — `CAS: Store::open recovers from a fence-during-open with a fresh epoch (bounded retry; TLA+ NoPermanentWedge)`.

---

### Task 5: B — background renewals never wait for the beat

**Files:**
- Modify: `$CA/Core/CasStore.cpp` + `CasStore.h` (beat thread; keeper wiring), `$CA/Core/CasStore.cpp:239-250` area
- Test: `src/Disks/tests/gtest_cas_store.cpp` (or heartbeat file — wherever the beat/ack tests live; grep `refreshViewForBeat` in src/Disks/tests first)

**Interfaces:**
- Produces: `Store::installedRound()` (fast, lock-light read of `retire_view.round()`); the keeper's `observed_round_fn` becomes `installedRound`; a `Store`-owned periodic beat thread (`beat_thread`, period = `mount_renew_period`) running `refreshViewForBeat` best-effort.

- [ ] **Step 1: Failing test** — the load-bearing property: `prepareRenew` (and therefore `renewOnce`) must NOT call `refreshViewForBeat`. Test via a hook/counting backend on a started Store: arrange the view so a beat WOULD do S3 reads (publish a gc/state with a newer round), then call the keeper's `renewOnce` (or drive one renewal) and assert the mount body's PUT happened with NO `gc/state` GET in between (counting backend: the only backend op of the renewal is the mount-key `putOverwrite`). Then call the beat explicitly (`store->refreshViewForBeat()` — make it public or test via the thread) and assert the ack advances on the NEXT renewal.

Design the exact test against the real test helpers (grep how existing tests drive `renewOnce` on a full Store — `gtest_cas_heartbeat.cpp` drives a bare keeper; a Store-level test may need the `openStoreForTest` + reaching `store->mount_keeper`... if the keeper is private, test the WIRING seam instead: assert `Store::open` passes `installedRound`-behavior — a lambda that does zero backend ops — by counting ops during a renewal. State the chosen approach in the report.)

- [ ] **Step 2: RED.**

- [ ] **Step 3: Implement**
1. `Store::installedRound()`: `return retire_view.round();` under the shared `view_gate` read lock if `round()` needs it (check how `retire_view.round()` is synchronized in `refreshViewForBeat` — if it is already safe to read, no lock).
2. Keeper wiring (`CasStore.cpp:248` and the remount twin at `:466`): `observed_round_fn` = `[raw] { return raw->installedRound(); }`.
3. `doStart`'s FIRST payload freshness (the resolved open-ordering question): in `Store::open`, call `store->refreshViewForBeat()` ONCE explicitly AFTER `claimMountAwaitingExpiry` succeeds and BEFORE `mount_keeper->start()` (inside the Task-4 loop, so a retry re-runs it) — the first anchored body then carries a post-claim ack exactly as today. Same in the remount path.
4. The background beat: a `ThreadFromGlobalPool` member in `Store` (start where `startBackground(mount_renew_period)` is gated — same `background_watermark` condition), loop: sleep `mount_renew_period`, `try { refreshViewForBeat(); } catch (...) { tryLogCurrentException(...); }`, exit on a stop flag set in the destructor/shutdown (mirror how `SingleWriterSlot::backgroundLoop`/`stopBackground` do the stop-flag + join — copy that shape; check Store's shutdown sequence and join BEFORE the members the beat touches are torn down).

- [ ] **Step 4: GREEN + full sweep.** Watch for: unit tests that relied on `renewOnce` advancing the ack (grep `observed_gc_round` assertions in gtest_cas_heartbeat/store) — those now need an explicit beat call before the renewal; fix the TESTS (the new contract is the point), never re-couple.

- [ ] **Step 5: Commit** — `CAS: renewals decoupled from the beat — a renewal is one PUT; the beat runs on its own cadence (P3.1 vector B)`.

---

### Task 6: live validation on the stand

**Files:** none (validation; fix-forward small findings).

- [ ] **Step 1**: rebuild + remount the ca-soak stand (down/up, ping both). 
- [ ] **Step 2**: fence-recovery cycle: stop ch1 for 60 s (past the 45 s threshold), confirm `gc_fence_out` + `state='fenced'` (as in the root-cause experiment), restart ch1 → recovers; NEW checks: err.log has NO "foreign writer" text for this cycle; `system.content_addressed_log` on ch1 shows the recovery; `system.content_addressed_mounts` shows a HIGHER `writer_epoch` live lease.
- [ ] **Step 3**: S13 full-scale run (`cd utils/ca-soak && setsid nohup python3 -m scenarios.run --scenario S13 --scale full --duration 20m --seed 20260707 > logs/scenario_S13_p31fix.log 2>&1 &`), wait, verify: PASS (the acceptance gate is 3× green — run 1× here; the remaining 2 runs are a release-gate item, note it in the report), and grep the run's err.log window: zero misleading "foreign writer" lines; any fence recoveries carry the honest text.
- [ ] **Step 4**: update `utils/ca-soak/scenarios/BACKLOG.md`: mark the P1 entry ("mount-lease self-adoption fails closed under rapid crash-restart") RESOLVED inline with the fix summary + commits; update `docs/superpowers/worklogs/2026-07-06-p31-mount-lease-root-cause.md` status line. Commit docs.

---

## Self-review notes

- Spec coverage: D→Task 1; E→Task 2; A→Task 3; C→Task 4; B→Task 5; acceptance/live→Task 6. Message-honesty + `branch=fenced_by_gc` events are inside Tasks 2-3. `doStart` keeps one synchronous beat (Task 5 step 3.3) per the resolved question.
- Type consistency: `MountClaimResult::FencedSelf` (Task 2) consumed by Task 4; `CAS_MOUNT_FENCED` (Task 2) thrown in Tasks 2-3, caught in Task 4; `installedRound` (Task 5) referenced nowhere else.
- The TLA+ mapping: Task 2 = model's fixed `AdoptRead`/`ClaimMount` fenced guards; Task 4 = the `localLost→AllocEpoch→ClaimMount` recovery loop; Task 3 = the fixed `Renew` fenced classification; sabotage configs are the regression meaning of Tasks 2/4.
