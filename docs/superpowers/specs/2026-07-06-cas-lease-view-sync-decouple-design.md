# CAS: Decouple Lease Renewal from Retired-View Sync — Design

**Date:** 2026-07-06
**Status:** Approved for planning
**Scope:** P3.1 Task 5 (the liveness half of the mount-lease fence-recovery work). The
correctness half (fence-costs-an-epoch, bounded open retry, `MountFencedException`) already
landed on `cas-gc-part-manifest-impl` / the P3.1 branch; this design removes the *cause* of the
lease expiry that made those recovery paths fire.

Related:
- `docs/superpowers/worklogs/2026-07-06-p31-mount-lease-root-cause.md` (root cause, CONFIRMED)
- `docs/superpowers/specs/2026-07-06-cas-mount-lease-fence-recovery-design.md` (vectors A–E; this is vector B)

---

## 1. Problem {#problem}

The per-server merged mount heartbeat (`MountLeaseKeeper`, a `SingleWriterSlot`) runs **one**
background renewal thread per writable mount. On every tick that thread calls `prepareRenew`,
which reads three values and stamps them into the lease body:

- `now_ms` — a local clock read (cheap);
- `minActive()` — one in-memory lock over `active_build_seqs` (cheap);
- `observed_gc_round` — currently wired to `Store::refreshViewForBeat()`, which is the **only**
  expensive part: it does one `gc/state` S3 `GET`, and when a newer GC round has been published
  it takes the exclusive `view_gate` (draining every in-flight `mutateShard`) and runs
  `RetireView::refresh()` — an O(populated-gc-shards) sequence of S3 `GET`s, each decoding a
  `RetiredSet`.

Because this expensive view refresh runs **synchronously on the renewal thread, before the lease
PUT**, a slow refresh delays the renewal. Under S3 retry storms (observed ~19% read-error rate on
RustFS, backoff to minutes) the refresh — or the `view_gate` drain waiting behind a `mutateShard`
that is itself stuck on slow S3 — can push the renewal past the 30 s lease TTL. The lease then
expires while the server is fully alive, and the GC leader legitimately fences it
(`computeHeartbeatFloor` → token-guarded `putOverwrite gc_fenced=true`). That fence is the
liveness root cause behind the P3.1 / S13 wedge.

The name `beat` is overloaded: it labels both the genuine liveness heartbeat (the lease renewal)
and this unrelated view refresh. The two are separated here and named apart.

## 2. Goal {#goal}

Make the lease-renewal cadence **independent of S3 latency**. Renewal must depend only on cheap,
local reads; the S3-heavy retired-view maintenance moves to its own background activity that
tolerates lag. Behavior is identical when S3 is fast; when S3 is slow, the lease no longer expires
from view work.

Non-goals: no change to the fence/adopt/recovery correctness paths (already landed), no change to
the lease body schema, no new config knob (the syncer reuses `mount_renew_period`).

## 3. Architecture {#architecture}

Split the single renewal thread's two jobs into two independent per-mount background activities.

### 3.1 Lease renewal (existing renewal thread) {#lease-renewal}

`MountLeaseKeeper::prepareRenew` keeps reading `now_ms` and `minActive()`, but its third value —
the advertised `observed_gc_round` — now comes from a new cheap reader, `Store::observedGcRound()`,
which returns `retire_view.round()` (the **currently installed** round, an in-memory read). The
renewal thread does **no** S3 view work: it computes three in-memory values and issues one lease
PUT. Its cadence is therefore bounded by the PUT alone.

Consequence: a view/S3 problem can no longer *cause* a renewal failure. Renewal can only fail on
the lease PUT itself (a genuine foreign touch or a real S3 failure of that PUT) — which is exactly
the fail-closed signal the single-writer contract wants. All existing fail-closed / fence-lost /
remount semantics are unchanged.

### 3.2 Retired-view sync (new Store-owned poller thread) {#retired-view-sync}

A new background thread per writable mount periodically runs `Store::syncRetiredView()` — the
verbatim body of today's `refreshViewForBeat`: probe `gc/state`; if the published round is newer
than the installed one, take the exclusive `view_gate`, drain in-flight `mutateShard`, and run
`RetireView::refresh()`; emit one `RetiredViewAdvance` event per *actual* advance (an unchanged
view stays silent).

When the syncer installs a newer round, the **next** renewal advertises it for free through
`observedGcRound()`. This introduces a bounded lag of at most one renewal period between installing
a round and advertising it in the lease body. That lag is conservative and safe: advertising a
*lower* `observed_gc_round` never over-graduates GC — `computeHeartbeatFloor` takes the *min* ack
across live heartbeats, so a briefly-lagging ack only holds the floor back, never advances it past
what a writer has actually installed.

The syncer is Store-owned (methods + a thread member), matching the existing in-Store background
pattern (`scheduleRemount` / `tryRemountOnce` / `remount_thread`) rather than a new class — its
body needs Store internals (`pool_backend`, `pool_layout`, `view_gate`, `retire_view`,
`EventEmitter`), so extracting it would only widen the coupling surface.

### 3.3 Gating and open-ordering {#gating-open}

Both background threads are gated exactly like today's renewer via `background_watermark`
(production only: `context != nullptr && !read_only`; off in unit tests, which drive the
one-shot bodies explicitly).

`Store::open` does **one synchronous `syncRetiredView()` before `mount_keeper->start()`** (the
open-ordering decision already settled in the fence-recovery design), so the first anchored lease
body carries a current ack rather than a stale round-0 one. After the fence is armed, `open` starts
the syncer thread under the same `background_watermark` gate as `startBackground`. The remount path
(`tryRemountOnce`) does the same: one synchronous `syncRetiredView()` feeds the fresh incarnation's
first body, then the syncer restarts.

## 4. Components and files {#components}

### 4.1 `CasStore.h` / `CasStore.cpp`
- Rename `refreshViewForBeat()` → `syncRetiredView()`. Body unchanged (probe, monotone guard,
  drain+refresh, per-advance event). It returns the installed round as before.
- Add `uint64_t observedGcRound()` — returns `retire_view.round()` (cheap; race-safe via
  `RetireView`'s own internal `shared_mutex`, so no Store-level lock is taken).
- Add the syncer: `startRetiredViewSync(std::chrono::milliseconds period)`,
  `stopRetiredViewSync()` (idempotent), `retiredViewSyncLoop(std::chrono::milliseconds period)`,
  and members `ThreadFromGlobalPool retired_view_sync_thread`, a `std::mutex` + `std::condition_variable`
  + `bool retired_view_sync_stop` guarding wakeup/stop — mirroring `SingleWriterSlot::backgroundLoop`
  (a `wait_for(period)` loop that runs the body, `catch(...)`+log, never throws out of the loop).
- Rewire both `MountLeaseKeeper` constructions (`open` ~line 270, remount ~line 504):
  `observed_round_fn = [raw]{ return raw->observedGcRound(); }`.
- `open`: one synchronous `syncRetiredView()` before `mount_keeper->start()`; start the syncer
  after `armMountFence`, under the `background_watermark` gate.
- Teardown / `stop()`: stop the syncer alongside the keeper. The syncer thread MUST be joined
  before `retire_view` / `pool_backend` are destroyed (mirror the `stopBackground` join ordering).
- `tryRemountOnce`: one synchronous `syncRetiredView()` before the fresh keeper `start`, restart
  the syncer with the fresh incarnation.

### 4.2 `CasEvent.h` / `CasEvent.cpp`
- Enum `MountBeat` → `RetiredViewAdvance`.
- String `"mount_beat"` → `"retired_view_advance"`.
- (CAS is pre-release with no persisted event data — the rename needs no compat scaffolding.)

### 4.3 `CasServerRoot.h` / `CasServerRoot.cpp`
- Reword the merged-heartbeat comments: "one beat renews all three" → the renewal PUT stamps the
  clock, the build-watermark floor, and the **last-installed** GC-round ack.
- The `observed_round_fn` constructor-param doc changes from "runs the beat" to "reads the installed
  round"; no signature change (still `std::function<uint64_t()>`).

### 4.4 Docs
Update the `mount_beat` prose mentions to `retired_view_advance`:
- `docs/superpowers/cas/08-testing-and-soak.md`
- `docs/superpowers/cas/ROADMAP.md`
- `docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md`

## 5. Data flow {#data-flow}

- **Renewal thread**, every `mount_renew_period`: `prepareRenew()` → `{ now_ms_fn(), min_active_fn(),
  observed_round_fn() = observedGcRound() }` → `encodeBody` → `putOverwrite`. All in-memory + one PUT.
  No `gc/state` GET.
- **Syncer thread**, every `mount_renew_period`: `syncRetiredView()` → `GET gc/state` → if published
  > installed: exclusive `view_gate` drain + `RetireView::refresh()` (O(shards) GETs) → emit
  `RetiredViewAdvance` on an actual advance.
- **Open / remount** (once, synchronous, bounded by the existing fence-recovery / open retry loop):
  `syncRetiredView()` before keeper `start()`.

## 6. Error handling and lifetime {#error-handling}

- The syncer loop never throws out of itself: the `syncRetiredView` body already swallows
  `gc/state` GET failures and `refresh()` throws with a log and returns the installed round; the
  loop's own `catch(...)` is the backstop. It logs and retries next tick — matching the
  fold-never-throw-on-404 principle. A slow/failing view never wedges renewal (different thread).
- Renewal fail-closed semantics are unchanged; the only behavioral change is that S3/view trouble
  can no longer be the *cause* of a renewal failure.
- `observedGcRound()` reads `retire_view.round()` concurrently with the syncer's
  `RetireView::refresh()`; both are serialized by `RetireView`'s own internal `shared_mutex`, so no
  new Store-level lock is introduced.
- Shutdown: `stopRetiredViewSync()` signals + joins the syncer thread before its dependencies are
  destroyed, exactly as `stopBackground()` joins the renewal thread.

## 7. Testing {#testing}

Unit tests in `src/Disks/tests/gtest_cas_store.cpp`, built in `build/` (SANITIZE=OFF — a deliberate
`LOGICAL_ERROR` throw aborts an ASan build):

1. **Rename** `CasStoreBeat.ViewAdvanceEmitsMountBeatEvent` → the `retired_view_advance` name, and
   assert on `CasEventType::RetiredViewAdvance` / `"retired_view_advance"`.
2. **Renewal is view-independent**: install a backend whose `gc/state` GET blocks or errors; assert
   `renewOnce()` still succeeds and advertises the *installed* (old) round — it neither stalls on
   the view GET nor throws.
3. **Syncer advances independently**: publish a newer `gc/state` round; one `syncRetiredView()` tick
   advances `round()` and emits exactly one `RetiredViewAdvance`; a second tick over an unchanged
   round emits nothing.

Live validation (Task 6 territory, on the soak stand): induce S3 read latency, confirm the renewal
cadence stays ≤ `mount_renew_period` while the syncer lags — the lease never expires and no spurious
`gc_fence_out` fires against a live server.

## 8. Naming migration summary {#naming}

| Old | New | Kind |
| --- | --- | --- |
| `Store::refreshViewForBeat()` | `Store::syncRetiredView()` | one-shot view-refresh body |
| — | `Store::observedGcRound()` | cheap installed-round reader (renewal path) |
| — | `Store::startRetiredViewSync` / `stopRetiredViewSync` / `retiredViewSyncLoop` / `retired_view_sync_thread` | syncer poller |
| `CasEventType::MountBeat` | `CasEventType::RetiredViewAdvance` | event enum |
| `"mount_beat"` | `"retired_view_advance"` | event string |

The lease side keeps its (correct) heartbeat vocabulary: `MountLeaseKeeper`, `renewOnce`,
`mount_renew_period`, `SingleWriterSlot` — all unchanged.
