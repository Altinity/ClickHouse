# CA GC lease — advisory heartbeat to stop the false-steal livelock (B160)

**Status:** design, awaiting review · **Date:** 2026-06-16 · **Branch:** `cas-mergetree-poc`
**Backlog:** B160. Brainstorm approved Approach A (clock-free lease + advisory heartbeat). B163 (prefix-sharded parallel GC) is a SEPARATE, larger effort and explicitly out of scope here.

## Problem

Both replicas mounting a shared CA pool run `CasGcScheduler` against one `gc/state`, picking a single GC leader via a deliberately **clock-free observation-window steal** (`Gc::acquireOrRenewLease`, `CasGc.cpp:1083`):
- A follower that sees the incumbent's `(owner, seq)` **change** between two of its own ticks treats it as alive → backs off.
- A follower that sees the **same** `(owner, seq)` across its window concludes the incumbent is dead → **steals** (`owner=gc_id, seq++, fence_seq++`, one atomic CAS).

But the leader bumps `lease.seq` **only once per round** (the renew at round start). A round (`fold`→`retire`→`fence`→`recheck`→`cascade`, each HEAD-ing candidates over the snap) takes **many seconds** on a large/contended pool — *longer than the follower's tick interval*. So:
1. Leader A starts round N, `seq=k`, then spends >1 tick folding/retiring.
2. Follower B ticks, sees `(A,k)`, records it, backs off.
3. B ticks again, sees `(A,k)` **still unchanged** (A alive, mid-round) → concludes A dead → **steals**.
4. A finishes; its `retire` gc/state CAS now Conflicts → `Code 236 ABORTED "gc/state moved during retire"` — a whole wasted round.

Measured: **~70–80% of GC rounds abort** this way (soak run #5: ch1 300 failed / 84 ok / 250 retire-contention). The loser re-scans the same+growing candidate set every tick, HEAD-ing objects the other replica already deleted → the **404-HEAD storm** and non-advancing `gc/state` (reclamation barely progresses). It is wasteful + load-amplifying, **not** a loss/dangle (the publish gate, not the lease, is the correctness mechanism; the lease is a liveness optimization, and is currently OUTSIDE the TLA+ model).

Root cause in one line: **the liveness heuristic mistakes a slow-but-alive leader for a dead one, because renewal cadence (once/round) is coupled to round duration while the steal window is one tick.**

## Goal

Decouple leader-liveness from round duration so a slow-but-alive leader is **never** falsely stolen from — while preserving (a) the clock-free property (no cross-node clock comparison), (b) automatic failover for a genuinely dead leader, and (c) the existing safety mechanism (atomic single-CAS steal + `fence_seq` epoch isolation) **unchanged**. Target: GC retire-contention drops from ~70–80% to ~0 on the two-replica soak.

Non-goals: parallelizing GC across prefixes (B163); a watchdog for an alive-but-wedged round thread (noted as a follow-up).

## Design — Approach A: advisory heartbeat

The pulse is an **additional** liveness signal OR'd into the existing one. Ownership + epoch and the steal itself are untouched.

### 1. New key `gc/hb` — pure liveness

`layout.gcHbKey()` → `{owner: UInt128, hb_seq: uint64}` (small; binary codec, same style as the gc/snap codec). It carries **no durable round state**, so unlike `gc/state` (which must never legally vanish) it is **recreatable-if-absent**.

### 2. Heartbeat thread in `CasGcScheduler`

A second, lightweight thread (lifecycle tied to `start()`/`stop()`). While this node believes it is the leader, it bumps `gc/hb` every `H` seconds, **independent of round progress**:
- The round thread sets `std::atomic<bool> i_am_leader` true after a successful acquire/renew (`report.acquired_lease`), false on any non-acquire (backoff / lost steal / exception).
- The heartbeat thread loop, while `i_am_leader`: read `gc/hb`; if absent or `owner == gc_id`, CAS-write `{gc_id, hb_seq+1}` against the read token; if `owner != gc_id` (leadership lost without the round thread noticing yet), stop bumping — the round thread discovers the loss on its next tick. `H` is a **local monotonic timer** (pacing), never compared to another node's clock.

### 3. The steal decision (the ONLY change to `acquireOrRenewLease`)

Today step 4 (`CasGc.cpp:1147-1159`) steals when the incumbent's `(owner, seq)` is identical to our remembered observation (`!incumbent_renewed`). Add the heartbeat as a second liveness check:
```
incumbent_alive = gc/state.(owner,seq) changed since our last observation   // existing
               OR ( gc/hb.owner == gc/state.owner                            // not a zombie pulse
                    AND gc/hb.hb_seq advanced since our last observation )    // new
steal ONLY IF NOT incumbent_alive
```
A live-but-mid-round leader has `gc/state.seq` frozen but `gc/hb.hb_seq` advancing ⇒ `incumbent_alive` ⇒ **no steal**. The false-steal vanishes. A dead leader has both frozen ⇒ steal — via the **same atomic single-CAS** on `gc/state` as today (`owner++, seq++, fence_seq++`), so the zombie-steal protection is unchanged.

### 4. Observation state

Extend the Gc instance's remembered observation (`last_seen_owner`, `last_seen_seq`, `has_observation`) with `last_seen_hb_owner`, `last_seen_hb_seq`. `rememberObservation` records both the `gc/state` lease and the `gc/hb` pulse read in the same acquire pass.

### 5. Fallbacks (never-worse-than-today)

- `gc/hb` **absent** (fresh pool, or leader hasn't created it yet) ⇒ no liveness signal ⇒ fall back to the existing `gc/state.seq` observation protocol. Worst case = today's behavior, never worse. The leader creates `gc/hb` on its first heartbeat.
- `gc/hb.owner != gc/state.owner` (a displaced ex-leader still pulsing) ⇒ ignore the pulse (bind liveness to the **current** `gc/state` owner); fall back to `gc/state.seq`.

### 6. Clock-free preserved

A follower judges liveness purely by **observing a monotonic counter change across its own ticks** (`gc/state.seq` and/or `gc/hb.hb_seq`). No node compares its wall clock to another's — exactly as today. The heartbeat's `H`-second cadence is local pacing only.

## Safety

`fence_seq` epoch isolation and the **atomic single-CAS steal on `gc/state`** are unchanged, so no-loss / no-dangle (already proven for `{L1,L2}` in `CaIncarnationCore`) holds for any interleaving and any number of steals. The heartbeat affects only **liveness/efficiency** (whether and when a follower attempts the still-atomic steal), never the durable commit/retire/fence paths.

### Tuning

`H` (heartbeat period) ≪ `W` (the follower's observation window = its tick interval = `gc_interval_sec`). With `H ≤ gc_interval/3` and the follower requiring `gc/hb.hb_seq` frozen across a full tick before stealing, a live leader's pulse always advances within `W` ⇒ no false steal; a dead leader's pulse freezes ⇒ steal after `W`. Both are observation-counted, not clock-compared.

## TLA+ (required, like B136/B137)

The lease/steal is currently **outside** the model. Add a focused model (extend `CaIncarnationCore` stage-5 multi-leader, or a dedicated `CaGcLeaseCore.tla`) covering: the observation-window steal protocol, the heartbeat pulse, and a long-round (multi-step) leader.
- `SabotageNoHeartbeat`: with the heartbeat disabled (or not bumped during a long round), an alive-but-slow leader IS stolen from ⇒ reproduces the B160 false-steal (the model must yield this counterexample).
- Prove WITH the heartbeat: an alive leader (pulse advancing) is never stolen from (no false steal); and SAFETY (no-loss/no-dangle) holds for any number of steals (extend the existing `{L1,L2}` invariants — the steal path is unchanged, so this should hold by the existing fence_seq argument).

## Testing (TDD)

- **Unit (`gtest_cas_*`)**: two `Gc` instances over a shared `CasInMemoryBackend` (the existing test backend). (a) Leader A renews + heartbeats while its round is artificially slow → follower B `acquireOrRenewLease` returns NOT-acquired (no steal) across multiple ticks. (b) Leader A stops heartbeating (simulated death) + `gc/state.seq` frozen → B steals (acquires) after the window. (c) `gc/hb` absent → falls back to the seq-only protocol (today's behavior). (d) zombie pulse (`gc/hb.owner` ≠ `gc/state.owner`) → ignored.
- **No regression**: full `Cas*`/`CaWiring*` suite stays green (only the pre-existing B140 `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` red).
- **Soak (headline)**: the two-replica soak GC retire-contention drops from ~70–80% to ~0; `gc/state.round` advances steadily; the 404-HEAD storm subsides.

## Risks

1. **Heartbeat-thread lifecycle race** — the round thread sets/clears `i_am_leader` while the heartbeat thread reads it. Use `std::atomic<bool>`; a brief stale read only means one extra/missing pulse, which the observation protocol tolerates.
2. **Brief dual-heartbeat during handoff** — a just-stolen-from ex-leader may pulse once more before its round thread notices; the `gc/hb.owner == gc/state.owner` gate makes followers ignore a stale-owner pulse, and `gc/state` ownership stays authoritative.
3. **Alive-but-wedged round thread, heartbeat alive** → no steal (conservative; GC stalls until the wedged leader recovers/dies). Separate failure mode → **backlog follow-up** (a progress-watchdog), not B160.
4. **Extra op** — one small-object `gc/hb` CAS per `H` seconds per leader; negligible vs a round's thousands of HEAD/GET/PUT.

## Out of scope
- B163 prefix-sharded parallel GC (separate spec/milestone).
- A wedged-round-thread watchdog (risk 3) — backlog.
