# CAS merge upload-failure resilience — fix implementation plan (#37)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the three CA-side defects that turn a transient S3 blip into a pool-wide, multi-minute write outage plus a merge-recompute storm (campaign item #37): over-fencing on transient renew exceptions, `ABORTED`'s silent-no-backoff handling defeating the merge queue's exponential backoff, and opaque `Information`-level logging with no `last_exception` visibility.

**Architecture:** Three independent, additive fixes, landed in priority order (fix 1 makes the whole scenario rare; fix 2 makes any residual outage back off; fix 3 is cheap diagnosis on top). Fix 1 touches only `SingleWriterSlot`/`MountLeaseKeeper` (`CasServerRoot.{h,cpp}`) plus one call site in `CasMountRuntime.cpp`. Fix 2 introduces one routing helper (`CasRequestControl.{h,cpp}`) and reroutes 19 existing `ABORTED` throw sites (10 in `CasPartWriteTxn.cpp`, 9 in `CasRefLedger.cpp`) to it — no new `ErrorCodes` value. Fix 3 adds a rate-limited `Warning` log inside that same helper.

**Tech Stack:** C++20, gtest, the `utils/ca-soak` scenario framework (Python) for the S3-fault regression leg.

## Global Constraints

- Branch: `cas-gc-rebuild`. Never push. Never rebase or amend — every fix is a new commit.
- Pathspec-exact commits (`git add <files>`, never `-A`/`.`); before each commit run `git diff --cached --stat` and confirm every listed path is one you intentionally touched (foreign-file check).
- Wrap every git command needing the shared lock: `flock /tmp/cas_git.lock -c '<git command>'`.
- Builds: `flock /tmp/cas_build.lock -c 'ninja -C <build_dir> <target>'` — never pass `-j`, never call `nproc`. Redirect ninja output to a build log file in the build directory and hand it to a subagent to summarize (never paste raw ninja output into the main transcript).
- Test runs: redirect to a uniquely-named log file in the build directory; hand each log to a subagent to summarize.
- Every commit message ends with the exact trailer:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```
- Prose/comments: `exception` not "crash"; `ASan` not "ASAN"; wrap literal identifiers (`MountLeaseKeeper`, `NETWORK_ERROR`, ...) in backticks; write `f` not `f()` when naming a function itself.
- CA fold/GC code must never throw on a 404 — untouched by this plan, but do not introduce a violation while editing nearby code.
- Allman braces (opening brace on its own line) for all new C++ code.
- Non-goals (do not implement): staged merged-part preservation/resume-upload; a new dedicated `ErrorCodes` value (the escape hatch is documented, not exercised); `MOVE`-to-CA (`promote 'moving'`) — separate spec.
- Source spec: `docs/superpowers/specs/2026-07-16-cas-merge-upload-retry-fix-design.md` (read it before starting; this plan implements it verbatim). Backlog entry: `docs/superpowers/cas/BACKLOG.md` §"PRODUCT FINDING (#37 ...)".

---

## Phase 1 — Renew-retry while the lease deadline is valid

### Task 1: `SingleWriterSlot` transient-vs-confirmed retry mechanism + `MountLeaseKeeper` deadline tracking

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp`

**Interfaces:**
- Produces: `SingleWriterSlot::shouldFenceOnTransientRenewFailure()` (protected virtual, default `true`); `MountLeaseKeeper`'s override of it; `MountLeaseKeeper`'s new trailing constructor parameter `std::chrono::milliseconds lease_safety_margin_ = std::chrono::milliseconds(2000)` (appended AFTER the existing `event_sink_ = {}` parameter, so every existing 8-arg/9-arg call site keeps compiling unchanged).
- Consumes: nothing new from other tasks.

- [ ] **Step 1: Read the current renewal loop to confirm line anchors**

Read `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp` around `SingleWriterSlot::renewOnce` (~line 913) and `SingleWriterSlot::backgroundLoop` (~line 995), and `CasServerRoot.h` around the `MountLeaseKeeper` class (~line 482). Confirm they still match the snippets below (the file may have shifted a few lines since this plan was written; the surrounding comments are the anchor, not the exact line numbers).

- [ ] **Step 2: Write the failing direct unit tests (RED — will not compile until Step 3/4 land)**

Append to `src/Disks/tests/gtest_cas_heartbeat.cpp` (after the existing `#include`s, add a testable subclass in the anonymous namespace already declared near the top of the file — extend the existing `namespace { ... }` block that holds `seedOwnClaim`):

```cpp
/// Fix #37 phase 1: `shouldFenceOnTransientRenewFailure` is `protected` on `MountLeaseKeeper` (it is an
/// internal decision hook, not part of the public keeper API) -- promote it to `public` here so these
/// tests can drive it directly, without needing a real background thread.
class TestableMountLeaseKeeper : public MountLeaseKeeper
{
public:
    using MountLeaseKeeper::MountLeaseKeeper;
    using MountLeaseKeeper::shouldFenceOnTransientRenewFailure;
};
```

Then, in the file body (after the existing `CasHeartbeat` tests), add:

```cpp
/// Fix #37 phase 1 (over-fencing): a TRANSIENT renewal failure (the background loop's `renewOnce`
/// threw, but NOT via a confirmed `onRenewMismatch`) must not fence while the last confirmed lease
/// still has more than `lease_safety_margin` left before it would expire -- the mount-lease protocol
/// guarantees no other writer can claim the slot before that deadline, so riding it out is safe.
TEST(CasHeartbeat, TransientRetryStaysWithinLeaseDeadline)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/1000);

    TestableMountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9,
                                     std::chrono::milliseconds(1000), [&] { return now_ms; },
                                     [] { return uint64_t{0}; }, CasEventSink{},
                                     /*lease_safety_margin=*/std::chrono::milliseconds(100));
    keeper.start();   /// claim() anchors confirmed_deadline_ms = 1000 (now) + 1000 (ttl) = 2000

    /// Well before the deadline's safety margin (2000 - 100 = 1900): must NOT fence.
    now_ms = 1500;
    EXPECT_FALSE(keeper.shouldFenceOnTransientRenewFailure());

    /// At/after the safety-margin boundary: must fence.
    now_ms = 1900;
    EXPECT_TRUE(keeper.shouldFenceOnTransientRenewFailure());
    now_ms = 2000;
    EXPECT_TRUE(keeper.shouldFenceOnTransientRenewFailure());
}

/// A successful renew (real or test-driven via `renewOnce`) extends the confirmed deadline -- the
/// boundary that WOULD have tripped against the OLD deadline no longer does against the refreshed one.
TEST(CasHeartbeat, SuccessfulRenewExtendsTransientRetryDeadline)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/1000);

    TestableMountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9,
                                     std::chrono::milliseconds(1000), [&] { return now_ms; },
                                     [] { return uint64_t{0}; }, CasEventSink{},
                                     /*lease_safety_margin=*/std::chrono::milliseconds(100));
    keeper.start();   /// confirmed_deadline_ms = 2000

    now_ms = 1900;
    ASSERT_TRUE(keeper.shouldFenceOnTransientRenewFailure()) << "sanity: 1900 trips the OLD deadline";

    /// A renew at now_ms=1900 succeeds and refreshes confirmed_deadline_ms to 1900 + 1000 = 2900.
    keeper.renewOnce();
    EXPECT_FALSE(keeper.shouldFenceOnTransientRenewFailure())
        << "the refreshed deadline (2900, margin 100) must not trip at now_ms=1900 any more";
}
```

- [ ] **Step 3: Confirm the tests fail to build**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_cas_heartbeat_p1_build.log 2>&1'
```
Expected: build FAILS — `CasEventSink{}` positional arg / `lease_safety_margin` parameter / `shouldFenceOnTransientRenewFailure` do not exist yet. Hand `build/gtest_cas_heartbeat_p1_build.log` to a subagent to confirm the failure is exactly these missing symbols (not an unrelated syntax error).

- [ ] **Step 4: Implement the mechanism in `CasServerRoot.h`**

In the `SingleWriterSlot` class, insert a new protected virtual hook right after the existing `onRenewFailed()` declaration:

```cpp
    /// Called from the background loop when `renewOnce` THREW (the loop is about to stop, off
    /// `state_mutex`); the mount-lease keeper latches the write fence to lost here. Default no-op.
    virtual void onRenewFailed() {}

    /// Fix #37 phase 1 (over-fencing): called from the background loop when `renewOnce` threw and the
    /// throw was NOT a confirmed mismatch (`onRenewMismatch` -- an observed PUT outcome proving
    /// supersession; see the flag this checks, `last_renew_failure_was_confirmed_mismatch`, in the
    /// private section below). Returning `true` means "treat as terminal now" -- fence immediately, the
    /// legacy behavior and the correct default for a subclass with no lease-deadline concept.
    /// `MountLeaseKeeper` overrides this to ride out a TRANSIENT exception (a `putOverwrite` that threw
    /// before any outcome was observed -- a timeout, 5xx, or connection reset) while its last CONFIRMED
    /// lease has not yet reached its safety-margin boundary: the mount-lease protocol guarantees no
    /// other writer can claim the slot before that deadline, so continuing to retry (not fencing) is
    /// safe. Called OFF `state_mutex`, same as `onRenewFailed`.
    virtual bool shouldFenceOnTransientRenewFailure() { return true; }
```

Then in the same class's `private:` section, add a new member right after `last_renew_time`:

```cpp
    std::chrono::steady_clock::time_point last_renew_time;

    /// Fix #37 phase 1: set immediately before `renewOnce` invokes `onRenewMismatch` (which always
    /// throws) so `backgroundLoop`'s catch block can tell a CONFIRMED mismatch (the PUT completed and
    /// observed a foreign token -- proven supersession) apart from a TRANSIENT exception
    /// (`putOverwrite` itself threw before any outcome was observed, or a defensive `dead`/`seq==0`
    /// guard fired). Reset to `false` at the top of every `renewOnce` call. `renewOnce` and
    /// `backgroundLoop` run on the SAME background thread, sequentially, so no synchronization is
    /// needed for this flag.
    bool last_renew_failure_was_confirmed_mismatch = false;
```

Now update the `MountLeaseKeeper` class. Its constructor declaration:

```cpp
    MountLeaseKeeper(
        BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
        uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
        std::function<uint64_t()> min_active_fn_,
        CasEventSink event_sink_ = {},
        std::chrono::milliseconds lease_safety_margin_ = std::chrono::milliseconds(2000));
```

(`lease_safety_margin_` is appended AFTER `event_sink_`, not before — both keep defaults, so every existing call site that passes 8 args, or 9 args ending in an explicit event sink, keeps compiling with zero changes.)

Its `protected:`/`private:` sections:

```cpp
protected:
    RenewPayload prepareRenew() const override;
    String encodeBody(uint64_t seq_, const RenewPayload & payload) const override;
    Token claim(const String & body) override;
    void terminate() override;
    void onRenewSucceeded() override;
    void onRenewFailed() override;
    void onRenewMismatch(const String & mismatched_key) override;
    /// Fix #37 phase 1: fence immediately only once our last CONFIRMED lease (the last successful
    /// `claim`/renew) has reached its safety-margin boundary -- `confirmed_deadline_ms - now <=
    /// lease_safety_margin`. Until then the mount-lease protocol still guarantees exclusivity.
    bool shouldFenceOnTransientRenewFailure() override;

private:
    /// Refreshes `confirmed_deadline_ms` from `now_ms_fn` + `ttl`. Called on every point this keeper
    /// KNOWS it holds a live lease: both success paths of `claim` (mint and adopt), and every
    /// successful background renew (`onRenewSucceeded`).
    void refreshConfirmedDeadline();

    String srid;
    UInt128 server_uuid;
    uint64_t writer_epoch;
    std::chrono::milliseconds ttl;
    std::function<uint64_t()> now_ms_fn;
    std::function<uint64_t()> min_active_fn;
    std::function<void()> on_renew_ok;
    std::function<void()> on_lost;
    CasEventSink event_sink;
    std::chrono::milliseconds lease_safety_margin;
    /// BOOTTIME-ms deadline (same clock as `now_ms_fn`/`MountFence`, and for the SAME suspend-safety
    /// reason -- see `MountFence`'s doc comment in `CasMountRuntime.h`) of the last CONFIRMED lease.
    /// 0 = none yet (`claim` always sets this before `startBackground` can run, so
    /// `shouldFenceOnTransientRenewFailure` observing 0 is defensive, not an expected steady state).
    uint64_t confirmed_deadline_ms = 0;
};
```

- [ ] **Step 5: Implement the mechanism in `CasServerRoot.cpp`**

Replace `SingleWriterSlot::renewOnce`:

```cpp
void SingleWriterSlot::renewOnce()
{
    /// Compute the per-call payload BEFORE taking state_mutex (see doStart): never hold state_mutex
    /// across the subclass callback.
    const RenewPayload payload = prepareRenew();

    std::lock_guard lock(state_mutex);
    /// Reset BEFORE the guards below: a `dead`/`seq==0` throw (a programming-bug guard, not a backend
    /// outcome) must not be misread as a CONFIRMED mismatch by `backgroundLoop` -- it falls into the
    /// TRANSIENT bucket by leaving this false, exactly like a `putOverwrite` exception below.
    last_renew_failure_was_confirmed_mismatch = false;
    if (dead)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: renew after {} on key '{}'", slot_name, terminal_verb, key);
    if (seq == 0)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS {}: renew before start on key '{}'", slot_name, key);

    const String body = encodeBody(seq + 1, payload);
    const PutResult res = backend->putOverwrite(key, body, last_token);
    if (res.outcome != PutOutcome::Done)
    {
        /// The PUT completed and observed a foreign token -- a CONFIRMED mismatch (proven
        /// supersession), not a transient failure. Mark it BEFORE calling the hook, which always throws.
        last_renew_failure_was_confirmed_mismatch = true;
        onRenewMismatch(key);
    }

    recordWrite(seq + 1, res.token);
}
```

Replace `SingleWriterSlot::backgroundLoop`:

```cpp
void SingleWriterSlot::backgroundLoop(std::chrono::milliseconds period)
{
    /// A CONFIRMED mismatch, or a TRANSIENT failure once `shouldFenceOnTransientRenewFailure` says the
    /// lease deadline has neared, stops the loop for good: lastRenewTime (and the slot's seq) stop
    /// advancing and GC observes the frozen seq. No retry, no re-mint. A TRANSIENT failure while the
    /// deadline is still safely away keeps the loop alive -- the mount-lease protocol guarantees no
    /// other writer can claim the slot before that deadline, so retrying is safe (fix #37 phase 1).
    std::unique_lock lock(background_mutex);
    while (!stop_requested)
    {
        if (wakeup.wait_for(lock, period, [this] { return stop_requested; }))
            break;

        lock.unlock();
        try
        {
            renewOnce();
        }
        catch (...)
        {
            /// `renewOnce` and this loop run on the SAME background thread, sequentially -- no
            /// synchronization needed to read the flag it just set.
            const bool confirmed = last_renew_failure_was_confirmed_mismatch;
            if (!confirmed && !shouldFenceOnTransientRenewFailure())
            {
                tryLogCurrentException(log, fmt::format(
                    "CAS {}: background renewal failed transiently, retrying while the lease is still valid",
                    slot_name));
                lock.lock();
                continue;
            }

            tryLogCurrentException(
                log, fmt::format("CAS {}: background renewal failed, the {} stops advancing", slot_name, slot_name));
            /// Notify the subclass that renewal failed and the loop is stopping (off `state_mutex`).
            /// The mount-lease keeper latches its local write fence to lost here. Never let the hook's
            /// own throw escape the loop — we are already stopping.
            try { onRenewFailed(); } catch (...) {}
            return;
        }
        /// Successful renewal: notify the subclass (off `state_mutex`) before sleeping again. The
        /// mount-lease keeper refreshes the write-fence deadline here.
        try { onRenewSucceeded(); } catch (...) {}
        lock.lock();
    }
}
```

Update `MountLeaseKeeper::MountLeaseKeeper`'s constructor (add the new parameter, initialize the new member — order must match the header's declaration order):

```cpp
MountLeaseKeeper::MountLeaseKeeper(
    BackendPtr backend_, const Layout & layout_, const String & srid_, UInt128 server_uuid_,
    uint64_t writer_epoch_, std::chrono::milliseconds ttl_, std::function<uint64_t()> now_ms_fn_,
    std::function<uint64_t()> min_active_fn_,
    CasEventSink event_sink_,
    std::chrono::milliseconds lease_safety_margin_)
    : SingleWriterSlot(std::move(backend_), layout_.mountKey(srid_), "mount-lease", "release", "CasMountLeaseKeeper")
    , srid(srid_)
    , server_uuid(server_uuid_)
    , writer_epoch(writer_epoch_)
    , ttl(ttl_)
    , now_ms_fn(std::move(now_ms_fn_))
    , min_active_fn(std::move(min_active_fn_))
    , event_sink(std::move(event_sink_))
    , lease_safety_margin(lease_safety_margin_)
{
}

void MountLeaseKeeper::refreshConfirmedDeadline()
{
    confirmed_deadline_ms = now_ms_fn() + static_cast<uint64_t>(ttl.count());
}

bool MountLeaseKeeper::shouldFenceOnTransientRenewFailure()
{
    /// Defensive: should never observe 0 here (see the member's doc comment) -- fail closed if it ever did.
    if (confirmed_deadline_ms == 0)
        return true;
    const uint64_t now = now_ms_fn();
    const uint64_t margin = static_cast<uint64_t>(lease_safety_margin.count());
    return now + margin >= confirmed_deadline_ms;
}
```

In `MountLeaseKeeper::claim`, add `refreshConfirmedDeadline();` immediately before EACH of its two success `return res.token;` statements: the mint branch (`"mint"`, absent-slot path, right after `emitMountEvent(event_sink, CasEventType::MountClaim, srid, "mint", ...)`), and the adopt branch at the very end of the function (right after `emitMountEvent(event_sink, CasEventType::MountClaim, srid, "adopt", ...)`).

Update `MountLeaseKeeper::onRenewSucceeded`:

```cpp
void MountLeaseKeeper::onRenewSucceeded()
{
    /// A successful background renew extended the durable lease. Refresh OUR OWN confirmed-deadline
    /// bookkeeping (fix #37 phase 1) as well as the Pool's write-fence deadline (via `on_renew_ok`,
    /// translated to `steady_clock::now() + ttl`, monotonic). No S3 read either way.
    refreshConfirmedDeadline();
    if (on_renew_ok)
        on_renew_ok();
}
```

- [ ] **Step 6: Build and run the new direct tests**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_cas_heartbeat_p1_build2.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.*' > build/gtest_cas_heartbeat_p1_run.log 2>&1
```
Expected: build succeeds; ALL `CasHeartbeat.*` tests pass, including the two new ones and every pre-existing one (`AnchorCarriesFloor`, `RenewRereadsCallbackAndBumpsSeq`, `StopStampsExpiredAndFarewellSentinel`, `ForeignTouchMakesRenewThrow`, `RenewOverFencedOwnSlotIsClassifiedNotForeign`, `StopBeforeStartIsQuietNoOp`). Hand the log to a subagent to confirm 100% pass, zero regressions.

- [ ] **Step 7: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_heartbeat.cpp
git diff --cached --stat
'
```
Verify the stat output lists ONLY those three files, then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: fix #37 over-fencing — retry transient renew failures while the lease deadline is valid

SingleWriterSlot::backgroundLoop treated any renewOnce exception as terminal, fencing the
mount-lease writer and burning its incarnation even on a transient network blip with ~28s of
valid lease left. Add shouldFenceOnTransientRenewFailure (default: legacy fence-immediately
behavior) and have MountLeaseKeeper override it to compare now against its own confirmed-lease
deadline (refreshed on every successful claim/renew). A CONFIRMED mismatch (onRenewMismatch, a
proven foreign token) still fences immediately; only a TRANSIENT exception rides out the
remaining lease.

EOF
)"'
```

---

### Task 2: Wire the fix into production + real-background-thread integration tests

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp`

**Interfaces:**
- Consumes: `MountLeaseKeeper`'s new trailing constructor parameter from Task 1.
- Produces: nothing new for later tasks.

- [ ] **Step 1: Write the failing integration tests**

Append to `src/Disks/tests/gtest_cas_heartbeat.cpp`:

```cpp
namespace
{
/// Wraps an `InMemoryBackend` so `putOverwrite` throws a TRANSIENT (non-mismatch) exception for the
/// first `fault_count` calls, then delegates normally. Models a `putOverwrite` that fails before any
/// outcome is observed (timeout / 5xx / connection reset) -- exactly the case fix #37 phase 1 targets,
/// as opposed to a `PreconditionFailed` (a CONFIRMED, backend-observed mismatch).
class TransientPutOverwriteFaultBackend final : public InMemoryBackend
{
public:
    int fault_count = 0;

    PutResult putOverwrite(const String & k, const String & b, const Token & e, const ObjectMeta & m = {}) override
    {
        if (fault_count > 0)
        {
            --fault_count;
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "injected transient putOverwrite fault");
        }
        return InMemoryBackend::putOverwrite(k, b, e, m);
    }
};
}

/// Real background thread: two transient faults, then the third beat lands. The loop must NOT stop and
/// must NOT fence (on_lost never fires) -- it just keeps retrying at the normal period.
TEST(CasHeartbeat, BackgroundLoopRetriesTransientFailureWithoutFencingOrStopping)
{
    auto backend = std::make_shared<TransientPutOverwriteFaultBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);
    backend->fault_count = 2;

    std::atomic<bool> lost{false};
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(30000),
                            [&] { return now_ms; }, [] { return uint64_t{0}; }, CasEventSink{},
                            std::chrono::milliseconds(2000));
    keeper.setFenceCallbacks([] {}, [&] { lost = true; });
    keeper.start();
    keeper.startBackground(std::chrono::milliseconds(20));

    /// Bounded poll (not a blind sleep): waits for the REAL background thread to land a renewal past
    /// the two faults. Generous 5s timeout; a background-thread test cannot be made synchronous without
    /// a dedicated test seam this codebase does not have (see gtest_cas_pool.cpp's preference for
    /// synchronous renewOnce-driven tests elsewhere -- not applicable here, since the loop-continuation
    /// behavior under test only exists inside backgroundLoop itself).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    uint64_t seq = 1;
    while (std::chrono::steady_clock::now() < deadline)
    {
        seq = decodeMountLease(backend->get(layout.mountKey(srid))->bytes).seq;
        if (seq >= 2)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    keeper.stopBackground();

    EXPECT_GE(seq, 2u) << "background loop never recovered from the transient faults";
    EXPECT_FALSE(lost.load()) << "a transient putOverwrite failure must not trip the fence";
}

/// A CONFIRMED mismatch (a foreign incarnation lands on the slot) must fence immediately, even with the
/// deadline nowhere near expiry -- the other half of fix #37 phase 1's distinction.
TEST(CasHeartbeat, BackgroundLoopFencesImmediatelyOnConfirmedMismatch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);

    std::atomic<bool> lost{false};
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(30000),
                            [&] { return now_ms; }, [] { return uint64_t{0}; }, CasEventSink{},
                            std::chrono::milliseconds(2000));
    keeper.setFenceCallbacks([] {}, [&] { lost = true; });
    keeper.start();

    /// A foreign incarnation overwrites the slot BEFORE the first background beat.
    const HeadResult h = backend->head(layout.mountKey(srid));
    MountLease foreign;
    foreign.server_uuid = uuid;
    foreign.writer_epoch = 9;
    foreign.seq = 99;
    ASSERT_EQ(backend->putOverwrite(layout.mountKey(srid), encodeMountLease(foreign), h.token).outcome,
              PutOutcome::Done);

    keeper.startBackground(std::chrono::milliseconds(20));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!lost.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    keeper.stopBackground();

    EXPECT_TRUE(lost.load()) << "a confirmed foreign-owner mismatch must fence immediately";
}
```

Add `namespace DB::ErrorCodes { extern const int NETWORK_ERROR; }` near the top of `gtest_cas_heartbeat.cpp` (after the existing includes, before `using namespace DB::Cas;`), and add `#include <atomic>` / `#include <thread>` to its includes if not already present (check first — the file currently includes `<limits>` only from the standard library).

- [ ] **Step 2: Confirm the tests fail to build (RED)**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_cas_heartbeat_p2_build.log 2>&1'
```
Expected: FAILS — `TransientPutOverwriteFaultBackend` references `DB::ErrorCodes::NETWORK_ERROR` which is not yet declared as extern in this file (or, if the extern is already added correctly, the test should actually compile at this point since Task 1 already landed the constructor signature — in that case skip to Step 3 and expect a RUNTIME failure/timeout instead if the wiring is wrong; if both build and an initial run look suspiciously green already, re-check that `fault_count` is actually being exercised).

- [ ] **Step 3: Update the production call site in `CasMountRuntime.cpp`**

In `CasMountRuntime::installKeeper`, change:

```cpp
    mount_keeper = std::make_unique<MountLeaseKeeper>(
        backend_ptr, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); });
```

to:

```cpp
    mount_keeper = std::make_unique<MountLeaseKeeper>(
        backend_ptr, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); },
        std::chrono::milliseconds(cas_request_budget.lease_safety_margin_ms));
```

- [ ] **Step 4: Build and run**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_cas_heartbeat_p2_build2.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.*' > build/gtest_cas_heartbeat_p2_run.log 2>&1
```
Expected: build succeeds; all `CasHeartbeat.*` tests pass, including the two new background-thread tests. If either new test times out (loops until the 5s deadline without the expected condition), that is a real bug in Task 1's implementation — do not weaken the test; go back and fix Task 1. Hand the log to a subagent to summarize pass/fail counts.

- [ ] **Step 5: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
        src/Disks/tests/gtest_cas_heartbeat.cpp
git diff --cached --stat
'
```
Verify only those two files, then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: wire the renew-retry fence-safety-margin into CasMountRuntime + background-thread tests

installKeeper now passes the pool CasRequestBudget lease_safety_margin_ms into MountLeaseKeeper,
so the production mount-lease keeper actually rides out transient renewal blips instead of
fencing on the first one (fix #37 phase 1). Adds two real-background-thread gtests proving the
loop retries-and-recovers on a transient fault without fencing, and still fences immediately on
a confirmed foreign-owner mismatch.

EOF
)"'
```

---

### Task 3: Full CAS gtest regression check for Phase 1

**Files:** none modified — verification only.

- [ ] **Step 1: Build and run the full CAS gtest surface**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_p1_full_build.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/gtest_p1_full_run.log 2>&1
```

- [ ] **Step 2: Hand the log to a subagent**

Ask it to report the total pass/fail count and list any failing test names verbatim. Since Phase 1 only touched `SingleWriterSlot`/`MountLeaseKeeper` (a single subclass, `CasServerRoot.{h,cpp}`) and one `CasMountRuntime.cpp` call site with a backward-compatible trailing default parameter, expect ZERO regressions outside the tests Task 1/2 already added. If anything else fails, treat it as a real bug — do not proceed to Phase 2 until this is green.

---

## Phase 2 — Retry-later error class via one helper

### Task 4: `throwCasWriteRetryLater` / `makeCasWriteRetryLaterExceptionPtr` helper

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp`
- Test: `src/Disks/tests/gtest_cas_request_control.cpp`

**Interfaces:**
- Produces: `[[noreturn]] void DB::Cas::throwCasWriteRetryLater(const String & why)`; `std::exception_ptr DB::Cas::makeCasWriteRetryLaterExceptionPtr(const String & why)`. Both throw/carry `Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why)`.

- [ ] **Step 1: Write the failing unit tests**

Insert into `src/Disks/tests/gtest_cas_request_control.cpp`, right after the existing unconditional `TEST(CasRequestControl, SuccessIsAlwaysCommitted)` test (line ~44) and BEFORE the `#if USE_AWS_S3` block that follows it — these two tests need no AWS S3 dependency and must compile/run in every build configuration:

```cpp
/// Fix #37 phase 2: the retry-later throw must be NETWORK_ERROR, never ABORTED -- ABORTED is silently
/// swallowed by ReplicatedMergeMutateTaskBase (no backoff, no last_exception), which is exactly the
/// defect this fix closes.
TEST(CasWriteRetryLater, ThrowsNetworkErrorNotAborted)
{
    bool threw = false;
    try
    {
        throwCasWriteRetryLater("test cause");
        FAIL() << "throwCasWriteRetryLater must always throw";
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
        EXPECT_NE(e.code(), DB::ErrorCodes::ABORTED);
        EXPECT_NE(e.message().find("test cause"), String::npos) << e.message();
        EXPECT_NE(e.message().find("retrying later"), String::npos) << e.message();
    }
    EXPECT_TRUE(threw);
}

/// The exception_ptr twin (for call sites that fail a pending future/promise rather than throw
/// directly, e.g. CasRefLedger's queued-append completion paths) must carry the SAME classification.
TEST(CasWriteRetryLater, ExceptionPtrVariantCarriesSameClassification)
{
    const std::exception_ptr eptr = makeCasWriteRetryLaterExceptionPtr("another cause");
    bool threw = false;
    try
    {
        std::rethrow_exception(eptr);
        FAIL() << "expected a thrown exception";
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
        EXPECT_NE(e.message().find("another cause"), String::npos) << e.message();
    }
    EXPECT_TRUE(threw);
}
```

Add `NETWORK_ERROR` and `ABORTED` to the file's UNGUARDED `ErrorCodes` extern block. The file currently has no unguarded `DB::ErrorCodes` block (only one inside `#if USE_AWS_S3`) — add one right after the `using namespace DB::Cas;` line:

```cpp
namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int ABORTED;
}
```

- [ ] **Step 2: Confirm the tests fail to build**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_helper_p4_build.log 2>&1'
```
Expected: FAILS — `throwCasWriteRetryLater`/`makeCasWriteRetryLaterExceptionPtr` are undeclared.

- [ ] **Step 3: Implement the helper in `CasRequestControl.h`**

Add near the end of the `DB::Cas` namespace (after `validateCasRequestBudget`'s declaration):

```cpp
/// Throw the recoverable "CAS write could not be committed, retry later" condition.
///
/// WHY NETWORK_ERROR (this replaces an earlier ABORTED throw):
/// A content-addressed write can fail for a reason that is neither the caller's fault
/// nor permanent: the mount-lease / write fence was lost (e.g. a renewal PUT timed out
/// against a slow or throttling object store), or a conditional PUT exhausted its retry
/// budget mid-outage. The right response is "abandon this attempt, try again later" --
/// which is precisely what a transient error means.
///
/// It previously threw ABORTED, which was actively harmful to background merges:
/// ReplicatedMergeMutateTaskBase treats ABORTED as "merge deliberately cancelled
/// (shutdown / DROP / merges-blocker), not an error", so it neither records
/// last_exception_time_ms nor lets ReplicatedMergeTreeQueue's exponential backoff
/// engage. Under a sustained store outage the queue re-executed the merge roughly every
/// 2 seconds, recomputing the whole (possibly multi-GiB) output part every time for the
/// entire outage -- hundreds of full recomputes, and invisible in system.replication_queue.
///
/// NETWORK_ERROR is the best-fitting EXISTING code:
///   - it is NOT in the merge "retry silently, no backoff" exemption set (only ABORTED
///     and PART_IS_TEMPORARILY_LOCKED are), so the existing backoff -- capped by
///     max_postpone_time_for_failed_replicated_merges_ms -- engages automatically;
///   - it is already in ClickHouse's transient/retryable taxonomy
///     (checkDataPart::isRetryableException lists it beside ABORTED), so a part under
///     verification is not misread as corrupted;
///   - nothing on the merge / insert / replication commit path special-cases it in a way
///     that would misfire (ZooKeeper retriability keys on Coordination::Exception, a
///     different type), and it is not caught specially on the CAS write path.
///
/// Honest caveat: NETWORK_ERROR is coarser than the true condition. For the
/// throttled-store / timed-out / lost-lease cases it is accurate; for a purely logical
/// fence loss (e.g. the namespace is being dropped) it slightly overstates "network".
/// The precise cause is always in the exception MESSAGE, never inferred from the code.
///
/// If that imprecision ever matters -- operator confusion, or a future upstream change
/// that attaches merge-path handling to NETWORK_ERROR and reintroduces a collision --
/// switch to a dedicated code (e.g. CONTENT_ADDRESSED_WRITE_RETRY_LATER) by changing the
/// single throw below. A dedicated code is honest and collision-proof by construction
/// (backoff still engages, since only ABORTED / PART_IS_TEMPORARILY_LOCKED are exempt);
/// the only extra work is one appended line in ErrorCodes.cpp and, optionally, adding it
/// to checkDataPart::isRetryableException and an HTTP-status mapping for the foreground
/// INSERT client. We deliberately kept NETWORK_ERROR for now to add zero new coupling to
/// generic ClickHouse code, consistent with the rest of the CAS layer.
///
/// SCOPE: only the ESCAPING retry-later throws route here (fence lost / write outcome
/// uncertain / conditional-create Unresolved). The ABORTED values used as internal
/// control-flow signals (the condemned/vanished "re-upload from source" signal caught
/// inside putBlob), and the startup/decommission and generic live-lock-brake ABORTEDs,
/// keep their meaning and are NOT rerouted here.
[[noreturn]] void throwCasWriteRetryLater(const String & why);

/// Same classification as `throwCasWriteRetryLater`, but returns the exception as a
/// `std::exception_ptr` for call sites that fail a pending future/promise (`CasRefLedger`'s
/// `complete_error`) rather than throw directly. Both entry points route through the SAME
/// construction internally, so the error code / message shape has exactly one place that decides it.
std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why);
```

- [ ] **Step 4: Implement in `CasRequestControl.cpp`**

Add `#include <Common/LoggingHelpers.h>` to the includes. Add `NETWORK_ERROR` to the `DB::ErrorCodes` extern block (alongside the existing `BAD_ARGUMENTS`, `CORRUPTED_DATA`, `LOGICAL_ERROR`, `NOT_IMPLEMENTED`). Add, in the `DB::Cas` namespace (this is the Phase 2 form — Phase 3 will add the rate-limited log; for now implement the plain throw/exception-ptr pair):

```cpp
[[noreturn]] void throwCasWriteRetryLater(const String & why)
{
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why)
{
    return std::make_exception_ptr(
        Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why));
}
```

- [ ] **Step 5: Build and run**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_helper_p4_build2.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='CasWriteRetryLater.*:CasRequestControl.*:CasRequestController.*' > build/gtest_helper_p4_run.log 2>&1
```
Expected: build succeeds; all pass, including the two new tests and every pre-existing `CasRequestControl`/`CasRequestController` test (unaffected).

- [ ] **Step 6: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp \
        src/Disks/tests/gtest_cas_request_control.cpp
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: add throwCasWriteRetryLater/makeCasWriteRetryLaterExceptionPtr (fix #37 phase 2, part 1)

Single helper the escaping retry-later CAS write throws will route through (next commits),
replacing an ABORTED that ReplicatedMergeMutateTaskBase silently swallows (no backoff, no
last_exception) with NETWORK_ERROR, which the merge queue's existing exponential backoff already
treats as retryable. See the helper's doc comment for the full rationale and the one-line escape
hatch to a dedicated error code if NETWORK_ERROR ever proves too coarse.

EOF
)"'
```

---

### Task 5: Reroute `CasPartWriteTxn.cpp`'s 10 retry-later throw sites

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp`

**Interfaces:**
- Consumes: `throwCasWriteRetryLater` from Task 4 (header already included via `CasRequestControl.h`, already `#include`d by this file).

- [ ] **Step 1: `requireAlive` — cancelled build (~line 134)**

```cpp
    if (cancelled.load(std::memory_order_acquire))
        throw Exception(ErrorCodes::ABORTED,
            "PartWriteTxn cancelled: its owning namespace was removed (dropNamespace) while this build was "
            "in flight; restart the build only after the namespace is recreated");
```
becomes:
```cpp
    if (cancelled.load(std::memory_order_acquire))
        throwCasWriteRetryLater(
            "PartWriteTxn cancelled: its owning namespace was removed (dropNamespace) while this build was "
            "in flight; restart the build only after the namespace is recreated");
```

- [ ] **Step 2: `requireAlive` — superseded epoch (~line 141)**

```cpp
    if (const uint64_t live = store->liveWriterEpoch(); epoch != live)
        throw Exception(ErrorCodes::ABORTED,
            "PartWriteTxn (writer_epoch {}) belongs to a superseded mount incarnation (live epoch {}) — "
            "the mount was fenced out and self-remounted; restart the build", epoch, live);
```
becomes:
```cpp
    if (const uint64_t live = store->liveWriterEpoch(); epoch != live)
        throwCasWriteRetryLater(fmt::format(
            "PartWriteTxn (writer_epoch {}) belongs to a superseded mount incarnation (live epoch {}) — "
            "the mount was fenced out and self-remounted; restart the build", epoch, live));
```

- [ ] **Step 3: `uploadFromSource` — Unresolved conditional create (~line 542)**

```cpp
        throw Exception(ErrorCodes::ABORTED,
            "uploadFromSource: conditional create at '{}' is UNCERTAIN (retry budget exhausted or mount "
            "fence lost) — nothing was acknowledged; retry re-uploads from the writer's own source (INV-1)",
            key);
```
becomes:
```cpp
        throwCasWriteRetryLater(fmt::format(
            "uploadFromSource: conditional create at '{}' is UNCERTAIN (retry budget exhausted or mount "
            "fence lost) — nothing was acknowledged; retry re-uploads from the writer's own source (INV-1)",
            key));
```

This is the ONE call site with an intended behavior change: `putBlob`'s bounded loop (~line 213, `catch (const Exception & e) { if (e.code() != ErrorCodes::ABORTED || ...) throw; }`) now sees `NETWORK_ERROR`, not `ABORTED`, so it rethrows on the FIRST attempt instead of locally retrying up to 8 times. This is deliberate (Task 7 updates the one test that pins the old 8-round behavior).

- [ ] **Step 4: `stageManifest` — `DefiniteFailure` (~line 787)**

```cpp
    if (put_outcome == CasWriteOutcome::DefiniteFailure)
        throw Exception(ErrorCodes::ABORTED,
            "stageManifest: part-manifest PUT at '{}' definitively failed (non-retryable rejection); "
            "nothing was named — the caller re-stages with a fresh ManifestId", key);
```
becomes:
```cpp
    if (put_outcome == CasWriteOutcome::DefiniteFailure)
        throwCasWriteRetryLater(fmt::format(
            "stageManifest: part-manifest PUT at '{}' definitively failed (non-retryable rejection); "
            "nothing was named — the caller re-stages with a fresh ManifestId", key));
```

- [ ] **Step 5: `stageManifest` — `Unresolved` (~line 796)**

```cpp
    if (put_outcome == CasWriteOutcome::Unresolved)
        throw Exception(ErrorCodes::ABORTED,
            "stageManifest: part-manifest PUT at '{}' is UNCERTAIN (retry budget exhausted) — "
            "nothing conclusive was named; the caller re-stages with a fresh ManifestId", key);
```
becomes:
```cpp
    if (put_outcome == CasWriteOutcome::Unresolved)
        throwCasWriteRetryLater(fmt::format(
            "stageManifest: part-manifest PUT at '{}' is UNCERTAIN (retry budget exhausted) — "
            "nothing conclusive was named; the caller re-stages with a fresh ManifestId", key));
```

- [ ] **Step 6: `promote` — manifest body absent (~line 903)**

```cpp
    if (!body_got)
        throw Exception(ErrorCodes::ABORTED,
            "promote: manifest body absent at {} — failing closed (retry with a fresh ManifestId)", manifest_key);
```
becomes:
```cpp
    if (!body_got)
        throwCasWriteRetryLater(fmt::format(
            "promote: manifest body absent at {} — failing closed (retry with a fresh ManifestId)", manifest_key));
```

- [ ] **Step 7: `promote` — `RefMatchesBody`/`ManifestNamespaceMatches` (~lines 907, 909)**

```cpp
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::ABORTED, "promote: RefMatchesBody failed for {}", manifest_key);
    if (!manifestNamespaceMatches(target_ns, body))
        throw Exception(ErrorCodes::ABORTED, "promote: ManifestNamespaceMatches failed for {}", manifest_key);
```
becomes:
```cpp
    if (!refMatchesBody(id.ref, body))
        throwCasWriteRetryLater(fmt::format("promote: RefMatchesBody failed for {}", manifest_key));
    if (!manifestNamespaceMatches(target_ns, body))
        throwCasWriteRetryLater(fmt::format("promote: ManifestNamespaceMatches failed for {}", manifest_key));
```

- [ ] **Step 8: `promote` — precommit no longer live owner (~line 949)**

```cpp
            if (!state.precommits.contains({final_ref_name, id.ref}))
                throw Exception(ErrorCodes::ABORTED,
                    "promote: precommit owner binding for ref '{}' (build {}) was removed (abandon or GC "
                    "reclaim) and is no longer the live owner — failing closed; the build must restart "
                    "(WPromote owner==bld)",
                    final_ref_name, u128ToHex(promote_build_id));
```
becomes:
```cpp
            if (!state.precommits.contains({final_ref_name, id.ref}))
                throwCasWriteRetryLater(fmt::format(
                    "promote: precommit owner binding for ref '{}' (build {}) was removed (abandon or GC "
                    "reclaim) and is no longer the live owner — failing closed; the build must restart "
                    "(WPromote owner==bld)",
                    final_ref_name, u128ToHex(promote_build_id)));
```

- [ ] **Step 9: `promote` — unique-ref invariant / no `allow_repoint` (~line 1015)**

```cpp
                if (!allow_repoint)
                    throw Exception(ErrorCodes::ABORTED,
                        "promote: ref '{}' already names a different committed manifest — refusing to overwrite "
                        "(unique-ref invariant; use republishRef for an intended repoint)", final_ref_name);
```
becomes:
```cpp
                if (!allow_repoint)
                    throwCasWriteRetryLater(fmt::format(
                        "promote: ref '{}' already names a different committed manifest — refusing to overwrite "
                        "(unique-ref invariant; use republishRef for an intended repoint)", final_ref_name));
```

- [ ] **Step 10: Remove the now-possibly-unused `ABORTED` check — do NOT remove the extern**

`ErrorCodes::ABORTED` stays used elsewhere in this file (`observeAndAdmit`'s condemned-token throw, `reviveObserve`, and `putBlob`'s `e.code() != ErrorCodes::ABORTED` check) — leave its `extern const int ABORTED;` declaration untouched. Add `extern const int NETWORK_ERROR;` is NOT needed here since this file never references `ErrorCodes::NETWORK_ERROR` directly (all 10 throws go through the helper) — confirm this with a quick grep after editing:

```bash
grep -n "ErrorCodes::NETWORK_ERROR" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
```
Expected: no output (0 matches) — if there is a match, something was edited incorrectly.

- [ ] **Step 11: Build (compile-only, do not run tests yet — Task 7 fixes the tests this reroute breaks)**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/cas_part_write_txn_reroute_build.log 2>&1'
```
Expected: build succeeds (production code compiles cleanly; some EXISTING tests will now FAIL at runtime, not at compile time, since they assert on `e.code()` values — that is expected and is Task 7's job).

- [ ] **Step 12: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: reroute CasPartWriteTxn.cpp retry-later throws through throwCasWriteRetryLater (fix #37 phase 2)

10 sites (requireAlive x2, uploadFromSource Unresolved, stageManifest x2, promote x5) that threw
ABORTED for a retry-later condition now throw NETWORK_ERROR via the shared helper, so the merge
queue's exponential backoff engages instead of the silent ABORTED no-backoff path. One intended
behavior change: putBlob's 8-round condemned-churn retry loop only catches ABORTED, so
uploadFromSource's NETWORK_ERROR now escapes to the caller (merge backoff) on the first attempt
instead of being re-driven locally 8 times -- desirable (no point hammering a lost fence locally).
Existing tests pinning ABORTED at these sites are fixed in the next commit.

EOF
)"'
```

---

### Task 6: Reroute `CasRefLedger.cpp`'s 9 retry-later throw sites

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`

**Interfaces:**
- Consumes: `throwCasWriteRetryLater` and `makeCasWriteRetryLaterExceptionPtr` from Task 4.

- [ ] **Step 1: Add the include**

Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>` to this file's includes (it is not currently included here).

- [ ] **Step 2: Recovery-restart cap (~line 231)**

```cpp
            if (attempt > kRefRecoveryMaxRestarts)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot or "
                    "log object kept vanishing between its LIST and GET) — giving up; this bound is a "
                    "runaway brake against a pathological cleanup race, not an expected steady state",
                    ns.string(), attempt - 1);
```
becomes:
```cpp
            if (attempt > kRefRecoveryMaxRestarts)
                throwCasWriteRetryLater(fmt::format(
                    "CAS ref-table recovery for namespace '{}' restarted {} times (a selected snapshot or "
                    "log object kept vanishing between its LIST and GET) — giving up; this bound is a "
                    "runaway brake against a pathological cleanup race, not an expected steady state",
                    ns.string(), attempt - 1));
```

- [ ] **Step 3: Recovery seal PUT did not commit (~line 393)**

```cpp
            if (outcome != CasWriteOutcome::Committed)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS recovery seal PUT for namespace '{}' did not commit; failing recovery closed "
                    "(the table stays unrecovered/non-writable; the next touch restarts recovery and "
                    "re-seals)", ns.string());
```
becomes:
```cpp
            if (outcome != CasWriteOutcome::Committed)
                throwCasWriteRetryLater(fmt::format(
                    "CAS recovery seal PUT for namespace '{}' did not commit; failing recovery closed "
                    "(the table stays unrecovered/non-writable; the next touch restarts recovery and "
                    "re-seals)", ns.string()));
```

- [ ] **Step 4: Store shutting down (~line 799)**

```cpp
    if (shutting_down.load(std::memory_order_acquire))
        throw Exception(ErrorCodes::ABORTED,
            "CAS store is shutting down — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id);
```
becomes:
```cpp
    if (shutting_down.load(std::memory_order_acquire))
        throwCasWriteRetryLater(fmt::format(
            "CAS store is shutting down — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id));
```

- [ ] **Step 5: Mount lost / lease expired (~line 904)**

```cpp
    if (!may_mutate())
    {
        complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return;
    }
```
becomes:
```cpp
    if (!may_mutate())
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS mount lost / lease expired — refusing to append ref-log transactions for server_root '{}'",
            config.server_root_id)));
        return;
    }
```

- [ ] **Step 6: Superseded by self-remount, whole-batch (~line 918)**

```cpp
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }
```
becomes:
```cpp
    if (rt->superseded_by_remount.load(std::memory_order_acquire))
    {
        complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount — "
            "retry against the fresh mount incarnation",
            config.server_root_id)));
        return;
    }
```

- [ ] **Step 7: Wedge still Unresolved (~line 986)**

```cpp
                complete_error(carve_all_pending(), std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                    "CAS ref-log append for namespace '{}' txn {}-{} is still UNCERTAIN — the append lane "
                    "stays wedged until the SAME key resolves durable or a conclusive rejection is observed",
                    ns.string(), wedge_copy->txn_id.writer_epoch, wedge_copy->txn_id.ref_sequence)));
```
becomes:
```cpp
                complete_error(carve_all_pending(), makeCasWriteRetryLaterExceptionPtr(fmt::format(
                    "CAS ref-log append for namespace '{}' txn {}-{} is still UNCERTAIN — the append lane "
                    "stays wedged until the SAME key resolves durable or a conclusive rejection is observed",
                    ns.string(), wedge_copy->txn_id.writer_epoch, wedge_copy->txn_id.ref_sequence)));
```

- [ ] **Step 8: Superseded by self-remount, before id allocation (~line 1144)**

```cpp
        complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
```
becomes:
```cpp
        complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
            "CAS ref-log append for server_root '{}': this cached table was superseded by a self-remount "
            "before id allocation — retry against the fresh mount incarnation",
            config.server_root_id)));
```

- [ ] **Step 9: `DefiniteFailure` (~line 1265)**

```cpp
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                "CAS ref-log append for namespace '{}' definitively failed (non-retryable rejection); "
                "cached state is unchanged and txn id {}-{} is a safe gap",
                ns.string(), id.writer_epoch, id.ref_sequence)));
```
becomes:
```cpp
            complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' definitively failed (non-retryable rejection); "
                "cached state is unchanged and txn id {}-{} is a safe gap",
                ns.string(), id.writer_epoch, id.ref_sequence)));
```

- [ ] **Step 10: `Unresolved`, wedged (~line 1278)**

```cpp
            complete_error(survivors, std::make_exception_ptr(Exception(ErrorCodes::ABORTED,
                "CAS ref-log append for namespace '{}' txn {}-{} is UNCERTAIN (retry budget exhausted) — "
                "the append lane is wedged until the SAME key resolves durable or a conclusive rejection "
                "is observed; this outcome is unproven, not failure",
                ns.string(), id.writer_epoch, id.ref_sequence)));
```
becomes:
```cpp
            complete_error(survivors, makeCasWriteRetryLaterExceptionPtr(fmt::format(
                "CAS ref-log append for namespace '{}' txn {}-{} is UNCERTAIN (retry budget exhausted) — "
                "the append lane is wedged until the SAME key resolves durable or a conclusive rejection "
                "is observed; this outcome is unproven, not failure",
                ns.string(), id.writer_epoch, id.ref_sequence)));
```

- [ ] **Step 11: Remove the now-unused `ABORTED` extern**

All 9 of this file's `ErrorCodes::ABORTED` usages were the ones just rerouted (confirm: `grep -n "ABORTED" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` should now show ONLY the `extern const int ABORTED;` declaration line itself, no usages). Remove that now-dead extern declaration from the file's `ErrorCodes` block (do not leave an unused extern). The `LOGICAL_ERROR` throw at the "impossible foreign interference" site (~line 960) is UNCHANGED — it stays `LOGICAL_ERROR`, not rerouted (a genuine invariant violation, not a retry-later condition).

- [ ] **Step 12: Build (compile-only)**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/cas_ref_ledger_reroute_build.log 2>&1'
```
Expected: succeeds. If it fails because some OTHER file in the same translation unit graph still references `CasRefLedger`'s (now-removed) `ABORTED` extern indirectly — it won't, externs are per-file — this is just a sanity note; the removal is local to this file only.

- [ ] **Step 13: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: reroute CasRefLedger.cpp retry-later throws through throwCasWriteRetryLater (fix #37 phase 2)

9 sites (recovery-restart cap, recovery seal PUT failure, shutdown, mount-lost, superseded-by-
remount x2, wedge-Unresolved x2, DefiniteFailure) that threw ABORTED for a retry-later condition
now throw/carry NETWORK_ERROR via the shared helper. This was every ABORTED usage in the file, so
the now-dead ABORTED extern declaration is removed. Existing tests pinning ABORTED at these sites
are fixed in the next task.

EOF
)"'
```

---

### Task 7: Fix the existing tests broken by the reroute (known set)

**Files:**
- Modify: `src/Disks/tests/gtest_cas_promote_republish.cpp`
- Modify: `src/Disks/tests/gtest_cas_part_write.cpp`
- Modify: `src/Disks/tests/gtest_cas_ref_writer.cpp`
- Modify: `src/Disks/tests/gtest_ca_wiring.cpp`

**Interfaces:** none — test-only fixes, one-to-one with Task 5/6's reroute.

- [ ] **Step 1: `gtest_cas_promote_republish.cpp` — `PromoteOverDifferentCommittedRefFailsClosed` (~line 90-113)**

This test calls `build2->promote(...)` directly with `allow_repoint` defaulted to `false` — this is the rerouted site (`promote` ~line 1015). Change:
```cpp
        FAIL() << "PRE-FIX: promote silently overwrote a committed ref (PROMOTE-OVER-COMMITTED-LEAK); "
                  "POST-FIX must throw ABORTED";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED);
    }
```
to:
```cpp
        FAIL() << "PRE-FIX: promote silently overwrote a committed ref (PROMOTE-OVER-COMMITTED-LEAK); "
                  "POST-FIX must throw a CAS write-retry-later NETWORK_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
    }
```
Add `extern const int NETWORK_ERROR;` next to the file's existing `extern const int ABORTED;` (~line 27).

**Do NOT touch** `RepublishReDriveOverDifferentContentDstFailsClosed` (~line 235-269, the OTHER `ABORTED` assertion in this same file, at line 267): it reaches `CachedPartFolderAccess::republishRef`'s OWN guard (`PartFolderAccess.cpp` ~line 359, "destination is already committed with different content"), which the design spec explicitly keeps as `ABORTED` (a genuine content conflict, not a retry-later condition) — verify this by reading `PartFolderAccess.cpp`'s `republishRef` before touching this test; leave it exactly as-is.

- [ ] **Step 2: `gtest_cas_part_write.cpp` — `PromoteFailsClosedWhenPrecommitNoLongerLiveOwner` (~line 1450)**

```cpp
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->promote(ns, "part_1", build->buildId(), id); });
```
becomes:
```cpp
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
        [&] { build->promote(ns, "part_1", build->buildId(), id); });
```
(this is the `promote` ~line 949 precommit-no-longer-live-owner site).

- [ ] **Step 3: `gtest_cas_part_write.cpp` — `PromoteRepointsCommittedRef` (~line 1506)**

```cpp
    /// allow_repoint = false (the default) -> ABORTED, existing invariant untouched; M1 still resolves.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build2->promote(ns, "part_1", build2->buildId(), m2_id); });
```
becomes:
```cpp
    /// allow_repoint = false (the default) -> NETWORK_ERROR (CAS write-retry-later), existing invariant
    /// untouched; M1 still resolves.
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR,
        [&] { build2->promote(ns, "part_1", build2->buildId(), m2_id); });
```
(this is the `promote` ~line 1015 site).

- [ ] **Step 4: `gtest_cas_part_write.cpp` — rewrite `BudgetExhaustionMapsToAborted` (~line 2124-2155)**

This test pins putBlob's OLD 8-round retry-then-escape behavior, which Task 5 Step 3 deliberately changed (NETWORK_ERROR escapes on the FIRST attempt). Replace the whole test (including its doc comment) with:

```cpp
/// Budget exhaustion: EVERY attempt is ambiguous and nothing ever lands. The controller reports the
/// uncertainty and uploadFromSource maps it to NETWORK_ERROR (fix #37 phase 2) -- the same retryable
/// abort class stageManifest and the ref-log lane map their exhausted budgets to. Unlike the OLD
/// ABORTED mapping, putBlob's bounded condemned-churn loop (8 rounds) does NOT re-drive this: it only
/// catches ABORTED, so a NETWORK_ERROR escapes on the FIRST attempt -- desirable (no point hammering a
/// lost fence locally 8 times; the caller's own backoff, e.g. the merge queue's, is what should retry).
TEST(CasPartWriteTxnBlobPutRetry, BudgetExhaustionMapsToNetworkErrorAndEscapesImmediately)
{
    auto b = std::make_shared<BlobPutFaultBackend>();
    auto s = openBlobFaultPool(b, /*max_attempts=*/3);
    const RootNamespace ns{"srv/tbl"};
    const String payload = "blob-payload-C";

    auto build = startBuildFor(s, ns, "part_blob_exhausted");
    const ManifestId id = build->stageManifest({blobManifestEntry("a.bin", payload)});
    build->precommitAdd(ns, "part_blob_exhausted", id);

    int payload_streams = 0;
    b->fault_count = 1000000;
    bool threw = false;
    try
    {
        build->putBlob(idOf(payload), countingSource(payload, payload_streams));
    }
    catch (const DB::Exception & e)
    {
        threw = true;
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
        EXPECT_NE(e.message().find("UNCERTAIN"), String::npos) << e.message();
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(b->stream_attempts, 3) << "the 3-attempt controller budget for ONE outer attempt -- "
                                          "putBlob's outer condemned-churn loop must NOT re-drive a NETWORK_ERROR";
}
```

Add `extern const int NETWORK_ERROR;` next to this file's existing `extern const int ABORTED;` (~line 40).

- [ ] **Step 5: `gtest_cas_ref_writer.cpp` — cancelled-build assertions (~lines 2377, 2380)**

```cpp
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->stageManifest({}); });
    /// And it certainly cannot promote a fresh committed ref into the removed namespace (the important
    /// invariant -- though the old WPromote "precommit removed" guard also blocked this, less directly).
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { build->promote(ns, "inflight", build->buildId(), id); });
```
becomes:
```cpp
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { build->stageManifest({}); });
    /// And it certainly cannot promote a fresh committed ref into the removed namespace (the important
    /// invariant -- though the old WPromote "precommit removed" guard also blocked this, less directly).
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { build->promote(ns, "inflight", build->buildId(), id); });
```
(both trip `requireAlive`'s `cancelled` guard, ~line 134).

- [ ] **Step 6: `gtest_cas_ref_writer.cpp` — wedged `dropNamespace` (~line 2412)**

```cpp
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropNamespace(ns); });
```
becomes:
```cpp
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { store->dropNamespace(ns); });
```
(the injected `_log` PUT fault drives `flushRefBatch` into the `Unresolved`/wedge path, ~line 1278). Add `extern const int NETWORK_ERROR;` next to this file's existing `extern const int ABORTED;` (~line 35).

- [ ] **Step 7: `gtest_ca_wiring.cpp` — `PromoteWithoutLivePrecommitAbortsWithoutResurrect` (~line 2402)**

```cpp
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED);
    }
```
becomes:
```cpp
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NETWORK_ERROR);
    }
```
(this is the `promote` ~line 949 owner-liveness site). Add `extern const int NETWORK_ERROR;` next to this file's existing `extern const int ABORTED;` (~line 2252). Optionally tidy the surrounding comment (~lines 2350, 2358, 2363, 2394) that says "ABORTED" in prose to say `NETWORK_ERROR` — not required for the test to pass, but keeps the doc comment honest.

- [ ] **Step 8: Build and run these four files' tests**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_p2_known_fixes_build.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='CasPromoteRepublish.*:CasPartWriteTxn*.*:RefWriterNamespaceRemoval.*:CaWiringResurrect.*' > build/gtest_p2_known_fixes_run.log 2>&1
```
Expected: all pass, including `RepublishReDriveOverDifferentContentDstFailsClosed` (untouched, must still assert `ABORTED` and still pass — this is the negative control proving the "do not touch" call was correct).

- [ ] **Step 9: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/tests/gtest_cas_promote_republish.cpp \
        src/Disks/tests/gtest_cas_part_write.cpp \
        src/Disks/tests/gtest_cas_ref_writer.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: fix tests broken by the ABORTED -> NETWORK_ERROR retry-later reroute (fix #37 phase 2)

Updates the known set of existing gtest assertions that pinned ABORTED at the 19 rerouted call
sites (promote x3 tests, requireAlive-cancelled x1, dropNamespace-wedge x1) to NETWORK_ERROR, and
rewrites CasPartWriteTxnBlobPutRetry.BudgetExhaustionMapsToAborted (renamed ...MapsToNetworkError-
AndEscapesImmediately) for the one genuine behavior change: putBlob's 8-round condemned-churn loop
no longer re-drives a NETWORK_ERROR. Deliberately leaves gtest_cas_promote_republish.cpp's
RepublishReDriveOverDifferentContentDstFailsClosed asserting ABORTED unchanged -- it reaches
PartFolderAccess::republishRef's own genuine-content-conflict guard, which is NOT one of the 19
rerouted sites.

EOF
)"'
```

---

### Task 8: Sweep the full CAS gtest suite for any remaining reroute fallout

**Files:** none known in advance — this task discovers and fixes them.

- [ ] **Step 1: Build and run the FULL CAS gtest surface**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_p2_full_sweep_build.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/gtest_p2_full_sweep_run.log 2>&1
```

- [ ] **Step 2: Hand the log to a subagent for triage**

Ask it to list every FAILING test name plus its assertion message. Known files that likely still have MORE `ABORTED` assertions beyond the ones Task 7 fixed (not yet individually traced by this plan): `gtest_cas_ref_writer.cpp` (additional `expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->dropRef(...) / store->listRefs(...) })` calls), `gtest_cas_pool.cpp` (`dropRef` assertions). Apply this decision rule to each failure:

- If the assertion is on an exception from `dropRef`/`dropNamespace`/`listRefs`/`appendRefOps`/`precommitAdd`/`promote`/`stageManifest`/`requireAlive`/`putBlob`'s `uploadFromSource`-Unresolved path (i.e., reachable through one of the 19 sites Task 5/6 rerouted in `CasPartWriteTxn.cpp` or `CasRefLedger.cpp`) — update `DB::ErrorCodes::ABORTED` → `DB::ErrorCodes::NETWORK_ERROR` at that assertion, and add `extern const int NETWORK_ERROR;` to the file if not already present (following the exact pattern used in Task 7).
- If the assertion is on GC-internal code (`CasGc.cpp`, e.g. `gtest_cas_gc_attempt.cpp`, `gtest_cas_gc_resume.cpp`, `gtest_cas_gc_undercount_repro.cpp`), decommission/startup code (`CasPool.cpp` startup/decommission, e.g. `gtest_cas_decommission.cpp`), `PartFolderAccess::republishRef`'s own content-conflict guard (`gtest_cas_part_folder_access.cpp`), or `CasPlainObjects.cpp`'s live-lock brake — LEAVE IT UNCHANGED. If one of these unexpectedly fails, that is a genuine regression — investigate why (it should not be affected by this reroute at all) rather than silently rerouting it.
- For any failure that does not obviously fit either bucket, read the actual throw site the test hit (via the assertion message / a quick trace) before deciding — never blindly reroute an assertion without confirming it is one of the 19 sites.

- [ ] **Step 3: Apply the fixes found by Step 2**

Edit each confirmed-affected test file following the Task 7 pattern exactly (swap the `ErrorCodes` constant, add the `NETWORK_ERROR` extern if missing).

- [ ] **Step 4: Re-build and re-run until green**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_p2_full_sweep_build2.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/gtest_p2_full_sweep_run2.log 2>&1
```
Repeat Steps 2-4 until the full CAS gtest surface is 100% green. Do not proceed to Phase 3 until it is.

- [ ] **Step 5: Commit**

```bash
flock /tmp/cas_git.lock -c 'git add <the specific files fixed in Step 3 — list them explicitly, never -A>'
git diff --cached --stat
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: fix remaining ABORTED->NETWORK_ERROR test fallout from the #37 retry-later reroute

Full CAS gtest sweep after the CasPartWriteTxn.cpp/CasRefLedger.cpp reroute (previous two
commits) turned up <N> more assertions pinning ABORTED at one of the 19 rerouted call sites;
updated to NETWORK_ERROR. <list the specific test names fixed>.

EOF
)"'
```

---

## Phase 3 — Honest message + observability

### Task 9: Rate-limited `Warning` log inside the helper

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp`

**Interfaces:** none new — internal change to Task 4's helper implementation only.

- [ ] **Step 1: Implement the log**

The `why` string already carries the precise cause at every one of the 19 call sites (fence-lost vs N-attempt budget exhaustion — see each site's message in Task 5/6). Visibility into `system.replication_queue.last_exception`/`last_exception_time` is already automatic from Phase 2 alone (`NETWORK_ERROR` is not in `ReplicatedMergeMutateTaskBase`'s `ABORTED`/`PART_IS_TEMPORARILY_LOCKED` exemption set, so `updateLastExeption` now runs — do not touch that upstream file). This task adds ONE CAS-side rate-limited log line, independent of whatever any particular caller does with the exception, so the condition is visible directly in the CAS logs too. Replace the Task 4 implementation in `CasRequestControl.cpp`:

```cpp
[[noreturn]] void throwCasWriteRetryLater(const String & why)
{
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why)
{
    return std::make_exception_ptr(
        Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why));
}
```

with:

```cpp
namespace
{
/// Shared by both public entry points below so the log line and the exception's message text can
/// never drift apart. Rate-limited (not per-distinct-`why` -- `LogSeriesLimiter` keys on the LOGGER
/// NAME only, so under a sustained outage where `why` keeps changing slightly, only the first message
/// in each window prints; this is the intended throttle, not a bug). Raised from the old implicit
/// Information-level visibility (fix #37 phase 3) to Warning: this condition is expected to self-heal
/// (the caller retries), but an operator watching CAS logs directly should see it without having to
/// know to look at system.replication_queue.
void logCasWriteRetryLater(const String & why)
{
    LogSeriesLimiter log(getLogger("CasWriteRetryLater"), /*allowed_count=*/1, /*interval_s=*/30);
    LOG_WARNING(log, "CAS write could not be committed ({}); retrying later", why);
}
}

[[noreturn]] void throwCasWriteRetryLater(const String & why)
{
    logCasWriteRetryLater(why);
    throw Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why);
}

std::exception_ptr makeCasWriteRetryLaterExceptionPtr(const String & why)
{
    logCasWriteRetryLater(why);
    return std::make_exception_ptr(
        Exception(ErrorCodes::NETWORK_ERROR, "CAS write could not be committed ({}); retrying later", why));
}
```

Add `#include <Common/LoggingHelpers.h>` to the file's includes (for `LogSeriesLimiter`) if Task 4 did not already add it.

- [ ] **Step 2: Rebuild and re-run the Task 4 helper tests (regression check, no new test needed)**

```bash
flock /tmp/cas_build.lock -c 'ninja -C build unit_tests_dbms > build/gtest_helper_p3_build.log 2>&1'
build/src/unit_tests_dbms --gtest_filter='CasWriteRetryLater.*' > build/gtest_helper_p3_run.log 2>&1
```
Expected: both `CasWriteRetryLater.*` tests from Task 4 still pass unchanged — the message text and error code are identical; only a log side effect was added, and gtest does not capture/assert on log output.

- [ ] **Step 3: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: rate-limited Warning log for the retry-later helper (fix #37 phase 3)

throwCasWriteRetryLater/makeCasWriteRetryLaterExceptionPtr now also emit a LogSeriesLimiter-
throttled Warning-level CAS log line, independent of whatever the caller (e.g.
ReplicatedMergeMutateTaskBase) separately does with the exception. system.replication_queue
visibility (last_exception/last_exception_time) is already automatic from phase 2's NETWORK_ERROR
reroute -- this only adds direct CAS-log visibility for operators not looking at that table.

EOF
)"'
```

---

## Phase 4 — Testing (S3-fault regression + fsck verification)

### Task 10: New scenario card — short vs long S3 fault against the mount lease

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py` (or the next free `S<N>` if `S39` has been claimed by other in-flight work by the time this task runs — check `grep -rn "^class S[0-9]" utils/ca-soak/scenarios/cards/*.py` first)
- Possibly modify: `utils/ca-soak/scenarios/framework/base.py` or wherever `compose_variant` is consumed (investigate in Step 1)

**Interfaces:**
- Consumes: `docker-compose-s3faultproxy.yml`'s control port (`http://localhost:8474/config`, `POST {"rate": ..., "modes": [...], "methods": [...]}` to arm, `POST {"rate": 0.0}` to disarm — same pattern as `s23_s27_misc.py`'s `_ctl` helper), `utils/ca-soak/scenarios/framework/assertions.py`'s `assert_fsck_clean(result, fsck)`, `utils/ca-soak/scenarios/framework/lifecycle.py`'s `fsck_summary(...)`/`dryrun(...)`.

- [ ] **Step 1: Investigate how this card reaches `docker-compose-s3faultproxy.yml`**

Read `utils/ca-soak/scenarios/framework/base.py`'s `compose_variant` field and whatever bring-up code consumes it (grep for `compose_variant` across `utils/ca-soak/`). Determine whether it already supports pointing at an arbitrary compose file, or only switches between a small fixed set (e.g. `None` / `"gc_shards2"`). Two acceptable outcomes:
  (a) If the mechanism generalizes cleanly (e.g. it already takes a file path/name), add `compose_variant = "s3faultproxy"` to this new card and wire it to `docker-compose-s3faultproxy.yml`.
  (b) If it does not, follow whatever pattern the existing repo already uses for a scenario that needs a NON-default compose (check `README.md` in `utils/ca-soak/scenarios/` for how `S27` — currently `needs_infra` because it lacks this exact proxy — was intended to eventually run once the infra existed; the `docker-compose-s3faultproxy.yml` file's own header comment describes it as already used by SOME runner). Do not mark this card `needs_infra`/inconclusive — per the "no skipped scenarios" project rule, build/adapt whatever compose-selection plumbing is missing rather than skip it.

- [ ] **Step 2: Write the scenario card**

Model it closely on `utils/ca-soak/scenarios/cards/s23_s27_misc.py`'s structure and its `_ctl` helper (`urllib.request` POST/GET against `http://localhost:8474`). Full skeleton:

```python
"""S39: mount-lease resilience under a degraded-but-alive S3 (fix #37 regression).

Closes the chaos-coverage gap the #37 post-mortem identified: prior soak chaos only faulted
*nodes* (kill/restart), never a degraded-but-alive store. Two legs on the SAME compose
(docker-compose-s3faultproxy.yml, the s3proxy container's control port at :8474):

- SHORT fault (< mount lease TTL): PUT/POST faulted at rate=1.0 for a window shorter than the
  configured mount_lease_ttl_ms, with nodes alive and a background INSERT/merge workload running.
  Asserts the mount lease is NEVER lost: no fence trip, no incarnation recycle (no MountConflict /
  epoch-bump events), writes pause/retry-through and resume once the fault clears.
- LONG fault (> mount lease TTL): same fault, held past the TTL. The fence trips (correct
  fail-close) -- asserts the queue's postpone/backoff GROWS (not a tight ~2s retry loop) and the
  system recovers cleanly once the fault clears. Also runs fsck to fixpoint afterward and asserts
  dangling == 0 (correctness verification -- links to the known S30 DANGLING-PRECOMMIT class for
  the precommit-window edge).
"""

import time

from ..framework import lifecycle, observe, sql
from ..framework.assertions import assert_fsck_clean
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common


def _ctl(path, body=None, timeout=10):
    import json as _json
    import urllib.request
    url = f"http://localhost:8474{path}"
    if body is None:
        return _json.loads(urllib.request.urlopen(url, timeout=timeout).read().decode())
    req = urllib.request.Request(url, data=_json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    return _json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())


@register
class S39(Scenario):
    name = "S39"
    title = "mount-lease resilience under a degraded-but-alive S3 (fix #37)"
    priority = "P1"
    param_table = {
        "dev": {"mount_lease_ttl_s": 15, "short_fault_s": 8, "long_fault_s": 25, "settle_s": 20},
        "ci": {"mount_lease_ttl_s": 30, "short_fault_s": 15, "long_fault_s": 45, "settle_s": 40},
        "full": {"mount_lease_ttl_s": 30, "short_fault_s": 15, "long_fault_s": 60, "settle_s": 60},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params

        # Disarm first: bring-up must be clean regardless of a prior run's state.
        _ctl("/config", {"rate": 0.0})

        # --- Leg A: SHORT fault (< lease TTL) -- lease must survive ---
        sql.exec(cl, "node1", "CREATE TABLE IF NOT EXISTS s39.t (k UInt64) ENGINE=ReplicatedMergeTree "
                              "ORDER BY k", database="s39")
        since = observe.now(cl)
        _ctl("/config", {"rate": 1.0, "modes": ["503"], "methods": ["PUT", "POST"]})
        try:
            sql.exec(cl, "node1", "INSERT INTO s39.t SELECT number FROM numbers(1000)", database="s39",
                     timeout=p["short_fault_s"] + 10, allow_fail=True)
            time.sleep(p["short_fault_s"])
        finally:
            _ctl("/config", {"rate": 0.0})
        time.sleep(5)   # let any in-flight retry land

        events = observe.ca_events_since(cl, since)
        fence_trips = [e for e in events if e.get("type") == "MountConflict"]
        result.add(Verdict.check("short fault: no MountConflict/fence-trip event", 0, len(fence_trips),
                                  len(fence_trips) == 0))

        # --- Leg B: LONG fault (> lease TTL) -- fence trips, merge backs off, then recovers ---
        since2 = observe.now(cl)
        _ctl("/config", {"rate": 1.0, "modes": ["503"], "methods": ["PUT", "POST"]})
        try:
            sql.exec(cl, "node1", "INSERT INTO s39.t SELECT number FROM numbers(1000, 1000)",
                     database="s39", timeout=5, allow_fail=True)
            time.sleep(p["long_fault_s"])
        finally:
            _ctl("/config", {"rate": 0.0})

        # Give the queue's backoff + self-remount time to recover.
        time.sleep(p["settle_s"])

        queue_rows = sql.exec(cl, "node1",
            "SELECT num_postponed, num_tries, last_exception FROM system.replication_queue "
            "WHERE database='s39' AND table='t' ORDER BY num_tries DESC LIMIT 5",
            database="s39")
        # The tight-loop regression looked like num_tries growing every ~2s with last_exception EMPTY.
        # Post-fix: last_exception is populated (phase 2/3) and num_postponed grows with num_tries.
        any_last_exception_populated = any(r.get("last_exception") for r in (queue_rows or []))
        result.add(Verdict.check("long fault: last_exception populated on the replication_queue entry",
                                  True, any_last_exception_populated, any_last_exception_populated))

        sql.exec(cl, "node1", "INSERT INTO s39.t SELECT number FROM numbers(2000, 100)", database="s39")
        final_count = sql.exec(cl, "node1", "SELECT count() FROM s39.t", database="s39")
        result.add(Verdict.check("post-recovery INSERT succeeds", True, bool(final_count),
                                  bool(final_count)))

        # --- Correctness verification: fsck to fixpoint, dangling == 0 ---
        fsck = lifecycle.fsck_to_fixpoint(stable=3) if hasattr(lifecycle, "fsck_to_fixpoint") else lifecycle.fsck_summary()
        assert_fsck_clean(result, fsck)
```

Adapt the exact `sql.exec`/`observe.*` helper signatures to whatever this framework's current API actually is (read `utils/ca-soak/scenarios/framework/sql.py` and `observe.py` — the ones used by `s23_s27_misc.py` and `s19_s22_clone_fetch.py` — before finalizing; the shapes above are illustrative of the ASSERTIONS this card must make, not a guarantee every helper name/signature is exact).

- [ ] **Step 3: Run the new scenario card standalone**

Follow this repo's existing convention for running a single scenario card against its compose (check `utils/ca-soak/README.md` / whatever `run_scenario.sh`-equivalent script exists) — e.g. something in the shape of:
```bash
cd utils/ca-soak && python3 -m scenarios.run --scenario S39 --scale dev 2>&1 | tee /tmp/s39_run.log
```
(confirm the exact invocation from the README before running). Both legs' `Verdict`s must pass; the fsck verdict must show `dangling == 0`.

- [ ] **Step 4: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py <any framework file touched in Step 1>
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: S39 scenario — mount-lease resilience under a degraded-but-alive S3 (fix #37 regression)

Closes the chaos-coverage gap identified in the #37 post-mortem: prior soak chaos only faulted
nodes, never a degraded-but-alive object store. Short fault (< lease TTL) asserts the mount lease
survives with no fence trip; long fault (> TTL) asserts the fence trips correctly, the merge
queue's last_exception is populated (fixes 2/3), and the system recovers cleanly; fsck-to-
fixpoint afterward asserts dangling == 0.

EOF
)"'
```

---

### Task 11: Fold S39 into the permanent regression suite

**Files:**
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` and/or `utils/ca-soak/scenarios/RUN_HISTORY.md` (whichever this repo's convention uses to track "this scenario is now part of the standard sweep" — check both before editing)

**Interfaces:** none.

- [ ] **Step 1: Register S39 in whatever list drives a full/`--priority P1` sweep**

Confirm `@register` (used in Task 10) already makes `S39` discoverable by the standard `--scenario all` / `--priority P1` runner — if so, no additional registration file exists to edit and this step is a no-op beyond a `RUN_HISTORY.md` note. If a separate manifest/README table enumerates scenario numbers for the "permanent set", add `S39` to it following the existing row format.

- [ ] **Step 2: Record the result in `RUN_HISTORY.md`**

Follow the existing entries' format in `utils/ca-soak/scenarios/RUN_HISTORY.md` — add one row/section for S39's first green run (date, scale, verdict summary).

- [ ] **Step 3: Commit**

```bash
flock /tmp/cas_git.lock -c '
git add utils/ca-soak/scenarios/BACKLOG.md utils/ca-soak/scenarios/RUN_HISTORY.md
git diff --cached --stat
'
```
Then:
```bash
flock /tmp/cas_git.lock -c 'git commit -m "$(cat <<"EOF"
cas: fold S39 (fix #37 lease-fault regression) into the permanent scenario sweep

EOF
)"'
```

---

## Self-review notes (for whoever executes this plan)

- Spec coverage: Fix 1 (Task 1-3), Fix 2 (Task 4-8), Fix 3 (Task 9), Testing (a)/(b) (Task 10-11), Testing (c) unit gtests (Task 1, 2, 4 — delivered inline per TDD rather than deferred to a separate "testing phase", since writing-plans requires tests alongside the code they cover).
- The MountLeaseKeeper constructor's new parameter is placed AFTER `event_sink_`, not before, specifically so the ~11 existing call sites across `gtest_cas_heartbeat.cpp`/`gtest_cas_mount.cpp` need zero changes — verify this remains true after Task 1 by grepping `MountLeaseKeeper keeper(\|MountLeaseKeeper k(` across `src/Disks/tests/` before considering Task 1 done.
- Task 8 exists precisely because a full hand-trace of every `ABORTED` assertion in `gtest_cas_ref_writer.cpp`/`gtest_cas_pool.cpp` was not completed while writing this plan (only a representative, confirmed subset in Task 7) — it is a real, necessary task, not filler.
