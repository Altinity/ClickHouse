# CA GC — Keeper-only coordination profile (S3 is the complete durable truth)

The recommended coordination profile for the EBR GC (`2026-06-07-ca-gc-ebr-design-plan.md`). It collapses the
S3-only coordination machinery (the things the reviews kept finding holes in) by letting Keeper provide
linearizability — **while keeping the hard rule that Keeper holds no durable state.**

## 1. The governing invariant {#invariant}

> **S3-COMPLETE:** every durable fact lives in S3 and is self-describing. Keeper holds **only ephemeral
> coordination** whose size is **O(active writers)** — never O(objects) and never O(epochs). Losing Keeper
> entirely (even wiping it to empty) loses **no durable state**: the full pool state is understandable and
> rebuildable from S3 alone.

This is what makes Keeper a true *accelerator* (G4), not a second database. It also keeps Keeper light: a
handful of tiny znodes (one per live writer + the leader election), kilobytes total — Keeper memory does
**not** grow with the number of blobs, parts, epochs, or tombstones.

## 2. The state split {#split}

| Fact | Where | Recoverable from S3? |
|---|---|---|
| blobs / manifests `blobs/<H>/<e>`, `parts/<id>/<e>` | **S3** | yes (LIST) |
| refs (roots / commit points) | **S3** | yes (LIST) — the authoritative reachability source |
| condemn markers `<e>.tombstone` | **S3** | yes |
| `epoch/current` (authoritative epoch) | **S3** (leader writes under fence) | yes |
| `log/<e>/<shard>` (+/- deltas), `snap/<e>/<shard>` (folded counts) | **S3**, sharded | yes; and `snap` is a *cache* of ref-reachability, rebuildable by reconcile |
| leader election + **fence token** | **Keeper** (ephemeral-sequential; fence = seq #) | transient — re-elected; no durable loss |
| writer lease + observed epoch `O_W` | **Keeper** (ephemeral session node, one per live writer) | transient — re-registered |

Keeper = `{ leader-election znode, writers/<session> = O_W }`. That's it. Nothing per-object.

## 3. What Keeper-linearizability collapses {#collapse}

Every S3-only VIOLATION the reviews found was S3 *working around its lack of linearizability*. Keeper removes
them outright:

| S3-only mechanism (now deleted) | Keeper replacement |
|---|---|
| `create-if-absent` fence **counter** (O(n) scan) | **ephemeral-sequential** znode; the sequence number *is* the monotone fence (free) |
| writer time-lease + `2·Δ_skew` + read-back-durable renewals (clock-skew-dependent) | **ephemeral session** — Keeper's session timeout is the single arbiter; **no clocks compared**, no skew |
| strong-read fence + stealing-leader out-wait (split-brain visibility) | leadership = "my ephemeral node is still the lowest seq"; **linearizable**, no visibility race |
| closed-epoch fold S3 barrier + the `Δ_skew` math | the leader reads a **linearizable snapshot** of `{O_W}` via `getChildren(writers/)`; quiescence is exact |

The one safety hinge reduces to its clean form: **a writer self-fences (goes read-only) the instant its Keeper
session is lost** — and it *knows*, because its Keeper ops start failing. No clock-skew assumption survives.

## 4. The collapsed protocol {#protocol}

```
WRITER (Keeper session S, observed epoch O_W = read epoch/current from S3, cached; refreshed)
  - while session S alive: may commit. Session lost  -> immediately READ-ONLY, abort in-flight commit.
  - obtain H, early-`+` to log/<O_W> (durable in S3), publish ref last  (as in the plan; epoch == generation).
  - publish O_W in the ephemeral node writers/<S>; advance it only after its `+`s are durable+folded (the plan's
    flush-`+`-then-advance rule). Keeper makes "no live writer is still in epoch e" a linearizable read.

GC LEADER (holds the lowest-seq election znode = fence F)
  R0 CLOSE   epoch/current := E_cur+1 in S3 (only the fence-holder writes it).
  R1 FOLD    only CLOSED epochs; per-shard STREAMING MERGE-SORT of log deltas into the sorted snap shard
             (O(delta + touched shards), bounded memory) -> count-0 candidates.   <-- sharded; not O(all objects)
  R2 CONDEMN createIfAbsent <e>.tombstone.
  R3 QUIESCE safe_epoch := min(O_W) over getChildren(writers/)  [linearizable; dead writers' nodes already gone].
             If no live writer -> safe_epoch := E_cur.
  R4 RECLAIM delete (H,e_a) iff  E_cur >= e_a+2  AND  safe_epoch > e_a  AND still-unreferenced AND I am still
             the lowest-seq leader (re-check F). A NEW leader re-folds + re-quiesces under its own F first.
```

No `Δ_skew`, no fence counter, no strong-read gymnastics, no S3 close-barrier — Keeper gives the ordering.

## 5. Recovery: total Keeper loss {#recovery}

1. **During the outage:** every writer's session drops → each goes **read-only** and aborts any in-flight
   commit (no new ref, no reuse). The GC leader loses its election node → **GC stops** (no condemn, no delete).
   The system is *paused but safe*: no mutation that could dangle or lose data happens with Keeper down.
2. **On restore (even empty Keeper):** a GC leader re-elects (fresh ephemeral-seq, fence from seq #). It reads
   `epoch/current` from **S3** and bumps it once (open a fresh epoch, fencing off any pre-outage in-flight
   epoch). Writers reconnect, create new session nodes, observe the current epoch, resume.
3. **Snapshot trust:** if `snap` integrity is in any doubt, **reconcile** rebuilds it from S3 ground truth —
   a full reachability pass over `refs/` → manifests → blobs. Refs are durable and written-last, so S3
   reachability is the authority; `snap`/`log` are only the fast filter. Thus **S3 alone suffices** to
   understand and rebuild the entire state.

The only correctness requirement across the outage is the §3 hinge — writers self-fence on session loss —
which Keeper makes reliable (failed ops), with **no clock-skew assumption**.

## 6. Scale notes (Milovidov #3) {#scale}

- **Snapshot fold is O(delta), not O(objects).** `snap` is sharded by hash prefix; a round folds only the
  shards touched by that round's deltas, via streaming merge-sort (sorted `snap` shard ⋈ sorted log deltas),
  bounded memory. We already build it this way. So the "fold is a hidden full scan" concern does not apply on
  the hot path; the only O(all-objects) pass is **reconcile**, which is off-hot-path and rare.
- **Write amplification:** `+`/`-` deltas are **coalesced** (group-commit, one log object per (shard, window)
  via the log writer), so it is O(commits/window), not one PUT per blob. (Still worth measuring end-to-end
  round-trips per INSERT — open item.)
- **Keeper load:** O(active writers) tiny znodes + heartbeats. Independent of pool size. Light.

## 7. Verdict {#verdict}

Keeper-only is both **simpler** (deletes the fence counter, the `2·Δ_skew` lease, the strong-read fence, and
the closed-epoch S3 barrier — the entire class of S3-eventual-consistency workarounds the reviews flagged) and
**safer** (no clock-skew dependency; the hinge becomes "session lost ⇒ read-only", which Keeper enforces). It
keeps the **S3-COMPLETE** invariant: Keeper is O(active writers) of throwaway coordination, and S3 alone is the
recoverable source of truth. This is the variant to spec and build.

**Open items carried forward:** (1) confirm the §4 flush-`+`-then-advance coupling is expressed cleanly with
Keeper's linearizable `O_W` publish; (2) measure round-trips per INSERT; (3) the leader's one-time epoch bump
on recovery — prove it fences pre-outage in-flight epochs without stranding a just-published ref.
