# CAS mount crash-recovery — design

**Status:** DESIGN (2026-07-02). **Branch:** `cas-mount-crash-recovery`.
**Fixes:** S13 — a hard-killed CA server cannot self-recover its mount on restart
(exits with `ABORTED`: *"server_root_id … is actively mounted by another server"*).

## Problem

On `docker kill -s KILL` (or any hard crash / OOM), the dying server's `MountLeaseKeeper::terminate`
never runs — SIGKILL skips the `Store` destructor — so the mount object survives in the pool as
`{server_uuid = self, writer_epoch = N, expires_at_ms = kill_time + ttl}`.

On restart `Store::open` runs its strict mount-safety order (owner → epoch → mount → watermark):

1. `claimOwnerOrThrow` — passes (owner is sticky; same `server_uuid`).
2. `allocateWriterEpoch` — unconditionally CAS-bumps the durable counter to **N+1**.
3. `claimMount(our_epoch = N+1)` — reads the stale mount and finds:
   - `existing.server_uuid == our_uuid` (it is our own prior incarnation), **but**
   - `existing.writer_epoch (N) != N+1` → the adopt-own (same-epoch) branch does not match, **and**
   - `existing.expires_at_ms > now_ms` (a restart is far faster than the 30 s TTL)
   → returns `LiveDoubleStart` → `Store::open` throws → the server exits non-zero.

The epoch bump *before* the mount claim guarantees a restart always carries a higher epoch than the
stale mount, so a crashed predecessor can only ever be recovered through the **`expired`** branch of
`claimMount` — which needs wall-clock time to pass. The current startup performs a **single-shot**
claim and aborts instead of waiting for that precondition. Any crash therefore wedges the server for
up to a full TTL, and only manual operator intervention (or luck with restart timing) recovers it.

This is not a durability defect — no data is lost, the owner/epoch machinery is correct on restart.
It is purely a **liveness** gap: startup refuses to wait out its own dead lease.

## What is already proven (no model change)

`docs/superpowers/models/CaCasMountCore.tla` already models and proves the safe fix:

- `ClaimMount`'s `expired` branch lets the owner reclaim its **own** lapsed mount and re-stamp it with
  the new `localEpoch` (a different, higher epoch than the stale body) — exactly the crash-restart
  reclaim.
- The liveness witness `W_SameUuidReclaimsExpired` shows that reclaim state is **reachable**.
- A live twin is safely excluded: while `mount.deadline > clock` and the epoch differs, `ClaimMount`
  is simply **not enabled** (fail closed); only `Renew` keeps the deadline ahead of the clock, so a
  holder that stops renewing (crashes) lets `Tick`s pass the deadline and makes the reclaim reachable.
- The three sabotage configs (`SabForeignTakeover`, `SabEpochReset`, `SabSupersededWrites`) each break
  their matching invariant and confirm the guards are load-bearing.

The model permits self-recovery; the **code** just never waits for the `expired` precondition. So the
model is **unchanged** and re-run as a regression to confirm it stays GREEN.

## Design

Replace the one-shot claim in `Store::open` step 4 with a **bounded wait-for-expiry** reclaim.

### Wait semantics

- Poll the mount by re-invoking the unchanged `claimMount` (which re-`head`s / re-`get`s each call):
  - `Claimed` → the lease lapsed and we reclaimed it under its observed token → proceed.
  - `ForeignOwner` → throw immediately (never wait across UUIDs).
  - `LiveDoubleStart` and `now_ms < wait_deadline` → sleep one poll interval, retry.
  - `now_ms >= wait_deadline` → throw the existing "actively mounted by another server" error. With
    the wait in place this now means: the lease never lapsed ⇒ a genuinely live second server holds it.
- `wait_deadline` is computed **once**, from the first `LiveDoubleStart` body:
  `wait_deadline = max(existing.expires_at_ms - now_ms, 0) + margin`, capped so we never block longer
  than one TTL plus margin. `margin ≈ poll_interval` (covers poll granularity + minor wall-clock skew).
- `poll_interval ≈ mount_renew_period / 2` — derived from existing config; **no new user setting**
  (YAGNI). A live holder renews every `mount_renew_period`, so polling twice as often reliably observes
  a still-live lease.

### Why the wait is safe (token-guarded, not timing-guarded)

The reclaim inside `claimMount` is a `putOverwrite` against the **observed token**. If a live holder
renewed the lease between our `head` and our `put`, the token changes and the `putOverwrite` fails →
`LiveDoubleStart` → fail closed. Correctness therefore does **not** depend on the poll interval or on
clock precision — a poll can never steal a lease from a holder that renewed after we read it. The wait
only decides *how long* we are willing to keep trying before declaring a double-start.

This is a wall-clock lease-expiry wait (the protocol is inherently time-based), not a `sleep` papering
over a race: we wait on the **condition** (lease lapsed ⇒ reclaim committed under token), bounded by
the lease deadline. It complies with the "no sleep for race conditions" rule.

## Components

### 1. `claimMountAwaitingExpiry` — new free function in `CasServerRoot.{h,cpp}`

Signature (alongside `claimMount`):

```cpp
MountClaimResult claimMountAwaitingExpiry(
    Backend & b, const Layout & l, const String & srid, UInt128 our_uuid, uint64_t our_epoch,
    std::function<uint64_t()> now_ms_fn,      // wall-clock reader (injectable for tests)
    uint64_t ttl_ms,
    uint64_t poll_interval_ms,
    uint64_t margin_ms,
    const std::function<void(uint64_t)> & sleep_ms_fn);  // injectable: real sleep in prod, fake in tests
```

- Loops over `claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms)`.
- On the first `LiveDoubleStart` from **our own** uuid, latches `wait_deadline` from `body.expires_at_ms`
  (capped at `now + ttl_ms + margin_ms`).
- Returns the first non-`LiveDoubleStart` result (`Claimed` / `ForeignOwner`), or the last
  `LiveDoubleStart` once `now_ms_fn() >= wait_deadline` (so the caller emits the double-start error).
- `sleep_ms_fn` is injected so unit tests drive a fake clock (advance `now_ms_fn`) with **no real
  sleeping**; production passes `std::this_thread::sleep_for` and a `system_clock` reader.

`claimMount` itself is **unchanged**.

### 2. `Store::open` step 4 — `CasStore.cpp`

- Replace the single `claimMount(...)` + `if (claim.kind != Claimed) throw` with a call to
  `claimMountAwaitingExpiry(...)`, passing `poll_interval = mount_renew_period/2`,
  `margin = poll_interval`, and the same `now_ms` lambda already defined there.
- Before entering the wait (i.e. when the first attempt returns `LiveDoubleStart` from our own uuid),
  `LOG_INFO` the holder's `uuid/epoch/pid/expires_at_ms` and the computed abort time, so an operator
  watching startup sees "waiting up to <T> for a possibly-stale self-mount to expire; will abort at
  <deadline> if a second server holds it."
- Keep the existing multi-line `ABORTED` error verbatim for the timeout and `ForeignOwner` paths.

## Error handling / fail-closed

- Foreign `server_uuid` on the mount → immediate throw, never waited (as today).
- Corrupt mount object → `decodeMountLease` throws `CORRUPTED_DATA` (as today).
- Wait timeout → existing "actively mounted by another server" `ABORTED` error (now correctly meaning:
  a live second server).
- `writer_epoch` is bumped before the wait; a subsequent timeout leaves it consumed — harmless (the
  counter is durable-monotone, never reused).

## Testing

### Unit (`InMemoryBackend`, injected `now_ms_fn` + `sleep_ms_fn` fake clock — no real sleep)

1. Stale same-uuid mount, `expires_at_ms` already in the past → `Claimed` on the first poll.
2. Stale same-uuid mount, `expires_at_ms` in the future; fake clock advances past it with **no** renew
   → `Claimed` after N polls; reclaimed body carries `our_epoch` and `seq = existing.seq + 1`.
3. Live holder: every poll advances `expires_at_ms` (simulated renew) → wait times out → throw
   double-start; assert the mount is **still** the holder's (not stolen).
4. Foreign uuid, live lease → immediate throw, **zero** polls (assert `sleep_ms_fn` never called).
5. Token-guard: `expires_at_ms` in the past but the token changes just before the reclaim `putOverwrite`
   → `LiveDoubleStart` (fail closed), not `Claimed`.

### TLA+ regression

`docs/superpowers/models/run_mount.sh` — positive gate + 3 sabotage configs — must stay GREEN. The
`.tla`/`.cfg` files are **not** modified.

### Integration / soak

- Remove the `CA_SOAK_NO_HARD_KILL` KILL→RESTART downgrade for **CH replicas** in
  `utils/ca-soak/soak/chaos.py` (restore real `docker kill -s KILL` of a ClickHouse replica).
  RustFS keeps its B145 graceful-only scoping — unrelated to this fix.
- Re-run the S13 scenario (`process loss during write+GC`): a hard-killed replica must become healthy
  again within ~TTL of restart, with no manual intervention, and rejoin the agreement/fsck checks.

## Non-goals

- No change to `claimMount`, the owner anchor, the epoch counter, or the watermark.
- No new user-facing configuration (wait bound derives from existing `mount_lease_ttl_ms` /
  `mount_renew_period`).
- Does not address the different-uuid case (regenerated ClickHouse uuid file) — that remains a fail-closed
  `CORRUPTED_DATA` foreign-owner condition with its existing operator guidance.
- Does not make startup wait interruptible by an in-flight server shutdown; the wait is bounded by
  TTL+margin (~30 s), which is acceptable for a disk-open path. Can revisit if it proves disruptive.
