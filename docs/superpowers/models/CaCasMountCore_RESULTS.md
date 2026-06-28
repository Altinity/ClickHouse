# CaCasMountCore — TLC results (mount ownership + server-root identity gate)

Model: `CaCasMountCore.tla`. Checker: TLC (`tmp/tla2tools.jar`), bounded
`Actors={A,B}`, `None=NoneVal`, `MaxClock=4`, `MaxEpoch=3`, `TTL=2`.
Run: `./run_mount.sh <cfg>` (configs below; `CHECK_DEADLOCK FALSE` because the
bounded spec legitimately terminates).

This is **bounded model checking, not a proof**: TLC exhausts all interleavings
within the finite bounds. A clean check is strong evidence, not a theorem for
unbounded clocks/epochs.

## What is modelled

One modeled `server_root_id` with three objects and two server `Actors` (`A`,`B`,
each a distinct fixed ServerUUID):

- `owner` ∈ `Actors ∪ {None}` — sticky identity; once set, never changes
  (first-writer-wins server-root claim).
- `epoch` ∈ `0..MaxEpoch` — the durable monotone counter, living in its **own**
  object (NOT the mount).
- `mount` — `None`, or a TTL lease record `[uuid, epoch, deadline]`.

A mount lease is a CAS register; every claim / adopt / reclaim is **one atomic
action** (`ClaimMount`). Abstract time is the global `clock`; `Tick` advances it
so a deadline can pass and a reclaim becomes reachable. `Die` marks a holder lost
so a later expiry/reclaim is reachable. `Write` is a mutation gated on a live, own,
current-epoch, not-lost mount. Three CONSTANT booleans (`Sab*`, default FALSE,
woven in exactly as `CaGcLeaseCore`'s `EnableHeartbeat`) reproduce each bad state.

## Invariants (the GATE)

- **`TypeOK`** — type/bounds sanity.
- **`NoTwoServerUuidsOwnSameServerRoot`** — `owner # None => owner = firstOwner`
  (owner is sticky; never transitions to a different non-None value).
- **`ForeignUuidNeverAutoTakesOver`** — `mount # None => mount.uuid = owner`
  (a mount is never held by a non-owner, for ANY clock/deadline).
- **`WriterEpochMonotoneUnique`** — no two distinct actors share a written epoch,
  AND the durable `epoch` counter is a monotone ceiling over every written epoch
  (`∀x∈wrote : x[2] ≤ epoch`).
- **`SupersededWriterMakesNoMutation`** — `~lostThenWrote`: once an actor is
  superseded (`localLost`), no NEW mutation of its enters `wrote`.

Liveness witness (separate cfg, asserted as an INVARIANT so a reachable good
state reports VIOLATED):

- **`W_SameUuidReclaimsExpired`** == `~(∃a : reclaimed[a])` — VIOLATED means an
  actor reclaiming its OWN expired mount is reachable.

## Results

| config | flags | invariant(s) | distinct states | verdict |
|---|---|---|---|---|
| `CaCasMountCore_stage1` | all `Sab*`=FALSE | all 5 | 6,085 | **PASS** — `Model checking completed. No error has been found.` |
| `CaCasMountCore_sab_foreigntakeover` | `SabForeignTakeover`=TRUE | ForeignUuidNeverAutoTakesOver | 933 (to 1st violation) | **VIOLATED** — `Error: Invariant ForeignUuidNeverAutoTakesOver is violated.` |
| `CaCasMountCore_sab_epochreset` | `SabEpochReset`=TRUE | WriterEpochMonotoneUnique | 653 (to 1st violation) | **VIOLATED** — `Error: Invariant WriterEpochMonotoneUnique is violated.` |
| `CaCasMountCore_sab_supersededwrites` | `SabSupersededWrites`=TRUE | SupersededWriterMakesNoMutation | 681 (to 1st violation) | **VIOLATED** — `Error: Invariant SupersededWriterMakesNoMutation is violated.` |
| `CaCasMountCore_witness_reclaim` | all `Sab*`=FALSE | W_SameUuidReclaimsExpired | 811 (to 1st violation) | **VIOLATED (expected)** — `Error: Invariant W_SameUuidReclaimsExpired is violated.` (state reachable) |

(Sabotage / witness distinct-state counts are "states to first violation" and vary
slightly run-to-run because `-workers auto` parallelizes the BFS and halts at the
first counterexample. `stage1` exhausts the full space deterministically: 6,085.)

## Counterexamples (action sequences)

**`sab_foreigntakeover`** (8 states) — foreign uuid auto-takes-over an expired mount:
```
Tick,Tick -> ClaimOwnerEmpty(A) -> ClaimMount(A)            (owner=A, mount=A, deadline=6)
Tick,Tick (clock=4; mount stays live here) -> ClaimMount(B)
   => with the owner=a guard dropped on the expired branch, B (a NON-owner)
      installs mount=[uuid|->B,...] while owner=A   =>  mount.uuid (B) # owner (A)
```
Final state: `owner = A`, `mount = [epoch|->0, deadline|->6, uuid|->B]`.

**`sab_epochreset`** (6 states) — non-monotone epoch via reset:
```
ClaimOwnerEmpty(A) -> AllocEpoch(A) (epoch=1, localEpoch[A]=1)
SabResetEpoch (mount=None, epoch:=0) -> ClaimMount(A) -> Write(A) tags <<A,1>>
   => wrote={<<A,1>>} but epoch=0, so 1 <= epoch is FALSE  (ceiling dropped below a written epoch)
```

**`sab_supersededwrites`** (6 states) — a superseded holder mutates:
```
Tick -> ClaimOwnerEmpty(A) -> ClaimMount(A) -> Die(A) (localLost[A]=TRUE)
Write(A): with the epoch/not-lost conjunct dropped, the lost A writes
   => lostThenWrote=TRUE  =>  SupersededWriterMakesNoMutation violated
```

**`witness_reclaim`** (7 states, expected VIOLATED = reachable):
```
Tick -> ClaimOwnerEmpty(A) -> ClaimMount(A) (deadline=clock+TTL)
Tick,Tick (deadline passes) -> ClaimMount(A) again
   => own-expired reclaim branch: reclaimed[A]=TRUE  =>  witness reachable
```

## Reproduce

```bash
cd docs/superpowers/models
for c in CaCasMountCore_stage1 \
         CaCasMountCore_sab_foreigntakeover \
         CaCasMountCore_sab_epochreset \
         CaCasMountCore_sab_supersededwrites \
         CaCasMountCore_witness_reclaim; do
  echo "===== $c ====="
  ./run_mount.sh "$c"
done
# stage1 -> "No error has been found"; the four others -> "Error: Invariant ... is violated"
```
