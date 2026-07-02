# CAS mount crash-recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a hard-killed CA server self-recover its mount on restart by waiting out its own stale lease (bounded by TTL) instead of aborting immediately (S13).

**Architecture:** Add a free function `claimMountAwaitingExpiry` (in `CasServerRoot.{h,cpp}`) that loops over the unchanged `claimMount`, polling until a same-uuid stale lease lapses (then reclaims, token-guarded) or a bound (`≤ TTL + margin`) elapses (then reports `LiveDoubleStart` — a genuinely live twin). `Store::open` calls it in place of the one-shot claim. All wait logic is unit-tested with an injected clock + sleep (no real sleeping). The multi-line double-start error is extracted into a tested helper `mountDoubleStartMessage`.

**Tech Stack:** C++ (ClickHouse), GoogleTest (`unit_tests_dbms`), `InMemoryBackend` test double, TLA+ (TLC via `run_mount.sh`), Python (`utils/ca-soak`).

## Global Constraints

- Allman braces (opening brace on its own line) — enforced by the CI style check.
- Never use `sleep` to fix a race condition. The wait here is a wall-clock lease-expiry wait (the protocol is inherently time-based) with a token-guarded reclaim — waiting on a condition, not papering over a race. Keep it that way.
- Fail closed: foreign `server_uuid` and corrupt objects must abort, never wait or reclaim across UUIDs.
- No new user-facing configuration; derive `poll_interval`/`margin` from existing `mount_lease_ttl_ms` / `mount_renew_period` (YAGNI).
- Do NOT modify `claimMount`, the owner anchor, the epoch counter, the watermark, or any `.tla`/`.cfg` file.
- Say "exception", not "crash", in comments/messages.
- Build: run `ninja` (no `-j`, no `nproc`) redirected to a log in the build dir; use a subagent to summarize the log. Same for test runs (unique log file per run).
- `utils/ca-soak/` is the user's working tree — edit `chaos.py` as directed, but do not restructure the suite.
- Spec: `docs/superpowers/specs/2026-07-02-cas-mount-crash-recovery-design.md`.

---

### Task 1: Extract the tested double-start message helper

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h` (add declaration after `claimMount`, ~line 162)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp` (add definition; add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>` for `u128ToHex`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `MountLease` struct (`CasServerRoot.h`), `u128ToHex` (`CasIds.h`).
- Produces: `String mountDoubleStartMessage(const String & srid, const MountLease & existing);` — the full multi-line, operator-actionable message body for a genuinely-live second mount. Task 3 (`Store::open`) throws it verbatim.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_mount.cpp` (near the other `CasServerRoot` tests):

```cpp
TEST(CasMountMessage, DoubleStartTextHasIdentityAndRemediation)
{
    MountLease m;
    m.server_uuid = (UInt128(0xdeadbeefcafef00dULL) << 64) | UInt128(0x0011223344556677ULL);
    m.writer_epoch = 7;
    m.hostname = "host-9.example.com";
    m.pid = 4242;
    m.seq = 13;
    m.expires_at_ms = 1700000030000ULL;

    const std::string msg = mountDoubleStartMessage("replica-a", m);

    /// Identity / existing-holder fields.
    EXPECT_NE(msg.find("server_root_id"), std::string::npos);
    EXPECT_NE(msg.find("'replica-a'"), std::string::npos);
    EXPECT_NE(msg.find("hostname=host-9.example.com"), std::string::npos);
    EXPECT_NE(msg.find("pid=4242"), std::string::npos);
    EXPECT_NE(msg.find("last_seq=13"), std::string::npos);
    EXPECT_NE(msg.find("expires_at_ms=1700000030000"), std::string::npos);
    /// New wait-aware remediation (this server already waited; the lease kept being renewed).
    EXPECT_NE(msg.find("waited"), std::string::npos);
    EXPECT_NE(msg.find("unique"), std::string::npos);
    EXPECT_NE(msg.find("reclaim the mount on restart"), std::string::npos);
    EXPECT_NE(msg.find("uuid file"), std::string::npos);
}
```

- [ ] **Step 2: Run test to verify it fails**

Build the unit test target (redirect + subagent-summarize the log), then run:

```bash
ninja -C build unit_tests_dbms > build/build_cas_mount_msg.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMountMessage.*' > build/test_cas_mount_msg.log 2>&1
```
Expected: compile FAILS — `mountDoubleStartMessage` is not declared.

- [ ] **Step 3: Add the declaration**

In `CasServerRoot.h`, immediately after the `claimMount(...)` declaration (after line 162, before the `MountLeaseKeeper` doc comment):

```cpp
/// Format the operator-actionable startup error shown when the mount lease is held by a genuinely
/// live second server (the same `server_root_id` is mounted twice). Produced only AFTER this server
/// has already waited for the lease to lapse (see `claimMountAwaitingExpiry`) and it did not — so the
/// remediation is about a live twin, not about waiting.
String mountDoubleStartMessage(const String & srid, const MountLease & existing);
```

- [ ] **Step 4: Add the definition**

At the top of `CasServerRoot.cpp` add the include (next to the existing CAS Core includes):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
```

Add the definition (place it just above `claimMount`, after the anonymous-namespace `makeMountBody` helper closes):

```cpp
String mountDoubleStartMessage(const String & srid, const MountLease & existing)
{
    return fmt::format(
        "Content-addressed disk cannot start: server_root_id '{}' is actively mounted by another LIVE server.\n"
        "  Existing mount: server_uuid={} hostname={} pid={} last_seq={} expires_at_ms={}\n"
        "This server already waited for the mount lease to lapse, but it kept being renewed — a second\n"
        "server is holding the same CAS namespace. This prevents two ClickHouse servers from writing it.\n"
        " - If the other server is running intentionally, configure a unique <server_root_id> for this disk.\n"
        " - If the other server is a stale/zombie process, stop it; this server will then reclaim the mount on restart.\n"
        " - If the local ClickHouse uuid file was regenerated, restore the old uuid file, or remove the stale\n"
        "   owner object gc/server-roots/{}/owner only after verifying no server uses this root.",
        srid, u128ToHex(existing.server_uuid), existing.hostname, existing.pid,
        existing.seq, existing.expires_at_ms, srid);
}
```

Note: `CasServerRoot.cpp` already uses `fmt`-style formatting via `Exception`; include `<fmt/format.h>` if `fmt::format` is not already transitively available (check the compile error and add it only if needed).

- [ ] **Step 5: Run test to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/build_cas_mount_msg.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMountMessage.*' > build/test_cas_mount_msg.log 2>&1
```
Expected: `[  PASSED  ] 1 test.` (use a subagent to summarize each log.)

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_mount.cpp
git commit -m "CAS mount: extract tested mountDoubleStartMessage helper (S13)"
```

---

### Task 2: `claimMountAwaitingExpiry` — bounded wait-for-expiry reclaim

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h` (declaration after `claimMount` / `mountDoubleStartMessage`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp` (definition below `claimMount`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `claimMount(Backend&, const Layout&, const String&, UInt128, uint64_t, uint64_t now_ms, uint64_t ttl_ms) -> MountClaimResult` (unchanged); `MountClaimResult` (`Claimed`/`LiveDoubleStart`/`ForeignOwner`) + its `.body` (a `MountLease`).
- Produces:
  ```cpp
  MountClaimResult claimMountAwaitingExpiry(
      Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
      const std::function<uint64_t()> & now_ms_fn,
      uint64_t ttl_ms, uint64_t poll_interval_ms, uint64_t margin_ms,
      const std::function<void(uint64_t)> & sleep_ms_fn,
      const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {});
  ```
  Returns `Claimed` (reclaimed), `ForeignOwner` (immediate), or `LiveDoubleStart` (waited out the bound — a live twin). `on_wait_start` (default no-op) is fired ONCE with the observed lease + the latched `wait_deadline_ms` when the function decides to wait, so `Store::open` can log it. Task 3 consumes this from `Store::open`.

- [ ] **Step 1: Write the failing tests**

Add to `src/Disks/tests/gtest_cas_mount.cpp`:

```cpp
TEST(CasMountAwaitExpiry, PastExpiryReclaimsImmediatelyNoSleep)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A prior incarnation (uuid=1, epoch=7) claimed a lease live until 1100.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1200;                 // already past 1100 → the stale lease is dead
    int sleeps = 0;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 25, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    EXPECT_EQ(sleeps, 0);                                             // decided on the first attempt
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 8u);   // reclaimed as us
}

TEST(CasMountAwaitExpiry, FutureExpiryReclaimsAfterClockAdvances)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;                 // lease live until 1100, holder does NOT renew
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 50, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    const auto body = decodeMountLease(b->get(l.mountKey("r"))->bytes);
    EXPECT_EQ(body.writer_epoch, 8u);
    EXPECT_EQ(body.seq, 2u);                                         // reclaim continues seq (prev 1 + 1)
}

TEST(CasMountAwaitExpiry, LiveRenewingTwinTimesOutAsDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    auto now_fn = [&] { return now; };
    /// Each poll: time advances AND the live holder (uuid=1, epoch=7) renews its own lease.
    auto sleep_fn = [&](uint64_t ms)
    {
        now += ms;
        ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, now, 100).kind, MountClaimResult::Claimed);
    };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 20, /*margin*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);   // still the holder's
}

TEST(CasMountAwaitExpiry, ForeignUuidFailsClosedImmediately)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A foreign server (uuid=2) holds the mount.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(2), 1, /*now*/ 1000, /*ttl*/ 100).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    int sleeps = 0;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; ++sleeps; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 25, /*margin*/ 25, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::ForeignOwner);
    EXPECT_EQ(sleeps, 0);                                            // never waits across UUIDs
}

TEST(CasMountAwaitExpiry, SkewedFarFutureExpiryIsCappedAtTtlPlusMargin)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    /// A prior incarnation stamped a far-future expiry (killer clock ahead): live until 1000 + 100000,
    /// but the holder is dead (never renews). The wait must be capped at ~ttl + margin, not block to
    /// the absurd expiry, and fail closed (LiveDoubleStart) rather than reclaim a still-live-looking lease.
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), 7, /*now*/ 1000, /*ttl*/ 100000).kind, MountClaimResult::Claimed);

    uint64_t now = 1000;
    auto now_fn = [&] { return now; };
    auto sleep_fn = [&](uint64_t ms) { now += ms; };

    const auto r = claimMountAwaitingExpiry(
        *b, l, "r", UInt128(1), /*our_epoch*/ 8, now_fn, /*ttl*/ 100, /*poll*/ 20, /*margin*/ 20, sleep_fn);
    EXPECT_EQ(r.kind, MountClaimResult::LiveDoubleStart);
    EXPECT_LE(now, 1000u + 100u + 20u + 20u);                        // bounded ≈ start + ttl + margin (+ one poll)
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);   // not reclaimed
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
ninja -C build unit_tests_dbms > build/build_cas_mount_await.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMountAwaitExpiry.*' > build/test_cas_mount_await.log 2>&1
```
Expected: compile FAILS — `claimMountAwaitingExpiry` is not declared. (Subagent-summarize the logs.)

- [ ] **Step 3: Add the declaration**

In `CasServerRoot.h`, after the `mountDoubleStartMessage` declaration from Task 1:

```cpp
/// Bounded wait-for-expiry mount claim (S13 crash-recovery). Wraps `claimMount`:
///   - first attempt decided immediately for `Claimed` (reclaimed / adopted) or `ForeignOwner`;
///   - a `LiveDoubleStart` from OUR OWN uuid (a stale lease from a prior incarnation of this server)
///     is waited out: poll every `poll_interval_ms` (advancing wall-clock via `now_ms_fn`, sleeping via
///     `sleep_ms_fn`) until the lease lapses and we reclaim it (`Claimed`), or the wait bound elapses.
/// The wait bound is latched ONCE from the first observed `expires_at_ms + margin_ms`, capped so we never
/// block longer than `now + ttl_ms + margin_ms` (bounds a forward-clock-skewed expiry). On timeout the
/// last `LiveDoubleStart` is returned (a genuinely live second server). The reclaim inside `claimMount`
/// is token-guarded, so a holder that renews after our read can never be stolen from — correctness does
/// not depend on the poll interval. `now_ms_fn` / `sleep_ms_fn` are injected so tests drive a fake clock
/// with no real sleeping. `on_wait_start` (default no-op) is invoked once, with the observed lease and
/// the latched wait deadline, when the function decides to wait — for an operator-visible startup log.
MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    const std::function<uint64_t()> & now_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms, uint64_t margin_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn,
    const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {});
```

- [ ] **Step 4: Add the definition**

In `CasServerRoot.cpp`, directly below the `claimMount` definition (after its closing brace, ~line 328):

```cpp
MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    const std::function<uint64_t()> & now_ms_fn,
    uint64_t ttl_ms, uint64_t poll_interval_ms, uint64_t margin_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn,
    const std::function<void(const MountLease &, uint64_t)> & on_wait_start)
{
    /// A zero poll interval would spin; a single-ms floor keeps the loop a real (bounded) wait.
    const uint64_t poll = poll_interval_ms == 0 ? 1 : poll_interval_ms;

    MountClaimResult r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms);
    if (r.kind != MountClaimResult::LiveDoubleStart)
        return r;

    /// A same-uuid, different-epoch, still-live lease from a prior incarnation of THIS server. It is
    /// either our own crashed process (its keeper died without releasing the lease) or a genuinely live
    /// twin. Wait for the lease to lapse — a live twin keeps renewing and never lapses, so we time out
    /// and report it; a dead predecessor lapses within its TTL and we reclaim (token-guarded).
    const uint64_t start_ms = now_ms_fn();
    uint64_t wait_deadline = r.body.expires_at_ms + margin_ms;
    const uint64_t cap = start_ms + ttl_ms + margin_ms;
    if (wait_deadline > cap)
        wait_deadline = cap;

    if (on_wait_start)
        on_wait_start(r.body, wait_deadline);

    while (now_ms_fn() < wait_deadline)
    {
        sleep_ms_fn(poll);
        r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms);
        if (r.kind != MountClaimResult::LiveDoubleStart)
            return r;
    }

    /// Timed out still LiveDoubleStart → a genuinely live second server holds the mount.
    return r;
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
ninja -C build unit_tests_dbms > build/build_cas_mount_await.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMountAwaitExpiry.*' > build/test_cas_mount_await.log 2>&1
```
Expected: `[  PASSED  ] 5 tests.` (subagent-summarize).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_mount.cpp
git commit -m "CAS mount: bounded wait-for-expiry reclaim (S13) + unit tests"
```

---

### Task 3: Wire the wait into `Store::open` and reframe the Store-level tests

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp:170-186` (step 4 of the mount-safety startup protocol)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (replace `CasMountStartup.LiveDoubleStartErrorTextHasRemediation`)

**Interfaces:**
- Consumes: `claimMountAwaitingExpiry(...)` and `mountDoubleStartMessage(...)` (Tasks 1–2); `config.mount_lease_ttl_ms`, `config.mount_renew_period` (`CasStore.h`); the existing `now_ms` lambda in `Store::open`.
- Produces: `Store::open` self-recovers a stale self-mount; throws `ABORTED` with `mountDoubleStartMessage` only on a genuinely live twin (wait timeout) and `ForeignOwner`.

- [ ] **Step 1: Write/replace the failing test**

In `src/Disks/tests/gtest_cas_mount.cpp`, DELETE the entire `TEST(CasMountStartup, LiveDoubleStartErrorTextHasRemediation)` (its premise — a live-but-not-renewing lease throwing synchronously — is exactly the behavior this fix converts into self-recovery; the error text is now covered by `CasMountMessage.DoubleStartTextHasIdentityAndRemediation`). Replace it with a self-recovery regression:

```cpp
TEST(CasMountStartup, StaleSelfMountReclaimedAfterWait)
{
    auto b = std::make_shared<InMemoryBackend>();

    /// Server A opens writable with a SHORT lease TTL and KEEPS its Store alive with NO background
    /// renewer (background_watermark defaults false) — i.e. it simulates a crashed process: the mount
    /// lease survives with a future expires_at_ms but is never renewed.
    auto a = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
        .mount_lease_ttl_ms = std::chrono::milliseconds(300),
        .mount_renew_period = std::chrono::milliseconds(100)});
    ASSERT_NE(a, nullptr);
    const uint64_t e1 = a->writerEpoch();

    /// A restart of the SAME server (same uuid) must NOT abort: it waits out the stale lease (≤ ~300ms)
    /// and reclaims the mount, coming up with a strictly higher durable writer_epoch.
    StorePtr a2;
    EXPECT_NO_THROW(
        a2 = Store::open(b, PoolConfig{
            .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "r", .root_shards = 1,
            .mount_lease_ttl_ms = std::chrono::milliseconds(300),
            .mount_renew_period = std::chrono::milliseconds(100)}));
    ASSERT_NE(a2, nullptr);
    EXPECT_GT(a2->writerEpoch(), e1);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/build_cas_store_recover.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMountStartup.StaleSelfMountReclaimedAfterWait' > build/test_cas_store_recover.log 2>&1
```
Expected: FAIL — `Store::open` still throws `ABORTED` ("actively mounted by another server") because it does a one-shot `claimMount`. (Subagent-summarize.)

- [ ] **Step 3: Implement the wiring in `Store::open`**

In `CasStore.cpp`, replace the current step-4 block (the `claimMount(...)` call + the `if (claim.kind != MountClaimResult::Claimed) throw Exception(...)` with its inline multi-line string, lines ~170–186) with:

```cpp
        const uint64_t ttl_ms = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());
        /// Poll twice per renew period so a live holder's renewal is always observed within the wait;
        /// margin = one poll interval (covers poll granularity + minor wall-clock skew). Derived from
        /// existing config — no new knob (spec §Config).
        const uint64_t poll_interval_ms = std::max<uint64_t>(
            1, static_cast<uint64_t>(store->config.mount_renew_period.count()) / 2);
        const uint64_t margin_ms = poll_interval_ms;
        const auto sleep_ms = [](uint64_t ms)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };
        /// Operator-visible log the moment startup decides to wait out a stale self-mount (the disk-open
        /// path blocks up to ~ttl here, so a silent block would be confusing).
        const auto on_wait_start = [&srid](const MountLease & held, uint64_t wait_deadline_ms)
        {
            LOG_INFO(getLogger("CasStore"),
                "CAS mount '{}': a stale mount lease is held by uuid={} epoch={} pid={} hostname={} "
                "(expires_at_ms={}); waiting for it to lapse, then reclaiming. If a second server is "
                "genuinely live, startup will abort once the wait bound (wait_deadline_ms={}) elapses.",
                srid, u128ToHex(held.server_uuid), held.writer_epoch, held.pid, held.hostname,
                held.expires_at_ms, wait_deadline_ms);
        };

        /// S13 crash-recovery: a hard-killed prior incarnation leaves a stale, unreleased mount lease.
        /// Rather than aborting, wait (bounded by ttl + margin) for that lease to lapse and reclaim it;
        /// a genuinely live second server keeps renewing and is reported as LiveDoubleStart. The reclaim
        /// is token-guarded (see claimMountAwaitingExpiry), so a live twin is never stolen from.
        const MountClaimResult claim = claimMountAwaitingExpiry(
            *store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
            [&now_ms]() { return now_ms(); }, ttl_ms, poll_interval_ms, margin_ms, sleep_ms, on_wait_start);
        if (claim.kind != MountClaimResult::Claimed)
        {
            /// LiveDoubleStart (waited out the bound → a live twin) or ForeignOwner → fail closed.
            throw Exception(ErrorCodes::ABORTED, "{}", mountDoubleStartMessage(srid, claim.body));
        }
```

Notes:
- `now_ms` is the wall-clock lambda already defined just above (`CasStore.cpp:164-168`); the wrapper `[&now_ms]{ return now_ms(); }` adapts it to `std::function<uint64_t()>`.
- `#include <thread>` and `<algorithm>` are ALREADY present (`CasStore.cpp:11-12`); `getLogger`/`LOG_INFO` and `u128ToHex` are already used in this file. No new includes needed.
- Call the free functions UNQUALIFIED (no `Cas::`): the `Store` methods are defined inside `namespace DB::Cas`, exactly as the existing `claimMount` / `claimOwnerOrThrow` / `allocateWriterEpoch` calls at `CasStore.cpp:154-170`.
- `mountDoubleStartMessage` is fed `claim.body`, which for `LiveDoubleStart`/`ForeignOwner` is the observed existing lease (identity fields populated), matching the old message's fields.

- [ ] **Step 4: Run the focused test + the full mount suite**

```bash
ninja -C build unit_tests_dbms > build/build_cas_store_recover.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMount*:CasServerRoot*' > build/test_cas_mount_all.log 2>&1
```
Expected: all `CasMount*` / `CasServerRoot*` tests PASS, including `StaleSelfMountReclaimedAfterWait`, `WriterEpochStrictlyIncreasesAcrossReopen` (clean-shutdown reopen still reclaims immediately — its lease was retired by `terminate`, so the first `claimMount` attempt returns `Claimed` with no wait), and `SecondServerSameRootFailsClosed` (foreign uuid still fails at the owner gate). (Subagent-summarize; confirm no test now hangs for ~30s — the reframed test uses a 300ms TTL.)

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_mount.cpp
git commit -m "CAS mount: Store::open self-recovers a stale self-mount (S13)"
```

---

### Task 4: Restore real hard-KILL of CH replicas in the soak chaos harness

**Files:**
- Modify: `utils/ca-soak/soak/chaos.py:68-74` (the `CA_SOAK_NO_HARD_KILL` downgrade block)

**Interfaces:**
- Consumes: nothing new.
- Produces: the chaos schedule again issues real `docker kill -s KILL` for CH replicas (RustFS keeps its B145 graceful-only scoping, which is unrelated to S13).

- [ ] **Step 1: Remove the S13 KILL→RESTART downgrade**

In `utils/ca-soak/soak/chaos.py`, delete the block added for S13 (the `if _os.environ.get("CA_SOAK_NO_HARD_KILL") ...` downgrade, lines ~68–74, including its `import os as _os`). The RustFS-specific downgrade just above it (the `if target == FaultTarget.RUSTFS and action == FaultAction.KILL:` block, B145) stays. After the edit, a CH replica `KILL` slot remains a real hard kill followed by `docker start` (see `apply_fault`).

- [ ] **Step 2: Verify the module still imports and the schedule contains real CH KILLs**

```bash
cd utils/ca-soak && python3 -c "
from soak.chaos import generate_chaos_schedule, FaultTarget, FaultAction
s = generate_chaos_schedule(seed=20260702, duration_s=3600, mean_interval_s=90)
ch_kills = [f for f in s if f.target in (FaultTarget.CH1, FaultTarget.CH2, FaultTarget.BOTH) and f.action == FaultAction.KILL]
print('total faults:', len(s), 'CH hard-kills:', len(ch_kills))
rustfs_kills = [f for f in s if f.target == FaultTarget.RUSTFS and f.action == FaultAction.KILL]
print('rustfs hard-kills (must be 0):', len(rustfs_kills))
assert len(ch_kills) > 0, 'expected real CH hard-kills after removing the downgrade'
assert len(rustfs_kills) == 0, 'RustFS must never be hard-killed (B145)'
print('OK')
"
cd ../..
```
Expected: `CH hard-kills: > 0`, `rustfs hard-kills (must be 0): 0`, `OK`.

- [ ] **Step 3: Commit**

The ca-soak suite is the user's working tree; committing `chaos.py` is explicitly in scope for this task (it restores the behavior S13 disabled). Only stage this one file:

```bash
git add utils/ca-soak/soak/chaos.py
git commit -m "ca-soak: restore real hard-KILL of CH replicas (S13 fixed)"
```

---

### Task 5: TLA+ regression + full build verification

**Files:** none modified. Verifies `docs/superpowers/models/CaCasMountCore.tla` stays GREEN (unchanged) and the server binary builds.

**Interfaces:** none.

- [ ] **Step 1: Run the mount model regression**

The model already proves the reclaim safe (`ClaimMount` expired branch + `W_SameUuidReclaimsExpired` witness); the code change requires NO model edit. Re-run it to confirm the positive gate + 3 sabotage configs are still GREEN:

```bash
cd docs/superpowers/models && bash run_mount.sh > /tmp/run_mount.log 2>&1; tail -40 /tmp/run_mount.log; cd ../../..
```
Expected: the positive config reports no invariant violation (`Model checking completed. No error has been found.`) and each sabotage config reports its expected violation (`ForeignUuidNeverAutoTakesOver`, `WriterEpochMonotoneUnique`, `SupersededWriterMakesNoMutation`). Use a subagent to summarize `run_mount.log` if long. (If `run_mount.sh` prints its own PASS/FAIL summary, that summary is authoritative.)

- [ ] **Step 2: Full server build**

Confirm the change compiles into the server binary (not just the unit test target):

```bash
ninja -C build clickhouse > build/build_cas_mount_server.log 2>&1
```
Expected: build succeeds. Use a subagent to summarize `build/build_cas_mount_server.log` and report only errors/warnings related to the change.

- [ ] **Step 3: Final focused unit-test sweep**

```bash
build/src/unit_tests_dbms --gtest_filter='CasMount*:CasServerRoot*' > build/test_cas_mount_final.log 2>&1
```
Expected: all PASS. (Subagent-summarize.)

- [ ] **Step 4: No commit**

This task changes no files — nothing to commit. Record the model + build results in the task ledger / final review notes. (The soak re-run of scenario S13 against the rebuilt binary is an integration follow-up, tracked in the spec's Testing section; it is not part of this plan's committable unit-test deliverable.)

---

## Notes for the executor

- **Do not** modify `claimMount`, the owner/epoch/watermark logic, or any `.tla`/`.cfg`.
- The whole wait is deterministically testable at the free-function level (Task 2) via injected clock/sleep — that is where the live-twin / cap / foreign behavior is proven. `Store::open` (Task 3) is thin wiring, tested for self-recovery with a short (300ms) real-clock TTL.
- If any existing `CasMount*` test other than the deleted one needs adjustment, that is a signal the behavior change is broader than intended — stop and re-check against the spec before editing it.
