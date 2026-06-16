# CA GC Lease Advisory-Heartbeat Implementation Plan (B160)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Stop the GC false-steal livelock (B160) by adding an advisory `gc/hb` heartbeat that gates the lease steal, so a slow-but-alive leader (its `lease.seq` frozen for the round) is never stolen from.

**Architecture:** A new `gc/hb` register `{owner, hb_seq}`, bumped on a fast cadence by a heartbeat thread in `CasGcScheduler` while this node holds the lease. `Gc::acquireOrRenewLease` reads `gc/hb` in the foreign-owner-frozen branch and backs off (no steal) if the heartbeat advanced. Ownership/epoch and the atomic single-CAS steal on `gc/state` are unchanged (TLA+-proven safe in `CaGcLeaseCore.tla`).

**Tech stack:** C++ (Allman braces), gtest (`unit_tests_dbms`). Spec: `docs/superpowers/specs/2026-06-16-ca-gc-lease-heartbeat-design.md`. Model: `CaGcLeaseCore.tla` (TLC-green).

**Build/test (per CLAUDE.md — redirect to log, no `-j`/`nproc`):**
`cd build && ninja unit_tests_dbms > build_b160.log 2>&1` ; `build/src/unit_tests_dbms --gtest_filter='CasGc*:CaWiring*'`

---

### Task 1: `gc/hb` key + `GcHeartbeat` codec

**Files:** `Core/CasLayout.h`, `Core/CasGcFormats.h`, `Core/CasGcFormats.cpp`, test `Disks/tests/gtest_cas_codecs.cpp`.

- [ ] **Step 1: layout key.** In `CasLayout.h`, after `gcStateKey()`:
```cpp
    /// GC heartbeat (advisory liveness pulse; B160): <prefix>/gc/hb.
    String gcHbKey() const
    {
        return prefix + "/gc/hb";
    }
```

- [ ] **Step 2: struct + codec decl.** In `CasGcFormats.h`, after the `GcState` decls:
```cpp
/// Advisory GC liveness pulse (B160). A leader bumps `hb_seq` on a fast cadence independent of
/// round progress; a follower's steal backs off if it sees this advance. Fixed 24-byte binary:
/// 16-byte big-endian owner + 8-byte big-endian hb_seq.
struct GcHeartbeat
{
    UInt128 owner{};
    uint64_t hb_seq = 0;
};
String encodeGcHeartbeat(const GcHeartbeat & hb);
GcHeartbeat decodeGcHeartbeat(std::string_view data);
```

- [ ] **Step 3: codec impl.** In `CasGcFormats.cpp` (add `#include <Common/Exception.h>` + the `CORRUPTED_DATA` extern if not present):
```cpp
String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    String out(24, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(hb.owner >> (8 * (15 - i))));
    for (int i = 0; i < 8; ++i)
        out[16 + i] = static_cast<char>(static_cast<UInt8>(hb.hb_seq >> (8 * (7 - i))));
    return out;
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    if (data.size() != 24)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: expected 24 bytes, got {}", data.size());
    GcHeartbeat hb;
    for (int i = 0; i < 16; ++i)
        hb.owner = (hb.owner << 8) | static_cast<UInt8>(data[i]);
    for (int i = 0; i < 8; ++i)
        hb.hb_seq = (hb.hb_seq << 8) | static_cast<UInt8>(data[16 + i]);
    return hb;
}
```

- [ ] **Step 4: round-trip test** in `gtest_cas_codecs.cpp`:
```cpp
TEST(CasGcHeartbeatCodec, RoundTrip)
{
    GcHeartbeat hb;
    hb.owner = (UInt128(0xab) << 64) | UInt128(0xcd);
    hb.hb_seq = 12345;
    GcHeartbeat d = decodeGcHeartbeat(encodeGcHeartbeat(hb));
    EXPECT_EQ(d.owner, hb.owner);
    EXPECT_EQ(d.hb_seq, 12345u);
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeGcHeartbeat(String("short")); });
}
```

- [ ] **Step 5: build + test + commit.** `ninja unit_tests_dbms`; `--gtest_filter='CasGcHeartbeatCodec.*'`; commit `git commit -m "CA B160: gc/hb key + GcHeartbeat codec"`.

---

### Task 2: heartbeat-gated steal + `Gc::pulseHeartbeat`

**Files:** `Core/CasGc.h`, `Core/CasGc.cpp`.

- [ ] **Step 1: observation members + pulse decl.** In `CasGc.h`, after `last_seen_seq`:
```cpp
    /// B160: the heartbeat observed alongside the lease (gates the steal).
    UInt128 last_seen_hb_owner{};
    uint64_t last_seen_hb_seq = 0;
```
and in the public section (near `runRegularRound`):
```cpp
    /// B160 advisory heartbeat: bump <prefix>/gc/hb to {gc_id, hb_seq+1}. Best-effort (a lost CAS
    /// is harmless — the next pulse retries). Touches NO Gc instance state, so the scheduler's
    /// separate heartbeat thread may call it concurrently with the round thread. Static by design.
    static void pulseHeartbeat(Store & store, UInt128 gc_id);
```

- [ ] **Step 2: pulse impl** in `CasGc.cpp`:
```cpp
void Gc::pulseHeartbeat(Store & store, UInt128 gc_id)
{
    const String key = store.layout().gcHbKey();
    const auto got = store.backend().get(key);
    GcHeartbeat hb;
    std::optional<Token> expected;
    if (got)
    {
        hb = decodeGcHeartbeat(got->bytes);
        expected = got->token;
    }
    hb.owner = gc_id;        /// we believe we are the leader; take/keep gc/hb ownership
    ++hb.hb_seq;
    /// Best-effort: a Conflict means another writer raced us; skip, the next pulse retries.
    store.backend().casPut(key, encodeGcHeartbeat(hb), expected, /*out_token=*/nullptr);
}
```

- [ ] **Step 3: heartbeat-gated steal.** In `acquireOrRenewLease`, replace the foreign-owner branch (the `incumbent_renewed` block) with:
```cpp
        /// Foreign owner. Read the advisory heartbeat (B160): a leader bumps gc/hb on a fast cadence
        /// independent of round progress, so a slow-but-alive leader (lease.seq frozen for the round)
        /// is still seen as alive here and is NOT stolen from. gc/hb absent / foreign-owner pulse =>
        /// no signal => fall back to the seq-only observation (never worse than before).
        GcHeartbeat hb;
        if (const auto hb_got = store->backend().get(store->layout().gcHbKey()))
            hb = decodeGcHeartbeat(hb_got->bytes);
        const bool hb_alive = has_observation
            && hb.owner == current.lease.owner
            && hb.hb_seq > last_seen_hb_seq;

        const bool incumbent_renewed = !has_observation
            || current.lease.owner != last_seen_owner
            || current.lease.seq != last_seen_seq;
        if (incumbent_renewed || hb_alive)
        {
            /// Alive (renewed, or heartbeat advanced) => record the observation and back off.
            rememberObservation(current.lease);
            last_seen_hb_owner = hb.owner;
            last_seen_hb_seq = hb.hb_seq;
            return false;
        }

        /* fall through to the EXISTING Step 4 steal (owner=gc_id, seq++, fence_seq++) unchanged */
```
Keep the existing steal CAS that follows verbatim. (`GcHeartbeat` is from `CasGcFormats.h`, already transitively included via `CasGc.h`/`CasGcFormats.h`; add `#include` if the build complains.)

- [ ] **Step 4: build dbms** `ninja dbms > build_b160_core.log 2>&1`; commit `git commit -m "CA B160: heartbeat-gated lease steal + Gc::pulseHeartbeat"`.

---

### Task 3: heartbeat thread in `CasGcScheduler`

**Files:** `CasGcScheduler.h`, `CasGcScheduler.cpp`.

- [ ] **Step 1: members.** In `CasGcScheduler.h` private section:
```cpp
    void heartbeatLoop();

    std::atomic<bool> i_am_leader{false};
    std::thread hb_thread;
    const std::chrono::milliseconds hb_interval;   /// H <= W (= interval/4)
```
Add `#include <Common/logger_useful.h>` is already pulled via CasStore.h; ensure `<atomic>` (already included).

- [ ] **Step 2: ctor sets `hb_interval`.** In `CasGcScheduler.cpp` ctor init list, after `gc_id(...)`:
```cpp
    , hb_interval(std::max<std::chrono::milliseconds>(
          std::chrono::milliseconds(50),
          std::chrono::duration_cast<std::chrono::milliseconds>(interval_) / 4))
```

- [ ] **Step 3: start/stop the hb thread.** In `start()`, after launching `thread`:
```cpp
    hb_thread = std::thread([this] { heartbeatLoop(); });
```
In `stop()`, after `thread.join()` (the `wake.notify_all()` + `stopping` already cover both loops):
```cpp
    if (hb_thread.joinable())
        hb_thread.join();
```

- [ ] **Step 4: round thread sets leadership.** In `loop()`, in the `try` after `runRegularRound`, set the flag on both branches:
```cpp
            i_am_leader.store(report.acquired_lease, std::memory_order_relaxed);
```
(place right after `const Cas::RoundReport report = gc.runRegularRound();`) and on the `catch (...)` path set it false:
```cpp
            i_am_leader.store(false, std::memory_order_relaxed);
```

- [ ] **Step 5: heartbeat loop.**
```cpp
void CasGcScheduler::heartbeatLoop()
{
    while (true)
    {
        {
            std::unique_lock lock(mutex);
            if (wake.wait_for(lock, hb_interval, [this] { return stopping; }))
                return;
        }
        if (!i_am_leader.load(std::memory_order_relaxed))
            continue;
        try
        {
            Cas::Gc::pulseHeartbeat(*store, gc_id);
        }
        catch (...)
        {
            tryLogCurrentException(log, "CA GC heartbeat pulse failed (advisory; will retry)");
        }
    }
}
```

- [ ] **Step 6: build + commit.** `ninja dbms`; commit `git commit -m "CA B160: heartbeat thread in CasGcScheduler"`.

---

### Task 4: unit tests — no-false-steal + still-fails-over

**Files:** `Disks/tests/gtest_cas_gc_round.cpp` (the existing `CasGcLease` suite).

- [ ] **Step 1: tests.** Add (mirroring `CasGcLease.FreshPoolAcquiresAndRenews` setup — `openTestStore`, `Gc`, `hexToU128`):
```cpp
TEST(CasGcLease, HeartbeatBlocksFalseStealOfAliveLeader)
{
    std::shared_ptr<InMemoryBackend> b;
    StorePtr s = openTestStore(b);
    Gc a(s, hexToU128("00000000000000000000000000000001"));
    Gc f(s, hexToU128("00000000000000000000000000000002"));

    EXPECT_TRUE(a.runRegularRound().acquired_lease);   // A leads (seq=1)
    EXPECT_FALSE(f.runRegularRound().acquired_lease);   // F observes (A,1), backs off

    // A is mid-round: seq stays frozen, but it heartbeats.
    Gc::pulseHeartbeat(*s, hexToU128("00000000000000000000000000000001"));

    // F sees (A,1) frozen BUT the heartbeat advanced -> must NOT steal.
    EXPECT_FALSE(f.runRegularRound().acquired_lease);
    EXPECT_FALSE(f.runRegularRound().acquired_lease);   // still no steal while A keeps... (one pulse is enough: hb_seq advanced once vs the recorded 0)
}

TEST(CasGcLease, FrozenHeartbeatStillAllowsFailoverSteal)
{
    std::shared_ptr<InMemoryBackend> b;
    StorePtr s = openTestStore(b);
    Gc a(s, hexToU128("00000000000000000000000000000001"));
    Gc f(s, hexToU128("00000000000000000000000000000002"));

    EXPECT_TRUE(a.runRegularRound().acquired_lease);   // A leads
    EXPECT_FALSE(f.runRegularRound().acquired_lease);   // F observes
    // A dead: no renew, no heartbeat. (gc/hb absent.)
    EXPECT_TRUE(f.runRegularRound().acquired_lease);    // F steals (legitimate failover)
}
```
NOTE on test 1's 3rd assertion: after one pulse `hb_seq=1`; F's 2nd round records `last_seen_hb_seq=1`; a 3rd round with NO further pulse would see hb frozen at 1 == last_seen -> NOT advanced -> would steal. So either pulse again before the 3rd call, or drop the 3rd assertion. Use TWO pulses if keeping it, or keep just the single no-steal assertion. (Implementer: keep it correct — pulse before each re-check, or assert only once.)

- [ ] **Step 2: build + run.** `ninja unit_tests_dbms`; `--gtest_filter='CasGcLease.*'` → all pass.

- [ ] **Step 3: full CA suite.** `--gtest_filter='Cas*:CaWiring*'` → only the pre-existing B140 `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` red.

- [ ] **Step 4: commit** `git commit -m "CA B160: lease heartbeat unit tests (no-false-steal + failover)"`.

---

### Task 5 (after merge): soak validation
Rebuild `clickhouse`, run the two-replica soak, confirm GC retire-contention drops ~70-80% → ~0 and `gc/state.round` advances steadily. (Separate; the harness exists.)

## Notes
- The heartbeat thread shares the scheduler's `mutex`/`wake` cv only for the stop signal; `pulseHeartbeat` touches no `Gc` instance state, so it's safe alongside the round thread.
- `i_am_leader` is `std::atomic` (set by the round thread, read by the heartbeat thread); a stale read costs at most one extra/missing pulse, which the observation protocol tolerates.
