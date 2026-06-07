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
| strong-read fence + stealing-leader out-wait (split-brain visibility) | leadership = lowest-seq ephemeral, re-checked with a **`sync`-ed** read before each delete; **fail-close on `Disconnected`** |
| the `Δ_skew` inter-clock lease math | **gone** — the ephemeral session is the lease; no two clocks are compared |

**NOT collapsed (review R3 — `2026-06-07-ca-gc-keeper-only-review.md`):** the **closed-epoch S3 fold barrier
is RETAINED**, not deleted. `log` lives in S3; Keeper's linearizable `{O_W}` snapshot does **not** create a
happens-before edge into the writer's S3 `+` durability (no cross-system ordering between a Keeper op and an S3
op). So the base plan's §5.2 R0/R1 closed-epoch fenced fold + the writer-ordered `flush-+-then-advance` (S3 `+`
durable *before* the Keeper `O_W` advance) stay exactly as in the EBR plan. Keeper erases **3 of 4** S3 hazards
(fence counter, `2·Δ_skew` lease, fence-steal out-wait) — not the cross-system fold barrier.

The safety hinge, stated correctly: **a writer self-fences fail-stop on `Disconnected`** (NOT on confirmed
`Expired`), using a **local-elapsed-time deadline inside `T_session`**, and the leader likewise fail-closes on
`Disconnected`. This is **not** "no timing assumption" — it is the *session-timeout* assumption (local elapsed
vs one timeout), which is strictly cleaner and weaker than the S3 mode's inter-clock `Δ_skew`, but not zero.

> **TLC-verified (CE-4, `docs/superpowers/models/`):** `Disconnected`-detection *alone* is **insufficient** —
> the model lost data via a server-expired-but-still-`Connected` writer until the **local-deadline self-fence
> inside `T_session`** was encoded (a consequential op requires `Connected ∧ session-alive-by-local-clock`).
> The local deadline is load-bearing, not belt-and-suspenders.

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

**Recovery-transient fixes (review R3 — INV-S3-COMPLETE does NOT hold for the live-writer transient without these):**
- **Retention backstop + reconcile grace (must-fix #4).** After a wipe there are *no* live leases, so a ref
  published *during* the outage (or a blob whose `+` was in-flight) has no lease/`+` root. Post-restore
  reconcile must therefore (a) treat a **conservative time-retention window** as a root — never reclaim
  anything younger than retention — exactly the Iceberg/Delta rule the base plan §10 keeps; and (b) run only
  *after* the leader's recovery epoch-bump, deriving roots from `refs/` (the durable, written-last authority).
- **Purge backup-restored ephemerals (must-fix #6).** If Keeper is restored from a *persistent snapshot*
  rather than coming up empty, ghost `leader`/`writers/<S>` znodes resurrect and break the "ephemeral vanished"
  assumption. The recovery procedure must delete all `writers/` and election znodes before re-electing.
- **`sync`-ed reads (must-fix #5).** The `safe_epoch := E_cur` empty-writer branch, and the leader's lowest-seq
  fence re-check, must use a **`sync`-ed** Keeper read — default reads are sequentially-consistent, not
  linearizable, so a stale read could under-count live writers or miss a steal.

The cross-outage correctness requirements are therefore the §3 hinge (writers + leader fail-stop on
`Disconnected`, session-timeout assumption — **not** clock-skew-free) **plus** the retention-backstop reconcile
and the backup-purge above. With those, S3 alone remains the recoverable source of truth.

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

Keeper-only is both **simpler** (deletes **3 of 4** S3 workarounds — the fence counter, the `2·Δ_skew` lease,
and the fence-steal out-wait; the **closed-epoch S3 fold barrier is RETAINED**, since Keeper cannot order S3
writes — review R3) and **safer** (no inter-clock-skew dependency; the hinge becomes fail-stop on
`Disconnected` against the session timeout — a cleaner, weaker timing assumption, not its elimination). It
keeps the **S3-COMPLETE** invariant: Keeper is O(active writers) of throwaway coordination, and S3 alone is the
recoverable source of truth. This is the variant to spec and build.

**Open items carried forward:** (1) confirm the §4 flush-`+`-then-advance coupling is expressed cleanly with
Keeper's linearizable `O_W` publish; (2) measure round-trips per INSERT; (3) the leader's one-time epoch bump
on recovery — prove it fences pre-outage in-flight epochs without stranding a just-published ref.

**NEW open question from TLC (CE-2 — `generation==epoch` vs long-lived dedup):** the model proved
"reuse only a generation `e ≥ O_W`" sufficient for safety, but combined with `generation==epoch` that would
**break long-lived dedup** — a stable `g=0` blob keeps generation 0 while `O_W` climbs unboundedly (one per GC
round), so `e ≥ O_W` would forbid de-duplicating against it and force wasteful resurrection. Resolve before
spec: most likely the dedup-preserving variant **"reuse + make `+` durable + re-check the tombstone; resurrect
only if now condemned"** (which the model did NOT encode — it used the stricter `e ≥ O_W`), or **decouple
generation from epoch** (a small per-blob generation counter, advanced only on actual resurrection, instead of
stamping the global epoch). The safety the model proved is real; the dedup-cost of the *particular* rule it
used is the thing to re-engineer.

**TLC status:** the stabilized core (closed-epoch fold + flush-`+`-then-advance + reuse-under-fresh-`O_W` +
session-alive self-fence + fenced/fail-close leader + `e+2`/`safe_epoch` reclaim + retention backstop) passes
exhaustive bounded model-checking (`INV_NO_LOSS/NO_DANGLE/NO_ABA`, 135M states, expiry+split-brain+wipe) —
see `docs/superpowers/models/RESULTS.md` for bounds and the residual untested surface.
