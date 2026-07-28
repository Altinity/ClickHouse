# CAS fence-window observability + opt-in write grace — design

**Status:** DRAFT (2026-07-28), **rev.2 — silent-expiry path**. Spec only; no code landed. Companion
plan: `docs/superpowers/plans/2026-07-28-cas-fence-observability-and-write-grace.md`.
rev.2 folds in verification against the actual msan incident log
(`tmp/investigate/msan23_server.log`, window 02:02–02:10): the dominant real failure was NOT a
throwing renewal + self-remount, but a **silently hanging** renewal PUT whose fence deadline expired
and later re-armed with **zero** log lines on any transition and **no** remount. Part A gains the
Mode-2 (silent hang/expiry/re-arm) coverage (§A.5); Part B gains the `setMountDeadline` re-arm
signaler (§B.2) it needs to actually wake on that path.
**Branch:** `cas-gc-rebuild`.
**Relationship to prior work:** builds directly on the fence-not-rescue design
(`docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md`, rev.4, IMPLEMENTED)
and the mount-lease self-race fix. That work made a lease holder that cannot confirm its renewal
*fence and self-remount* rather than abort; this work makes that lifecycle **visible** (Part A) and
lets a durable write **ride out** the fence→remount window instead of failing instantly, when the
operator opts in (Part B). Neither part changes the mount/lease/fence/remount protocol.

## Context — the RCA this design answers {#context-rca}

On the CA-over-S3 CI lanes the RustFS object store refused **new** TCP connections in short bursts
(fd exhaustion, fixed separately). One burst overlapped the mount-lease renewal window and the mount
fence tripped for ~2 minutes, refusing durable writes, then recovered.

**Two failure modes exist; the incident was the quieter one.** A mount fence can drop two ways, and
they log very differently:
- **Mode 1 — throwing renewal → self-remount.** A renewal PUT *throws* (a confirmed
  `PreconditionFailed` mismatch, or a budget-exhausted transient), `backgroundLoop` catches it,
  `onRenewFailed → tripMountLost` latches the fence lost, and `scheduleRemount → tryRemountOnce`
  reclaims under a fresh `writer_epoch` (`armMountFence`). This is the fence-not-rescue path, and it
  *does* leave log lines (`CasServerRoot.cpp:1202/:1209` at Error, `CasPool.cpp:1072/:1091`).
- **Mode 2 — silently hanging renewal → deadline expiry → silent re-arm (the observed incident).**
  Verified against `tmp/investigate/msan23_server.log`: across the 02:02–02:10 window (1.57 M Debug,
  1.37 M Trace, 42 k Information, 8 k Error lines — every level is present) there are **zero**
  occurrences of the `CasPool` logger, `"self-remount"`, `CasMountLeaseKeeper`, or any keeper Error
  line. So in the real incident `tryRemountOnce` never ran and the keeper never threw. The shape was:
  the renewal `putOverwrite` **hung** inside its own CAS request-budget retry loop (RustFS refused
  *new* connections; nothing threw, so `backgroundLoop`'s Error logging never fired); the fence
  deadline (`deadline_boot_ms`) silently passed mid-hang so `mayMutate()` flipped false — the start of
  the ~2-minute 668 window — with **no event and no log** (`mayMutate` is a passive deadline
  comparison, `CasMountRuntime.cpp:81-85`); and when the stuck PUT finally succeeded,
  `onRenewSucceeded → on_renew_ok → setMountDeadline` (`CasMountRuntime.cpp:242`, `:128`) silently
  re-armed the fence (`mayMutate()` true again → recovery) — again with no log, no epoch bump
  (`setMountDeadline` does not touch `fence_generation`, `:128-131`), and no remount.

This is the fence-not-rescue design working as intended (a holder that cannot confirm renewal is not
live), and it is not weakened here. The episode (msan lane, sha `07f8398acddff2c`, core window
02:02:52→02:05:00 with stragglers to 02:10) had two consequences:

1. **Every durable write during the fence→remount window failed instantly.** Two admission points
   threw:
   - the plain-object / staging-finalize path: `INVALID_STATE` (error 668) —
     `CasMountRuntime::checkFenceOrThrow` (`CasMountRuntime.cpp:98-112`), message *"mount fence
     tripped: the durable write is refused because this node no longer holds the mount incarnation
     it was admitted under"*;
   - the ref-log append lane: `NETWORK_ERROR` (error 210) — `CasRefLedger::flushRefBatch`'s
     `may_mutate` gate (`CasRefLedger.cpp:1335-1341`), *"CAS mount lost / lease expired —
     refusing to append ref-log transactions"*, routed through `makeCasWriteRetryLater…`.

   ~40 stateless tests failed. The clients are one-shot: a single `INSERT` has no caller-level
   retry, so the fence window surfaced as a hard error, not latency.

2. **The ~7-minute episode produced ZERO `CasMountLease*` text-log lines.** The renewal-confirm
   failure, the fence arming, the self-remount begin/progress/complete are all invisible at the
   default log level — the episode had to be reconstructed from `executeQuery` error timestamps.

### Why the fence/remount lifecycle is invisible today (verified — with a correction to the brief) {#context-invisible}

**Correction to the brief (two-part, refined in rev.2).** Renewal failure is logged *only in Mode 1*
(a **throwing** renewal): the base `SingleWriterSlot::backgroundLoop` (`CasServerRoot.cpp:1176-1237`)
logs both the transient-retry case (`:1202`) and the terminal fence-stop case (`:1209`) via
`tryLogCurrentException`, which defaults to **Error** (`Exception.h:276`), on the `CasMountLeaseKeeper`
logger. **In Mode 2 — the observed incident — nothing throws, so none of that fires**: a hanging
renewal PUT never returns to `backgroundLoop`'s `catch`, the deadline expiry is a passive comparison
with no log, and the `setMountDeadline` re-arm is silent. Mode 2 logs *nothing, anywhere* today. So
both modes must be named, and Mode 2 is the one the incident proves dominant (§A.5).

What is genuinely invisible spans both modes: the **runtime-side fence arm** (Mode 1), the **silent
deadline expiry and silent re-arm** (Mode 2, verified zero-logging above), and the coherence of the
timeline. For Mode 1 specifically:

- `CasMountRuntime.cpp` has exactly three `LOG_WARNING` lines, and all three are on **terminal**
  lifecycle edges: `enterIdentityLost` (`:363`), `enterVanished` (`:415`), and the unclean-shutdown
  branch of `finishTeardown` (`:528`). The transient, recoverable primitives —
  `tripMountLost` (`:87`), `armMountFence` (`:133`), `setMountDeadline` (`:128`),
  `noteLeaseLost` (`:309`), `noteRemounted` (`:324`), `scheduleRemount` (`:431`) and the self-remount
  thread body inside it — carry **no** `LOG_` statement at all. So *fence armed*, *self-remount
  begin*, *remount progress*, and *remount complete* produce nothing.
- The two base-class lines that do fire are (a) on the `CasMountLeaseKeeper` logger — a different
  name from the `CasMountLease` logger a triager watching for the *mount* story would filter on
  (which stays silent, explaining "zero `CasMountLease*` lines") — and (b) they log the **raw
  exception**, not a self-describing "fencing because <classified reason>, deadline <t>, remounting"
  timeline. The classified renewal-mismatch reason (`onRenewMismatch`'s
  `same_epoch_state_uncertain` / `superseded` / `vanished` branches, `CasServerRoot.cpp:905-986`)
  goes only to the **audit event sink**, never to text.
- In `CasServerRoot.cpp` the `CasMountLease` logger fires exactly once, at `:529`, inside
  `claimMountAwaitingExpiry` — the **token-stability observation wait**. A fence-triggered
  self-remount reclaims a `gc_fenced` slot on the fast reclaim path (`claimMount`,
  `CasServerRoot.cpp:396-414`) and never enters that observation wait, so even that one line does not
  fire during a self-remount. The `CasMountLeaseKeeper` logger otherwise fires only on the release
  no-op paths (`:1018`, `:1032`); `CasHeartbeatFloor` fires only on a GC-driven fence-out (`:638`).

So Part A's headline is the **runtime fence-arm + self-remount lifecycle timeline** (genuinely
missing), plus surfacing the **classified fence reason** at the keeper as text (today audit-only),
not a duplicate of the base-class raw-exception lines.

## Goals / non-goals {#goals}

**Goals.**
- (A) An `Information`-level, rate-limit-safe text-log timeline of the fence/remount lifecycle, plus
  a few cheap `ProfileEvents` counters, so a fence window is diagnosable from `err.log` alone.
- (B) An **opt-in, default-off** bounded, event-driven wait so a durable write started during a
  fence→remount window blocks (bounded) for the self-remount to complete and then admits normally,
  instead of failing instantly — turning a brief fence window into latency rather than an error for
  one-shot clients.

**Non-goals.**
- No change to the mount/lease/fence/remount **protocol**: no new object-store request steps, no
  change to lease TTL, fence arming, generation bumping, self-remount, or claim semantics. This is a
  standing user veto and is treated as absolute.
- No weakening of fence-not-rescue: a holder that cannot confirm renewal still fences and remounts.
- No change to the admission/incarnation safety check itself (Part B only *delays* it; §B-safety).
- No backward-compatibility scaffolding (pre-release codebase, no persisted data).
- Part A adds no audit-log (`system.content_addressed_log`) schema change — it is text-log +
  `ProfileEvents` only. The structured events already exist.

---

## Part A — mount-lease keeper & remount observability {#part-a}

Pure logging + counters. No behavior change, no new control flow, no new locks.

### A.1 What to log, and where {#a-what-to-log}

Part A adds the genuinely-missing lines (A2, A3, A4), enhances one existing line (A6 gains the
fenced-window duration), and leaves the already-existing lines alone (A1 base-class renewal failure;
A5 remount-attempt-failed). Every new/enhanced line is edge-triggered — fires once per fence
episode — so none needs a rate limiter (the one repeatable line, A5, already exists and is naturally
capped by the remount backoff).

| # | Moment | Site | Level | Change |
|---|---|---|---|---|
| A1 | Transient renewal failure (renew PUT threw, loop retries) / terminal fence-stop | base `SingleWriterSlot::backgroundLoop` (`CasServerRoot.cpp:1202`, `:1209`) | Error | **exists** — leave (base class, shared by all slots; already carries the store error) |
| A2 | Classified fence reason at the keeper (which `onRenewMismatch` branch fenced us) | `MountLeaseKeeper::onRenewFailed` (`CasServerRoot.cpp:892`), which runs after the classification | `Warning` | **new** — text line carrying the classified reason (today audit-event-only) |
| A3 | Fence armed (write fence latched lost, deadline frozen) | `CasMountRuntime::tripMountLost` (`CasMountRuntime.cpp:87`) | `Warning` | **new** |
| A4 | Self-remount begin | `Pool::tryRemountOnce` entry (`CasPool.cpp` — the `remount_attempt` callback, just before the claim protocol at `:985`) | `Information` | **new** |
| A5 | Self-remount attempt failed, will retry | `Pool::tryRemountOnce`'s catch (`CasPool.cpp:1091`) already logs *"self-remount attempt failed; will retry"* via `tryLogCurrentException` | Error | **exists** — leave (already fires once per failed attempt) |
| A6 | Self-remount complete (fence re-armed, back to `Live`) | `Pool::tryRemountOnce` (`CasPool.cpp:1072`) already logs *"recovered as writer_epoch {}"* | Information | **enhance** — add the fenced-window duration to the existing line |

**Reality corrections vs. the brief (Part A).** More of the remount lifecycle is already logged than
the brief implies — but in `Pool::tryRemountOnce` (the `remount_attempt` callback in `CasPool.cpp`),
not in `CasMountRuntime`, which is why the earlier `CasMountRuntime.cpp` survey found nothing:
`CasPool.cpp:1072` already logs remount **complete** (`"recovered as writer_epoch N"`, Info) and
`:1091` already logs remount **attempt-failed** (Error). What is genuinely missing is **A3** (the
fence-arm, in `tripMountLost`), a clear **A4** (a *"self-remount starting"* line — today the begin is
only implicit from the not-claimable / observation lines), the **A6 duration** (the existing
complete line reports the new epoch but not how long writes were refused), the **A2** classified
fence reason as text (audit-only today), and all three ProfileEvents (§A.3).

Message texts (exact proposed strings; `{}`-style, `fmt`). A3/A4 use the `CasMountLease` logger so
the fence-arm and remount-begin sit with the existing `CasPool` remount lines as one story:

- **A2** (`CasMountLeaseKeeper` logger, `Warning`):
  `"CAS mount-lease for server root '{}' fenced: {} — the write fence is latched lost; a self-remount under a fresh writer_epoch will follow"` — args: `server_root_id`, the classified reason string (the `onRenewMismatch` branch, e.g. `state uncertain (ambiguous prior renewal or epoch-state loss)` / `superseded by a newer incarnation` / `vanished`). The keeper stashes this reason when it classifies (§A.1a) so `onRenewFailed` can name it.
- **A3** (`CasMountLease` logger, `Warning`):
  `"CAS mount fence tripped for server root '{}' (writer_epoch={}): durable writes are refused until a self-remount re-admits this node"` — args: `server_root_id`, `mount_fence.writer_epoch`.
- **A4** (`CasMountLease` logger):
  `"CAS self-remount starting for server root '{}': reclaiming the mount under a fresh writer_epoch after a fence loss"` — arg: `server_root_id`.
- **A6 (enhanced)** (`CasPool` logger, extend the existing `:1072` line):
  `"CAS self-remount '{}': recovered as writer_epoch {} (fresh incarnation; older builds fail closed); writes were fenced for {} ms"` — the new trailing clause carries the fenced-window wall-ms.

**A.1a — the classified reason for A2.** `onRenewMismatch` (`CasServerRoot.cpp:905`) already knows the
branch it took but only emits it as an audit event; `onRenewFailed` (`:892`) fires next on the same
thread and logs the fence-latch. The cheap wiring: `onRenewMismatch` stores its human reason in a
keeper member before throwing, and `onRenewFailed` reads it for the A2 text. (Alternatively A2 lives
directly in `onRenewFailed` with a generic reason; storing the classified string is strictly better
and costs one `String` member.)

The A6 fenced-window duration is stamped at A3 (the fence-trip instant, in `tripMountLost`) and read
at A6 (the re-arm, in `tryRemountOnce`), using the runtime's existing `bootMsNow()`
(`CLOCK_BOOTTIME`, injectable in tests via `boot_ms_fn`). Boot time is both *more correct* than
wall-clock for "how long were writes refused" (it does not jump on an NTP step and includes
VM-suspend) and *deterministic in tests* (advance `boot_ms_fn` between trip and remount, assert the
exact recorded microseconds). This crosses the runtime/pool boundary, so the stamp lives on
`CasMountRuntime` (a `fence_tripped_boot_ms` member set in `tripMountLost`, read at the completion
site). If a remount ever completes with no prior trip stamp (`== 0`), the duration clause is omitted
rather than reporting a bogus interval.

### A.2 Rate-limit mechanism — pick the per-message limiter, not the per-logger one {#a-rate-limit}

Every line Part A adds or enhances (A2, A3, A4, A6) is edge-triggered — it fires once per fence
episode — so none needs a rate limiter. But the mechanism choice is recorded here because it is the
first thing a future contributor will reach for, and the wrong choice is a known trap
(`reference_logserieslimiter_aggregate_gotcha`): `LogSeriesLimiter` (`Common/LoggingHelpers.h:50`)
keys its budget on `Hash(logger_name)` **only** — every distinct message on that logger competes for
one accept/mute decision, so one chatty line can mute an unrelated important one on the same logger.
The correct primitive for any per-message throttle is **`LogFrequencyLimiter`** (`logger_useful.h:20`
→ `LogFrequencyLimiterImpl`, `LoggingHelpers.h:15`), which keys on `Hash(logger_name, format_string)`
and throttles **each message independently**. If A1's or A5's per-attempt Error volume during a very
long outage ever proves too loud, wrapping *that* line in `LogFrequencyLimiter` (interval ~30 s) is
the fix — but A1/A5 live in shared code (`SingleWriterSlot` base / `tryLogCurrentException`) and are
out of scope here.

### A.3 ProfileEvents {#a-profile-events}

Add three counters (`Common/ProfileEvents.cpp`, next to the existing `CasMountLeaseLost` /
`CasIdentityLost` / `CasDataRootVanished` at `:893-895`). These are cheap and test-friendly (a gtest
can assert the counter moved without scraping text logs):

- `CasMountFenceArmed` (`ValueType::Number`): count of transient fence trips — incremented in
  `CasMountRuntime::tripMountLost` (`CasMountRuntime.cpp:87`). (Distinct from `CasMountLeaseLost`,
  which counts the *keeper*-side terminal-loss classifications; `CasMountFenceArmed` counts the
  *runtime*-side fence latch, the thing that actually gates writes.)
- `CasSelfRemountCompleted` (`ValueType::Number`): count of successful self-remounts — incremented at
  the completion site in `Pool::tryRemountOnce` (`CasPool.cpp:1072`/`:1086`).
- `CasSelfRemountMicroseconds` (`ValueType::Microseconds`): total time (boot clock) spent
  fenced-and-remounting on the **Mode 1** path — incremented by the A6 fenced-window duration at the
  same completion site. `total / CasSelfRemountCompleted` is the average Mode-1 fence-window length.
- `CasMountFenceExpired` (`ValueType::Number`): count of **Mode 2** episodes — a fence deadline that
  expired without a lost-latch/remount, first observed at a refused durable write (§A.5).
- `CasMountFenceExpiredMicroseconds` (`ValueType::Microseconds`): total time (boot clock) writes were
  refused across Mode-2 (non-remount) recoveries — the fenced-window duration for the path Part B most
  often rides. `total / CasMountFenceExpired` is the average silent-outage length.

`ProfileEvents::increment(ProfileEvents::X, value)` is the recording call (see the existing CAS
counters in `CasRequestControl.cpp`). No new coupling to generic ClickHouse code.

### A.4 Part A test strategy {#a-tests}

gtest, in `src/Disks/tests/gtest_cas_heartbeat.cpp` (keeper-level A1/A2) and
`gtest_cas_pool.cpp` (runtime/remount-level A3/A4/A6 + the ProfileEvents):

- Drive a transient renewal failure through the real background loop (the existing
  `ApplyThenThrow…` / `TransientPutOverwriteFaultBackend` fault backends) and assert
  `CasMountFenceArmed` and `CasSelfRemountCompleted` move and `CasSelfRemountMicroseconds` is > 0.
  ProfileEvents are asserted with a `ProfileEvents::Counters` snapshot (deterministic; no log
  scraping, no fake clock).
- Text-log lines: assertions on the counters are the durable contract; the text strings are checked
  by a lightweight capture only where the test harness already supports it (a log-sink probe), else
  left to the counter assertions. Rationale: pinning exact text is brittle and the counters already
  prove the code path executed.

### A.5 The silent hang / expiry / re-arm path (Mode 2 — the dominant real one) {#a-mode2}

Mode 2 (§context-rca) produces **no log line on any transition** today. It has no thrown exception to
catch and no `armMountFence` to hook: the deadline expiry is a passive comparison in `mayMutate`
(`CasMountRuntime.cpp:81-85`) and the re-arm is a bare `setMountDeadline` store (`:128-131`). Part A
makes all three of its transitions observable, plus two counters, **without adding any work to the
hot `mayMutate` read path** (an explicit constraint):

| # | Moment | Where detected | Level |
|---|---|---|---|
| M1 | Fence **deadline expiry** — the first durable write refused because `deadline_boot_ms` passed with `lost == false` (i.e. an expiry, not a Mode-1 latch) | at the **admission sites** (`CasMountRuntime::checkFenceOrThrow` throw-path; the ref-lane `may_mutate` refusal), edge-latched | `Warning` |
| M2 | A renewal that **landed slow** (took longer than `mount_renew_period`) | `MountLeaseKeeper::onRenewSucceeded` (`CasServerRoot.cpp:883`), which has `last_attempt_boot_ms` | `Information` |
| M3 | Fence **re-arm after an expiry** (recovery without a remount) | `CasMountRuntime::setMountDeadline` (`:128`), edge-latched | `Information` |

**Edge-detection design (the chosen option, justified).** Expiry has no natural event, so a latch is
introduced: a relaxed atomic `fence_expiry_reported_boot_ms` on `CasMountRuntime` (0 = no unreported
expiry).
- **M1 (expiry) is detected at the admission sites, not by a watchdog and not on the `mayMutate` read
  path.** When `checkFenceOrThrow` is about to refuse and the cause is a deadline expiry
  (`!mount_fence.lost && bootMsNow() >= deadline_boot_ms`), it `compare_exchange`s
  `fence_expiry_reported_boot_ms` from 0 to the **expiry instant** (`deadline_boot_ms`, the true start
  of the window). The winner logs M1 once and increments `CasMountFenceExpired`. Justification for
  this placement over a watchdog thread or a keeper-side check: (a) no new thread; (b) the keeper
  thread is *hung inside the PUT* during a Mode-2 outage, so it cannot self-report the expiry until it
  unblocks — only an independent caller can; (c) the admission path is the natural independent caller
  and it runs the atomic-CAS-plus-log **at most once per episode**, never on the hot `mayMutate`
  read; (d) it fires exactly when the expiry first refuses a user write — the only moment it is
  actionable, and the true start of the operator-visible 668 window. If no write is attempted during a
  brief expiry, there is no user-visible incident to report, and M2/M3 still record it after the fact.
- **M3 (re-arm) is detected in `setMountDeadline`** (keeper thread, once per renew period — not hot):
  if `fence_expiry_reported_boot_ms != 0` and the new deadline restores liveness, log M3 with
  outage = `bootMsNow() - fence_expiry_reported_boot_ms`, increment
  `CasMountFenceExpiredMicroseconds`, reset the latch to 0, **and `notify_all` the Part B grace CV**
  (§B.2 — this is the Mode-2 wake). Because the stamp was the true expiry instant, the reported outage
  is the exact fenced window.
- **M2 (slow renewal landed)** is a plain compare in `onRenewSucceeded`:
  `bootMsNow() - last_attempt_boot_ms > mount_renew_period` → one `Information` line with the elapsed
  ms. It closes the loop for the case where the store was slow but the deadline had not (yet) expired.

Proposed message texts:
- **M1** (`CasMountLease` logger, `Warning`):
  `"CAS mount fence deadline expired for server root '{}' (writer_epoch={}): durable writes are refused; the lease renewer has not confirmed within the renew period and the deadline passed {} ms ago — the object store is likely slow or unreachable (no self-remount; recovery is a late renewal landing)"` — args: `server_root_id`, `mount_fence.writer_epoch`, `bootMsNow() - deadline_boot_ms`.
- **M2** (`CasMountLeaseKeeper` logger, `Information`):
  `"CAS mount-lease renewal for server root '{}' landed after {} ms (renew period {} ms) — the object store was slow"` — args: `server_root_id`, elapsed, `mount_renew_period`.
- **M3** (`CasMountLease` logger, `Information`):
  `"CAS mount fence re-armed for server root '{}' after a {} ms expiry window (a late lease renewal landed; recovered without a self-remount)"` — args: `server_root_id`, outage ms.

**Interplay with Mode 1.** The Mode-1 fence-arm counter (`CasMountFenceArmed`, in `tripMountLost`) and
the Mode-2 expiry counter (`CasMountFenceExpired`, at admission) are disjoint by construction:
`tripMountLost` sets `lost = true`, and the M1 detector fires only when `lost == false`. A given fence
window is therefore counted as exactly one of the two, never both.

---

## Part B — opt-in bounded wait-for-remount on the write path {#part-b}

Default OFF. When enabled, a durable write that would fail instantly during a fence→remount window
instead blocks (bounded, event-driven) until the self-remount re-admits the node, then proceeds
normally; or it fails exactly as today when the bound expires, the disk is shutting down, or the
remount concluded terminally.

Analogy: a `ReplicatedMergeTree` query during a brief Keeper reconnect experiences latency, not an
error.

### B.1 The setting {#b-setting}

One disk-level setting on `ContentAddressedSettings`
(`ContentAddressedSettings.cpp`, `LIST_OF_CONTENT_ADDRESSED_SETTINGS`):

```
DECLARE(UInt64, write_grace_ms, 0, "Bounded wait (ms) for a durable write to ride out a mount fence→self-remount window before failing; 0 = off = fail immediately (today's behavior)", 0)
```

- `0` (default) = feature off = **exactly** current behavior (the wait primitive returns
  immediately; §B-safety proves byte-identity).
- No `validate()` addition is required (any `UInt64` is a legal bound; `0` is off). It flows the
  same way `materialization_grace_ms` does:
  `ContentAddressedMetadataStorage` ctor reads `settings_[ContentAddressedSetting::write_grace_ms]`
  (alongside `materialization_grace_ms` at `ContentAddressedMetadataStorage.cpp:287`), sets
  `pool_config.write_grace_ms` (alongside `:740`), and `PoolConfig::mountConfig()`
  (`CasPool.h:225-235`) projects it into `MountConfig` so `CasMountRuntime` — which owns the fence,
  the remount thread, and `armMountFence` — can honor it.

Naming rationale: `write_grace_ms` is honest and short — it is a grace period a **write** is granted
before it gives up. It is distinct from `materialization_grace_ms` (a successor's post-reclaim wait
before trusting recovery listings — a different lifecycle moment on a different actor).

### B.2 The wait primitive — event-driven, on the runtime {#b-primitive}

A new method on `CasMountRuntime` (which already owns `MountFence`, `armMountFence`, `tripMountLost`,
the lifecycle atomics, and the self-remount thread):

```cpp
/// Part B (opt-in write grace). If the local write fence is currently held, returns immediately.
/// If it is transiently not held (a fence→self-remount is in flight) and config.write_grace_ms > 0,
/// blocks — event-driven, never a sleep-poll — until ONE of:
///   - the self-remount re-arms the fence (armMountFence → mayMutate() true again): return;
///   - the grace bound elapses: return (the caller's normal fence check then fails as today);
///   - the pool is shutting down / tearing down: return promptly (fails as today);
///   - the pool reached a TERMINAL lifecycle (vanished_intent / IdentityLost): return immediately
///     (no point waiting out a decommission or lost identity).
/// NEVER changes admission: on return the caller still captures a fresh fenceGeneration() and runs
/// its unchanged fence check. When write_grace_ms == 0 this is an unconditional immediate return.
void waitForWriteGrace() const;
```

Mechanism:
- A dedicated `std::condition_variable write_grace_cv` + `std::mutex write_grace_mutex`, **new and
  used only for this wait** — never the `remount_thread_mutex`/`remount_cv_mutex` the remount loop
  uses, never `builds_mutex`, never any ref-ledger lock. (No-deadlock argument, §B-nodeadlock.)
- The wait is `write_grace_cv.wait_for(lk, bound, predicate)` where `predicate` returns true when
  `mayMutate()` OR `remount_shutting_down.load()` OR `remountTerminal()`. `wait_for` gives the bound
  for free; the predicate guards against lost/spurious wakeups. This is a genuine event wait, not a
  poll: nothing sleeps-then-checks in a loop.
- **Signalers** (all `write_grace_cv.notify_all()`), added at the existing transition sites, each a
  single line next to code that already runs there:
  - `setMountDeadline` (`CasMountRuntime.cpp:128`) — **the Mode-2 re-arm** (a hung renewal landed and
    extended the deadline; §A.5). This is the dominant real recovery path, so it is the most important
    signaler: without it a Part B waiter would sleep out its whole bound through exactly the incident
    it is meant to absorb. It wakes waiters that then observe `mayMutate()` true.
  - `armMountFence` (`:133`) — **the Mode-1 re-arm** (a self-remount reclaimed under a fresh
    incarnation) ⇒ wake waiters; they observe `mayMutate()` true and return (proceed).
  - `stopRemountThread` (`:486`) and `finishTeardown` (`:504`) — teardown/shutdown ⇒ wake waiters so
    they unblock promptly (they observe `remount_shutting_down` and return → fail as today). This is
    the disk-lifecycle-leak guard: a waiting write thread must never outlive or block teardown.
  - `enterVanished` (`:372`), `enterIdentityLost` (`:337`), `publishVanishedIntent` (`:421`) —
    terminal ⇒ wake waiters so they fail immediately rather than wait out the whole bound.

The bound is a real (small, in tests) wall-clock timeout on `wait_for`; the fence's own timing keeps
using `boot_ms_fn`. No new clock injection is needed for the wait itself — tests drive it by
triggering `armMountFence`/shutdown from another thread (event-driven, matching the file's existing
bounded-poll convention), and test the bound-expiry case with a small real bound and an elapsed-≥-bound
assertion.

### B.3 Where the wait is inserted (the two admission surfaces) {#b-insertion}

Part B wraps only the **entry** gates — the points where a write is first admitted and has done no
incarnation-tied work yet. It deliberately does NOT wrap the mid-operation re-checks
(`CasPartWriteTxn.cpp:705`, `:747`), where a moved generation legitimately means "your staged
displacement work is stale, abort" — those keep failing immediately.

**Surface 1 — plain-object / staging-finalize (668).** Callers capture
`admitted_generation = pool->fenceGeneration()` and later call `checkFenceOrThrow(admitted_generation)`
before the durable write (e.g. `ContentAddressedTransaction.cpp:887,901`). Insert the wait
**immediately before the generation is captured**:

```cpp
pool->waitForWriteGrace();                                  // Part B: bounded, opt-in, may block
const uint64_t admitted_generation = pool->fenceGeneration();  // captured AFTER the wait (fresh)
...
[pool, admitted_generation] { pool->checkFenceOrThrow(admitted_generation); }
```

If the remount re-armed the fence during the wait, the freshly-captured generation matches the live
incarnation and `checkFenceOrThrow` passes — the write proceeds under the **current** incarnation
(not a stale one). If the bound expired / still fenced / terminal, `checkFenceOrThrow` throws exactly
668 as today.

**Surface 2 — ref-log append lane (210).** The `may_mutate` fence refusal lives in
`CasRefLedger::flushRefBatch` (`CasRefLedger.cpp:1335`), which runs **inside the append-leader
tenure**: a caller in `appendRefOps` becomes the lane leader by setting `leader_active = true`
(`CasRefLedger.cpp:1165`) and only then runs the leader loop → `flushRefBatch` → the fence gate.

**The wait must be placed at the append entry, BEFORE the lane acquires `leader_active` — never at
the `may_mutate` line inside leader tenure.** This is load-bearing, and is exactly the `leader_active`
caveat: the self-remount itself waits (bounded) for every lane to settle to `!leader_active` in
`refLanesSettledForRemount` (`CasRefLedger.cpp:837`, called by `Pool::tryRemountOnce` at
`CasPool.cpp:1022`), and clean shutdown waits the same way in `drainRefLanesForShutdown`
(`CasRefLedger.cpp:1011`). If a Part B wait were held *inside* leader tenure, it would keep
`leader_active` set while waiting for the fence to re-arm — but the fence only re-arms *after*
`refLanesSettledForRemount` returns, which is blocked on that very `leader_active`. The remount would
then stall until the write-grace bound expired (giving Part B no benefit for the ref lane and merely
delaying the remount), and shutdown would stall the same way. Placing the wait **before** leadership
avoids this entirely: the waiter holds no `leader_active` and no ledger lock, so the remount settles,
re-arms the fence, and wakes it; the waiter then acquires leadership and appends under the fresh
incarnation. Because the caller re-enters the leader loop after the wait (and the remount has swapped
the cached table runtime, `CasPool.cpp:1064`), the retry lands on a freshly-recovered runtime, so the
`superseded_by_remount` refusal (`CasRefLedger.cpp:1349`) is not hit either. The wait is invoked
through a small pool callback (mirroring the existing `may_mutate` `std::function<bool()>` injected at
`CasRefLedger.h:370`), so the ledger takes no new `Pool` dependency. The bound (`write_grace_ms`,
< lease TTL) is retained as defense-in-depth: even under an unforeseen interleaving, no lane can
delay a remount or a shutdown drain by more than the bound.

The **shutdown** admission gate at the same entry (`shutting_down` → `throwCasWriteRetryLater`,
`CasRefLedger.cpp:1123-1126`) is terminal and Part B does **not** wait on it — a shutting-down store
fails immediately, exactly as today.

Both surfaces reduce to the same shape: **wait (bounded, opt-in, outside any leader tenure or lock) →
run the unchanged fence check**.

### B.4 Why safety is untouched {#b-safety}

The single load-bearing claim: **Part B never lets a write proceed without a valid current mount
incarnation; it only inserts a bounded delay before the existing, unchanged admission check.**

1. **The admission check is byte-identical.** `checkFenceOrThrow` (`CasMountRuntime.cpp:98-112`) and
   the ref lane's `may_mutate` gate are not modified. `waitForWriteGrace()` is a *separate*
   statement that runs *before* the caller captures its generation and calls the check. It changes
   *when* the check runs, never *what* it decides.
2. **The generation is captured AFTER the wait, so no stale incarnation rides through.** The
   fence-generation guard (rev.7 `[C2]`, `CasMountRuntime.h:136-151`) exists precisely so a write
   admitted under incarnation *G* aborts if the incarnation moved. Part B captures the generation
   *after* the wait returns, i.e. under whatever incarnation is current then. A write only proceeds
   if `mayMutate()` is true AND its freshly-captured generation still matches at the check — the same
   two conditions as today. This is correct for **both** recovery modes and captures why "capture
   after the wait" is the right rule: in **Mode 2** (the dominant path, §A.5) the re-arm is a bare
   `setMountDeadline` that does **not** bump the generation (`CasMountRuntime.cpp:128-131`), so the
   post-wait generation equals what a pre-wait capture would have been and the write proceeds under the
   *same, still-valid* incarnation — the fence was never lost, only stalled. In **Mode 1** the remount
   bumps the generation via `armMountFence`, so the post-wait capture is the *new* incarnation's and
   the write admits *into* it, having done nothing under the old one (the wait is at the entry gate;
   §B.3 excludes the mid-operation re-checks). Either way the write proceeds only under a current,
   valid incarnation.
3. **No new trust, no bypass of any liveness/observation rule.** The wait consults only the local
   `MountFence` state (`mayMutate`) and lifecycle atomics; it issues no object-store request, reads
   no lease, and can never *make* the fence live. Only a genuine self-remount (`armMountFence`) makes
   `mayMutate()` true again, and that path is unchanged and still runs the full claim/observation
   protocol. Part B cannot manufacture admission.
4. **`write_grace_ms == 0` is provably today's behavior.** The primitive returns immediately, the
   generation is captured and checked exactly where it is today, and no signaler does anything a
   waiter observes. The only residual difference from today's source is one extra call that
   early-returns — no observable behavior change.
5. **Terminal outcomes fail fast, never wait out the bound.** `remountTerminal()` (vanished / FORGET
   / IdentityLost) and `remount_shutting_down` short-circuit the predicate, so a decommission or a
   lost identity refuses immediately, preserving the fence-not-rescue "terminal is terminal"
   contract.

### B.5 Why there is no deadlock and no lifecycle leak {#b-nodeadlock}

- **The wait holds nothing the remount needs.** `waitForWriteGrace` takes only its own dedicated
  `write_grace_mutex` (held solely to wait on `write_grace_cv`); it never holds `remount_thread_mutex`,
  `remount_cv_mutex`, `builds_mutex`, `ref_queue_mutex`, or any `RefTableRuntime::state_mutex`. The
  self-remount thread (`scheduleRemount`, `CasMountRuntime.cpp:451`) therefore runs to completion —
  including `armMountFence`, which is what wakes the waiter — with no lock the waiter could be
  holding. The signal→wake path acquires `write_grace_mutex` *inside* `armMountFence`'s existing body
  only for the `notify_all`, never nested under a lock the waiter also wants.
- **The wait is never held inside an append-leader tenure** (§B.3, Surface 2). This is the specific
  hazard the `leader_active` caveat names: `refLanesSettledForRemount` (`CasRefLedger.cpp:837`, on the
  remount path) and `drainRefLanesForShutdown` (`:1011`, on the shutdown path) both block on every
  lane being `!leader_active`. Part B's ref-lane wait sits *before* the caller sets
  `leader_active = true` (`CasRefLedger.cpp:1165`), so it can never keep those two settle-waits from
  completing — the remount that the waiter is waiting for is never blocked by the waiter.
- **The wait is promptly interruptible by teardown.** `stopRemountThread`/`finishTeardown` set
  `remount_shutting_down` (already latched at teardown top, `CasMountRuntime.cpp:492`) and
  `notify_all` the grace CV; the predicate returns true and the waiter unblocks within one wakeup —
  it never outlives or blocks the disk teardown. This directly answers the known disk-lifecycle-leak
  hazard class (`project_cas_disk_lifecycle_leak_mount_abort`): waiting threads self-release the
  moment shutdown is signaled.
- **The bound is a hard ceiling.** Even with no signal at all (e.g. a remount that never completes),
  `wait_for(bound)` returns after `write_grace_ms`, so a waiter can block for at most the configured
  bound.

### B.6 Relationship to the existing `CasWriteRetryLater` lane {#b-retry-later}

The existing `throwCasWriteRetryLater` / `makeCasWriteRetryLaterExceptionPtr`
(`CasRequestControl.h:207-261`, `CasRequestControl.cpp:174-188`) throws `NETWORK_ERROR` so that a
**caller with its own retry loop** — a background merge/mutation via
`ReplicatedMergeMutateTaskBase`, whose queue applies exponential backoff — re-executes the whole
operation later. It is coarse (the merge recomputes the entire output part on each retry) and, by
design, aimed at background work that *has* a retry loop.

Part B is complementary and orthogonal:
- It targets the writers that have **no** caller-level retry — one-shot `INSERT`/DDL clients — for
  which the retry-later throw is a hard failure, not a deferral.
- It acts **before** the write fails, not after: it holds admission briefly so the *same* in-progress
  write proceeds, rather than throwing and relying on a caller to start over.
- It does **not** replace or alter the retry-later lane. When the grace bound expires (or the feature
  is off), the write still fails through exactly the existing path — `checkFenceOrThrow`'s 668 for the
  plain surface, `makeCasWriteRetryLaterExceptionPtr`'s 210 for the ref lane — so the merge/mutation
  backoff machinery keeps working unchanged for the writers that rely on it. Part B only shortens the
  observed failure rate for the writers that today have nowhere to retry.

### B.7 Part B test strategy {#b-tests}

gtest, in `src/Disks/tests/gtest_cas_pool.cpp` (runtime + open path) with the existing fault-backend
and `wait_sleep_fn`/`boot_ms_fn` seams, plus a keeper-level driver in `gtest_cas_heartbeat.cpp`
where a bare `CasMountRuntime` suffices. Required scenarios (each event-driven — a signal from a
second thread, never a blind sleep to dodge a race):

1. **Fence trips while a write waits → remount completes → the SAME write succeeds.** With
   `write_grace_ms > 0`, a thread calls `waitForWriteGrace()` while the fence is down; a second
   thread `armMountFence`s (simulating remount complete); the waiter returns, the freshly-captured
   generation + live fence pass `checkFenceOrThrow`, no error.
2. **Bound expires before remount → today's failure, elapsed ≥ bound.** Small real bound; no re-arm;
   assert `waitForWriteGrace` returned after ≥ bound and the subsequent `checkFenceOrThrow` throws
   `INVALID_STATE`.
3. **Shutdown during wait → prompt unblock with failure, no hang, no use-after-free.** A waiter is
   blocked; `stopRemountThread`/`beginShutdownForTest` signals; the waiter returns promptly and the
   check fails. Run under ASan/TSan discipline; a bounded test-level deadline guards against a hang.
4. **Terminal remount (decommissioned / vanished pool) → immediate failure, no wait.** Force a
   terminal lifecycle (`setLifecycleForTest(VanishedForgotten)` / `enterIdentityLost`); assert
   `waitForWriteGrace` returns ~immediately (well under the bound) and the check fails.
5. **Feature off (`write_grace_ms == 0`) → behavior byte-identical to today.** `waitForWriteGrace`
   returns immediately even with the fence down; the check throws exactly as it does without the
   call.

Sanitizer discipline: no test `EXPECT_THROW`s a `LOGICAL_ERROR` (its construction aborts debug/ASan
builds); the admission failures here are `INVALID_STATE`/`NETWORK_ERROR`, which are safe to
`EXPECT_THROW`. If any assertion ever needs a `LOGICAL_ERROR` death, use the death-test split pattern
from `gtest_ca_wiring.cpp`.

---

## Open questions {#open-questions}

1. **Ref-lane wait placement (settled here; confirmed in the plan).** Surface-2's wait goes at the
   `appendRefOps` entry before `leader_active` is set (§B.3), so it precedes the `may_mutate` gate and
   lands the post-remount retry on a freshly-recovered runtime. The one implementation detail the
   plan's Surface-2 task pins is exactly where, relative to the `ref_queue_mutex` acquisition
   (`CasRefLedger.cpp:1119`) and the `shutting_down` check (`:1123`), the wait's advisory
   `mayMutate` pre-check is issued — the wait itself takes none of those locks, so this is placement,
   not a safety question.
2. **Grace vs. operation deadline interaction.** `write_grace_ms` is independent of
   `cas_request_budget.operation_deadline_ms`; a very large grace plus a long remount could exceed a
   client's own timeout. This is acceptable (the client's timeout then bounds it, exactly like any
   slow operation) and is called out only so operators size `write_grace_ms` sensibly (a value near
   one lease TTL is the natural ceiling — longer than that and the remount has almost certainly either
   completed or gone terminal).
