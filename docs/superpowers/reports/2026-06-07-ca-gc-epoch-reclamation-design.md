# CA GC — Epoch-Based Reclamation (EBR) design

Formalization + analysis of the epoch/quiescence GC proposed for the content-addressed (CA) backend.
This is **distributed Epoch-Based Reclamation** — the RCU/quiescent-state pattern, mapped to S3/Keeper.
It is the answer to the correctness review (`2026-06-07-ca-gc-simplified-correctness-review.md`), whose
killer findings were all *"a writer the GC didn't observe references/reuses a blob the GC deletes"*: EBR makes
that impossible by construction — a writer the GC hasn't observed-past **holds reclamation back** instead of
losing data.

## 0. The one idea {#idea}

Reclamation of an object condemned in epoch `e_a` is deferred until **every live writer has observed an epoch
> e_a**. Once a writer has observed past `e_a`, it has seen the removal-candidate set for `e_a`, so it will
never reuse a condemned object — it routes to a fresh epoch instead. Therefore, after all writers pass `e_a`,
**no new reference to a condemned object can ever appear**, the fold becomes authoritative, and the delete is
safe. A slow/paused writer simply doesn't advance its observed epoch → reclamation of newer epochs stalls
(a *liveness* cost) → it can never cause *loss*. This is the EBR grace period; "epoch quiescence" replaces the
unsafe wall-clock grace the review rejected.

## 1. State (objects / nodes) {#state}

Everything is `create-if-absent` / `read` / `delete` / `list` + a fenced leader + per-writer leases.

```
epoch/current                     the current epoch E_cur (monotone; advanced only by the fenced GC leader)
blobs/<H>/<e>                      immutable content bytes of hash H, created in epoch e   (generation == epoch)
blobs/<H>/<e>.tombstone           per-object CONDEMN marker = "this object is a removal candidate"
active/<H>                        hint: the epoch of the current reusable object for H (plain PUT; LIST-derivable)
store/.../refs/<part>             live ref (commit point / GC root) -> part_id  (unchanged)
parts/<part_id>/<e>               manifest, same epoch/condemn lifecycle as a blob
log/<e>/<shard>/<event_id>        reference deltas (+ / -) appended in epoch e        (the sloppy candidate filter)
snap/<e>/<shard>                  folded counts (the reverse index)
candidates/<e>                    OPTIONAL roll-up of what was condemned at epoch e (for reconcile); the
                                    authoritative per-object signal is the <e>.tombstone (point-lookup)
writers/<W>                       writer W's lease: { observed_epoch O_W }  (Keeper EPHEMERAL; S3 object + TTL)
gc.lock                           fenced GC-leader lock { server_id, fence_token, lease_deadline }
```

Key unification: **generation == epoch.** A resurrected object is just the same content `H` re-created under
the *current* epoch (`blobs/<H>/<E_cur>`), a different key from the condemned `blobs/<H>/<e_a>`. There is no
separate per-blob generation counter — one global epoch does both jobs.

## 2. Writer protocol {#writer}

```
WRITER W committing a part that needs content H
────────────────────────────────────────────────
0. LEASE: ensure writers/<W> is live with observed_epoch O_W := read(epoch/current); refresh it.
   *** FENCING RULE: if W's lease ever lapsed, W MUST re-register and re-read state before step 2/3. ***
1. obtain content H:
   a. read active/<H>  (or LIST blobs/<H>/)
   b. ABSENT  -> createIfAbsent blobs/<H>/<O_W>; upload bytes; createIfAbsent active/<H> = O_W.   use (H,O_W)
   c. PRESENT -> (H,e): HEAD blobs/<H>/<e>.tombstone
        - no tombstone   -> dedup-reuse (H,e)
        - tombstone here -> do NOT reuse; createIfAbsent blobs/<H>/<E_cur>; upload; PUT active/<H>=E_cur;
                            use (H,E_cur)              (resurrection == create under the current epoch)
2. append `+` for the chosen (H,e) into log/<O_W>/...
3. publish the live ref  (commit point, written LAST)
4. (drop, later) remove the ref first, then append `-`.
```

The writer's lease (step 0) is the **pin** the review demanded — but **one per writer, not one per blob**, and
it is an *epoch* the GC reads, not a per-blob object. That is the central simplification over `sessions/<id>`.

## 3. GC leader protocol (fenced, single deleter) {#gc}

```
GC LEADER (holds gc.lock with fence F)
──────────────────────────────────────
ROTATE (each round, ~10 min):
  R1. fold log/<=E_cur> into snap; objects with no live reference -> CONDEMN: createIfAbsent
      blobs/<H>/<e>.tombstone  (records condemnation; condemnation epoch e_a := E_cur).
  R2. E_cur := E_cur + 1     (fenced PUT epoch/current; the rotation barrier).
RECLAIM (each round):
  Q1. safe_epoch := min(O_W) over LIVE writer leases (expired leases ignored; if none, safe_epoch := E_cur).
  Q2. for each condemned (H,e_a):
        if safe_epoch > e_a  AND  (H,e_a) still unreferenced in the fold  AND stillLeader(F):
            DELETE blobs/<H>/<e_a>;  (leave/refresh active/<H>);  drop its tombstone & candidate entry.
        else: keep — re-presented next round.
```

`stillLeader(F)` (re-read `gc.lock`, confirm fence `F`) gates **every** DELETE and the `epoch/current` PUT.

## 4. Why it is safe {#safety}

Invariants:
- **INV-NO-LOSS:** an object is deleted only if unreferenced *and* no future reference can appear.
- **INV-NO-DANGLE:** a published ref always resolves to present bytes.
- **INV-NO-ABA:** delete and re-create never collide on the same key.

Argument (Lamport happens-before):
- **INV-NO-ABA** — resurrection creates `blobs/<H>/<E_cur>`, a *different key* from the condemned
  `blobs/<H>/<e_a>` (`E_cur > e_a`). `DELETE(e_a)` cannot touch `E_cur`. Holds with only `create-if-absent`.
- **INV-NO-LOSS / NO-DANGLE (the EBR core).** A delete of `(H,e_a)` requires `safe_epoch > e_a`, i.e. every
  live writer has `O_W > e_a`. A writer with `O_W > e_a` executed step-0 *after* the rotation that closed
  `e_a`, so its read of `active`/tombstones (step 1c) **happens-after** the condemnation of `(H,e_a)` —
  therefore it observes the tombstone and routes to a fresh epoch; it cannot emit a `+` for `(H,e_a)`. Any
  writer that *did* reference `(H,e_a)` had `O_W ≤ e_a`, which **forces `safe_epoch ≤ e_a`**, so the delete is
  not yet permitted, and its `+` is in `log/<≤e_a>` which the fold sees → `(H,e_a)` is not condemned/deleted.
  The two cases are exhaustive: at the delete point, `(H,e_a)` is unreferenced and *no new reference can
  appear*. ∎

This is exactly why it defeats the review's V1/V3/V4: a **paused** writer keeps its (still-live) lease at a low
`O_W`, pinning `safe_epoch` low, so the GC *cannot* advance reclamation past the epoch the paused writer might
still be acting in. Pauses cost liveness, never safety — the property wall-clock grace could not provide.

## 5. The one hinge everything rests on: the time-bounded epoch lease {#hinge}

The proof's "live writer" set must be **exactly** the writers that can still act. The danger is a writer whose
lease the GC treats as *expired/dead* but which is actually only *paused* and then resumes to reuse a
now-deleted blob. This is closed by a **time-bounded epoch lease with writer self-fencing** — and, crucially,
this lets the writer **commit without re-consulting `epoch/current` or the candidate set at the end**:

- **Lease validity window.** When W observes `O_W := epoch/current` at wall-clock `t`, that observation is
  valid only until `lease_until := t + T_lease`. W refreshes it in the background (re-read current epoch,
  advance `O_W`, re-read candidate sets up to the new `O_W`, extend `lease_until`).
- **Writer self-fence (local, no network read).** Immediately before publishing the ref (the commit point,
  step 3), W checks `now < lease_until` — a *local clock* check, free. If stale (or it could not refresh), W
  **goes read-only and rejects the write**. So W never commits under stale epoch data, and it needs **no
  end-of-commit round-trip** to re-read the epoch/candidates — the lease window guarantees freshness.
- **GC lease-out-wait.** Before dropping W from the live set (and advancing `safe_epoch` past the epoch W
  observed), the GC waits `lease_until + Δ_skew`. So any writer that could still pass its local self-fence is
  still counted by the GC.

**Safety (closes the EBR proof without the commit re-read).** A writer that commits a reuse of `(H,e_a)` at
`t_commit` had `t_commit < lease_until` and observed `O_W`. For the GC to have deleted `(H,e_a)` it needed
`safe_epoch > e_a`. Either W is still counted ⇒ `O_W > e_a` ⇒ W saw the tombstone ⇒ would not reuse it
(contradiction); or the GC already dropped W ⇒ it waited past `lease_until + Δ_skew` ⇒ W's local self-fence
already forced it read-only ⇒ W did not commit (contradiction). No loss. ∎

**Bonus — liveness backstop for free.** A stuck-but-alive writer cannot stall reclamation forever: it either
keeps refreshing (so it is not actually behind) or it cannot refresh, goes read-only, and its lease expires and
is dropped. So `safe_epoch` can always advance. One mechanism gives both safety and the liveness bound that
§8.5 previously listed as open.

**The cost — it is now a TIME lease, so S3 safety assumes bounded clock skew** (writer clock vs GC clock,
bounded by `Δ_skew`). That is the price of dropping the skew-free commit-time re-read; it is the standard
lease trade (Chubby/GFS) and is acceptable if `Δ_skew` is sized conservatively and the lease is renewed well
within `T_lease`.

- **Keeper (clean — no skew assumption):** make the lease the **ephemeral session** itself; Keeper's own clock
  is the single arbiter of liveness, so there is no cross-clock skew between writer and GC. Session loss ⇒
  the writer's ops fail ⇒ it goes read-only. This is why Keeper is the **recommended** coordinator and
  S3-only is the documented-weaker (clock-skew-dependent) mode.

Get this wrong and INV-NO-LOSS breaks; get it right and the EBR proof holds. **This (and `Δ_skew` sizing on
S3) is the thing to model-check.**

## 6. Failure analysis {#failures}

| Fault | Outcome |
|---|---|
| Writer crash between `+` and upload / upload and ref | no ref ⇒ not live; the blob ages out once the writer's lease expires (+ Δ) and the fold shows count-0 → condemned → reclaimed. Over-count only. Safe. |
| Writer crash after ref, before `-` | over-count (blob kept); reconciled later. Safe. |
| Writer **paused** (alive lease) across a rotation | `safe_epoch` pinned at `O_W`; reclamation of `> O_W` stalls. Liveness only. |
| Writer **wrongly declared dead** (lease expired) then resumes | §5 fencing: Keeper op-fail / S3 self-check forces re-sync before any reuse-commit. Safe iff §5 holds. |
| GC leader crash mid-rotate / mid-reclaim | idempotent: tombstones are `create-if-absent`; `epoch/current` PUT is monotone; deletes are fence-gated. Successor takes a higher fence. Safe. |
| Two GC leaders (split brain) | only the highest fence's writes/deletes land (`stillLeader(F)` on every mutation). The lower fence's deletes are rejected. Safe. |
| Duplicated/retried `+` / DELETE / tombstone create | `event_id` dedups `+`; DELETE idempotent; tombstone `create-if-absent` idempotent. Safe. |
| Lost snapshot / torn write | rebuild via the full reconcile scan (the *only* use of the full scan). Safe (over-protective). |
| `active/<H>` stale or lost | hint only; resolve by `LIST blobs/<H>/`; reads use any present epoch. No safety impact. |

## 7. What this buys vs. the current design {#delta}

- **Sessions (per-blob `sessions/<id>` + lease + delta_epochs + sticky-fail + reapFoldedSessions) → one
  per-writer epoch lease.** The review's irreducible "pin" becomes a single cheap lease per writer.
- **Per-blob generations → the global epoch.** One monotone counter does ABA-safety and the grace clock.
- **Wall-clock grace (unsafe for unbounded pause) → epoch quiescence (safe for any pause).** Fixes V1/V3/V4.
- **The log/snapshot count becomes an honestly *sloppy candidate filter*** (over-count safe); the *authority*
  is the tombstone + the EBR quiescence, not a perfect count. Resolves the Milovidov "why count if you scan"
  tension: the count is the O(candidates) filter; there is no full scan on the hot path.
- **Scan-B full reachability → reconcile/rebuild only.**
- Tombstones survive (as the per-object condemn marker — a point `HEAD`, not a list), `active` survives as a
  pure hint, `gc/sealed`-as-index and the resurrection cap are gone.

## 8. Open issues to verify before this is a spec {#open}

1. **§5 time-bounded lease on S3** — the sole safety hinge; model-check the writer local self-fence
   (`now < lease_until` before the ref-publish) + GC lease-out-wait `Δ_skew`, and **size `Δ_skew` vs realistic
   clock drift**. Keeper (ephemeral session) is skew-free by construction → **recommended** coordinator;
   S3-only is the documented clock-skew-dependent mode.
2. **Candidate signal at scale.** Use the per-object `<e>.tombstone` point-`HEAD` as the authoritative reuse
   check (O(1) per dedup), *not* a download of a monolithic `candidates/<e>` file (could be huge at billions).
   Keep `candidates/<e>` only as an optional reconcile roll-up.
3. **Epoch cadence vs writer refresh.** Writers must refresh `O_W` (and thus advance `safe_epoch`) faster than
   the GC rotates, or reclamation always lags one live writer. With a ~10-min epoch and per-commit refresh,
   fine; define the heartbeat for idle writers.
4. **`active/<H>` update races** under concurrent resurrection — all concurrent resurrectors agree on `E_cur`,
   so it is last-writer-wins to the same value; confirm no cross-epoch reorder can point `active` *backwards*
   below a present generation (reads tolerate it via LIST, but worth stating).
5. **Liveness backstop** — RESOLVED by the §5 time limit: a writer that falls too far behind cannot refresh
   its lease, goes read-only, and is dropped, so `safe_epoch` always advances. (Only remaining knob: choose
   `T_lease` / max-lag so a busy-but-healthy writer is never wrongly forced read-only.)

## 9. Verdict {#verdict}

Conceptually **sound and much simpler** than both the current design and my earlier "page-sized" attempt,
*because it is EBR* — a proven shape — and because it replaces the two things the review proved unsafe
(per-blob folded pin; wall-clock grace) with a single per-writer epoch lease and epoch quiescence. Its entire
safety reduces to **one** hinge (§5 writer-lease fencing), which is trivial on Keeper and needs a TTL margin +
writer self-check on S3. Recommend: model-check §5, adopt per-object tombstone point-lookups (§8.2), then this
becomes the from-scratch spec.
