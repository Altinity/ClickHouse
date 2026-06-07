---
description: 'Adversarial distributed-systems + object-storage correctness review of the improved content-addressed (CA) MergeTree GC design plan, which replaces the wall-clock-grace / "+-is-the-pin" approach with Epoch-Based Reclamation (per-writer epoch lease + epoch quiescence). Re-derives the Lamport model, attacks every new mechanism (EBR grace, time-bounded lease + Δ_skew, early-+ hazard pin, ≤1-epoch lag, epoch rotation vs in-flight +, new-writer registration, active reorder), runs a failure-injection catalog, re-checks the prior V1/V3/V4 schedules, and delivers a verdict with ordered must-fix gaps.'
sidebar_label: 'CA EBR GC plan — correctness review'
sidebar_position: 22
slug: /superpowers/reports/ca-gc-ebr-design-plan-review
title: 'Adversarial Correctness Review — CA EBR GC Design Plan'
doc_type: 'reference'
---

# Adversarial Correctness Review — CA EBR GC Design Plan {#review}

Panel: (1) a distributed-systems correctness specialist (Lamport happens-before, linearizability, TLA+-style
invariant reasoning, epoch-based reclamation / QSBR / Crossbeam, failure analysis); (2) an object-storage
(S3 / MinIO) + ZooKeeper/Keeper systems expert. Subject under test:
`docs/superpowers/reports/2026-06-07-ca-gc-ebr-design-plan.md` (the "improved" plan), consolidating
`2026-06-07-ca-gc-epoch-reclamation-design.md`. This review answers a single question: **does the EBR rework
actually close the V1/V3/V4 data-loss schedules the prior review found, and does it open new ones?**

This review is deliberately adversarial. The job is to find the schedules that lose data, strand a ref, or leak
forever — not to bless it.

**Top-line spoiler.** The EBR *core idea* is correct and is a genuine, material improvement: a paused-but-alive
writer now **holds reclamation back** (`safe_epoch` pins low) instead of losing data — V1/V3/V4 in their
original "the `+` is the pin / wall-clock grace" form are **genuinely closed**. **But the plan is not correct
as written.** Three independent must-fix gaps remain, two of them **data-loss VIOLATIONS**: (A) the
**condemn-before-advance ordering** (R1 vs R2) is the load-bearing happens-before edge of the entire proof and
the doc leaves it **unsequenced** — under the natural "fold the whole open epoch then advance" reading there is
a concrete loss schedule (**V-EBR-1**); (B) the **early-`+` hazard pin** is asserted to make a long op safe but
the plan never makes the GC *read* the `+` as a pin at delete time — the fold is epoch-window-bounded, so a
`+` that lands in an already-folded epoch is invisible and the decide→`+` window is a real loss window
(**V-EBR-2**); (C) the **S3 time-lease `Δ_skew`** closes the writer-resume race only under an assumption the
doc states but does not bound against **GC-clock fast-drift + the lease-renewal-vs-expiry gap** — there is a
skew schedule that deletes a blob a still-self-fenced writer then reuses (**V-EBR-3**, S3-only). Details,
timelines, and minimal fixes below.

---

## 1. Model (Lamport) {#model}

### 1.1 Durable state (the only ground truth) {#state}

Per the plan §4 (object keys wrapped as code):

- `epoch/current` — monotone `E_cur`; advanced ONLY by the fenced leader, once per round.
- `blobs/<H>/<e>` — immutable content bytes of `H`, created in epoch `e` (generation == epoch).
- `blobs/<H>/<e>.tombstone` — per-object condemn marker; records condemnation epoch `e_a`; `create-if-absent`.
- `active/<H>` — reuse hint (plain PUT, last-writer-wins; never trusted; LIST-derivable).
- `store/.../refs/<part>` — live ref (commit point / GC root).
- `parts/<part_id>/<e>` — manifest; identical lifecycle to a blob.
- `log/<e>/<shard>/<event_id>` — `+`/`-` deltas appended in epoch `e` (sloppy candidate filter; `event_id`-deduped).
- `snap/<e>/<shard>` — folded counts (reverse index).
- `writers/<W>` — writer lease `{ observed_epoch O_W, lease_until }` (Keeper EPHEMERAL session, or S3 object+TTL).
- `gc.lock` — fenced leader lock `{ server_id, fence_token, lease_deadline }`.

### 1.2 Actors {#actors}

- **Writers** `W1..Wn` — concurrent, uncoordinated. Each holds a lease publishing an observed epoch `O_W` and a
  time-bounded `lease_until`. Each can pause unboundedly between any two ops (VM freeze, GC pause). A writer
  commits a part referencing a set of `(H,e)`.
- **GC leaders** `L` — 1+ contending; exactly one *should* act, enforced by the `gc.lock` fence. A leader
  executes R1 FOLD → R2 CONDEMN → R3 ADVANCE → R4 QUIESCE → R5 RECLAIM (the plan's §5.2 labels; note the plan's
  prose orders CONDEMN at R2 and ADVANCE at R3 — I keep its numbering).
- **Readers** `R` — `GET active/<H>` → epoch → `GET blobs/<H>/<e>`; on `404`, `LIST blobs/<H>/` and read any
  present epoch.

### 1.3 Atomic events (each one primitive op; each may be retried/duplicated/delayed/reordered) {#events}

```
Writer W:
  W.lease0 : read epoch/current -> O_W ; ensure writers/<W> live, set lease_until        (step 0)
  W.list   : read active/<H> or LIST blobs/<H>/                                          (step 1a)
  W.head   : HEAD blobs/<H>/<e>.tombstone                                                (step 1c)
  W.up     : createIfAbsent blobs/<H>/<e>  (e = O_W on first-create, E_cur on resurrect) (step 1b/1c)
  W.plus   : createIfAbsent log/<O_W>/<shard>/<eid(+,H,e)>   (the "early +" hazard pin)  (step 2)
  W.fence  : LOCAL check now < lease_until                                               (step 3 pre-check)
  W.ref    : createIfAbsent store/.../refs/<part>  (commit point, LAST)                  (step 3)
  W.unref  : DELETE store/.../refs/<part>          (drop: ref FIRST)                      (step 4)
  W.minus  : createIfAbsent log/<O_W'>/<shard>/<eid(-,H,e)>   (drop: - AFTER unref)       (step 4)

GC leader L (fence F):
  R1.fold  : LIST log/<=E_cur> + read snap -> fold counts
  R2.cond  : createIfAbsent blobs/<H>/<e_a>.tombstone   (e_a := E_cur)
  R3.adv   : PUT epoch/current := E_cur+1               (fenced; rotation barrier)
  R4.quad  : safe_epoch := min(O_W) over not-definitely-expired leases (else E_cur)
  R5.del   : if E_cur>=e_a+2 AND safe_epoch>e_a AND unreferenced AND stillLeader(F):
                 DELETE blobs/<H>/<e_a>; drop tombstone; refresh active
  L.fence? : re-read gc.lock, confirm F  (gates R2/R3/R5)
```

### 1.4 Happens-before (→) edges the primitives actually give us {#hb}

- **HB-RAW.** If a `createIfAbsent K` / `PUT K` completes at real time `t`, any `GET/HEAD K` or `LIST` of the
  enclosing prefix that *starts after `t`* observes `K`. The edge exists **only when the reader provably starts
  after the writer's op completed** — an in-flight, not-yet-acked PUT is invisible and may even land after a
  later reader's LIST.
- **HB-CIA.** All `createIfAbsent K` for the same key `K` are totally ordered; exactly one wins.
- **HB-PROG.** A single actor's ops are ordered as issued, but **not as durably visible to others** unless each
  completed. A paused actor contributes no edges during the pause.
- **No edge from DELETE** toward anything (unconditional/idempotent).
- **LIST strongly consistent but the fold's LIST is a *prefix* read**: it sees exactly the keys durable when
  each page is served. A key created during pagination may be missed — relevant to `R1.fold` over `log/`.
- **Clocks are not synchronized.** Wall-clock comparisons across writer and GC are valid only under a bounded
  skew assumption `Δ_skew`. Keeper's session clock is a single arbiter (no cross-clock skew).

### 1.5 Safety invariants (precise) {#invariants}

- **INV-NO-LOSS.** No `blobs/<H>/<e>` is `DELETE`d while reachable from a live ref, or while any correct future
  fold-or-pin-read must still count a reference to it.
- **INV-NO-DANGLE.** Every published `refs/<part>` resolves to present bytes (reads tolerate stale `active` via
  LIST).
- **INV-NO-ABA.** `DELETE blobs/<H>/<e_a>` never destroys bytes a concurrent writer is creating under the same
  key `(H,e_a)`.
- **INV-OVER-COUNT-ONLY.** Every failure mode biases to over-count (leak, reconciled later), never under-count
  (loss).

The whole EBR thesis is: replace the unsafe wall-clock grace with the *epoch-quiescence* gate
`safe_epoch > e_a`, which is supposed to make `INV-NO-LOSS` hold for **any** pause length.

---

## 2. The EBR core proof — is the load-bearing happens-before edge actually established? {#core}

The plan's §4 proof (and §0 "the one idea") rests on this chain:

> A writer with `O_W > e_a` executed step-0 *after* the rotation that closed `e_a`, so its read of
> tombstones (step 1c) **happens-after** the condemnation of `(H,e_a)` — therefore it observes the tombstone
> and routes to a fresh epoch.

Unpack the HB edges required:

```
(a)  R2.cond(H,e_a)  →  R3.adv(E_cur := e_a+1)          [leader program order, must be durable-before]
(b)  R3.adv completes  →  W.lease0 reads O_W ≥ e_a+1     [HB-RAW: W's read starts after the PUT]
(c)  W.lease0  →  W.head(blobs/<H>/<e_a>.tombstone)      [W program order, AND W.head starts after R2.cond]
─────────────────────────────────────────────────────────────────
∴   W.head sees the tombstone  ⇒  W routes to E_cur, never emits +(H,e_a)
```

Edge (b) and (c) are sound: a writer reading `O_W = e_a+1` necessarily issued that read after `R3.adv`
completed, and `R3.adv` is issued (program order) after `R2.cond` *returns* — so its tombstone is durable, and
W's later `W.head` (which starts strictly after `W.lease0`) sees it by HB-RAW. **Good — but only if edge (a)
holds for *every* object condemned in `e_a`.** This is exactly the doc's own open item §13.3, and it is the
crack the EBR proof falls through.

### 2.1 V-EBR-1 — condemn/advance is unsequenced; the fold can race a live `+` in the closing epoch — **VIOLATION** {#v-ebr-1}

The proof needs: **the condemnation set for `e_a` is *complete and durable* before `R3.adv`,** i.e. before any
writer can observe `O_W = e_a+1`. The plan does not state that R1's fold is taken over a *closed* `log/<e_a>`.
Writers append `+` into `log/<O_W>` and a writer with `O_W = e_a` is *still appending into `log/<e_a>`* while
the leader folds it. There is no window-close barrier on `log/<e_a>` before `R1.fold` (the prior review's F15;
the companion's dropped `gc/current_epoch`). So:

```
   WRITER Wa (O_W = e_a, dedup-reuses an existing H @ e_a)     LEADER L (fence F), round at E_cur=e_a
   ──────────────────────────────────────────────────         ───────────────────────────────────────
   W.lease0: O_W = e_a                                          (a healthy round begins)
   W.list:   blobs/H/  -> H present @ e_a
   W.head:   blobs/H/<e_a>.tombstone -> ABSENT  (none yet)
                                                                R1.fold: LIST log/<=e_a>  -- Wa's + NOT yet durable
                                                                         count(H,e_a) == 0  (old refs all dropped)
   W.plus:   createIfAbsent log/<e_a>/.../+(H,e_a)   (lands AFTER the fold's LIST page passed)
                                                                R2.cond: createIfAbsent blobs/H/<e_a>.tombstone (wins)
   W.fence:  now < lease_until  (Wa still healthy)              R3.adv:  E_cur := e_a+1
   W.ref:    ref -> part naming (H,e_a)                         R4.quad: suppose Wa's lease lapses here OR Wa
                                                                         already advanced O_W on a refresh ...
                                                                R5.del:  E_cur(=e_a+1) >= e_a+2? NO -> wait one round
   ── next round, E_cur = e_a+1 ──
                                                                R1.fold: LIST log/<=e_a+1>  -- if Wa's + IS now durable
                                                                         it is SEEN -> count(H,e_a) == 1 -> spared. OK.
                                                                         BUT see the killer ordering below.
```

The `e+2` limbo delays the delete one extra round, which *usually* gives Wa's `+` time to become durable and be
re-folded — that is the limbo rule doing useful work. **But the limbo does not save the case where Wa is
dropped from the live set before its `+` is durable-and-folded, because the gate is a *conjunction* and the
fold is the weak link.** Concretely, the loss schedule is: Wa's `+` is *still not durable* (in-flight, paused
TCP, MinIO write buffer) across both the `e_a` fold and the `e_a+1` re-fold, while Wa itself has *already
committed its ref* and *then advanced its `O_W`* on a normal background refresh (so `safe_epoch` climbs past
`e_a`):

```
   WRITER Wa                                                    LEADER L
   ────────                                                     ────────
   W.head: tombstone ABSENT (e_a)                               R1.fold(e_a):  count(H,e_a)==0  (+ not durable)
   W.plus: ISSUE +(H,e_a)  -- ack lost / retried / buffered     R2.cond:       tombstone(H,e_a)
   W.ref:  ref -> (H,e_a)   (COMMITTED, durable)                R3.adv:        E_cur=e_a+1
   ── Wa background refresh: O_W := e_a+1, lease extended ──     R4.quad:       safe_epoch = min O_W; Wa now e_a+1
                                                                                ⇒ safe_epoch > e_a  ✓ (Wa "passed")
   (Wa's +(H,e_a) STILL not durable — the retry is in flight)   R1.fold(e_a+1): LIST log/<=e_a+1>; + still not there
                                                                                count(H,e_a) STILL 0
                                                                R5.del: E_cur>=e_a+2 ✓, safe_epoch>e_a ✓,
                                                                        unreferenced(fold) ✓, stillLeader ✓
                                                                        DELETE blobs/H/<e_a>   ←── LOSS
   (Wa's +(H,e_a) finally lands — too late; the bytes are gone, the ref Wa published dangles)
```

The EBR proof's case split ("either `O_W ≤ e_a` so safe_epoch≤e_a, or `O_W>e_a` so W saw the tombstone") has a
**third, unhandled case: a writer whose `O_W` has advanced past `e_a` but whose `+(H,e_a)` is not yet durable
and whose tombstone read happened *before* condemnation.** The proof assumes "`O_W > e_a` ⇒ W saw the
tombstone," but that is only true for reads W issues *after* advancing. The `+`/ref W already committed *at
`O_W = e_a`* is not retroactively protected by W later advancing `O_W`. The advance of `O_W` is precisely what
*removes* W's protection (`safe_epoch` climbs), and nothing guarantees W's `e_a`-era `+` is durable-and-folded
before that happens.

**Root cause.** Two unsequenced orderings the doc leaves open:
1. **No window-close barrier**: `R1.fold` reads `log/<e_a>` while writers still append to it. A `+` can land
   after the fold's LIST and never be counted at `e_a`.
2. **No "drain" coupling `O_W` advance to `+`-durability**: the doc's §6 "rule of thumb" says `O_W` may advance
   past `e` only once every decided dependency is pinned — **but a `+` being *issued* is not a `+` being
   *durable and folded*.** Allowing `O_W` to advance on `+`-issued (not `+`-durable-and-observed) is the bug.

**Minimal fix (V-EBR-1).** Restore a fenced **window-close barrier** so the leader folds only a *closed*
`log/<e_a>`: a writer with lease epoch `e_a` may only append to `log/<e_a>` while `e_a` is open; the close must
*happens-before* `R1.fold`, and appends after close are rejected/redirected to `log/<E_cur>`. AND make the
`O_W` advance *barrier-coupled*: a writer may not refresh `O_W` past `e_a` until it has confirmed (RAW read)
that every `+` it appended at `e_a` is durable — i.e. read-back its own `+` (or use a write that returns
durability) before advancing. With both, edge (a) holds for every object and the case split becomes exhaustive.
On Keeper the window-close is a linearizable counter bump + a "no appends below current" guard; on S3 it is a
fenced PUT of an `epoch/closed` marker that writers honor. **This is the single most important gap: the EBR
proof is only valid against a closed-epoch fold, and the plan does not specify one.**

### 2.2 Is `E_cur ≥ e_a+2` (Crossbeam limbo) necessary / sufficient / redundant given `safe_epoch > e_a`? {#limbo}

Verdict: **`E_cur ≥ e_a+2` is neither sufficient alone nor strictly necessary for *safety*; it is a *liveness /
bounded-garbage* discipline that also happens to *mask* (not fix) V-EBR-1.**

- **Not sufficient alone**: with no live writers (`safe_epoch := E_cur`), `E_cur ≥ e_a+2` permits deletes; but
  safety there comes from `safe_epoch = E_cur > e_a`, i.e. from quiescence, not from the `+2`. The `+2` without
  quiescence would delete under a live low-`O_W` writer.
- **Not necessary for safety**: if `safe_epoch > e_a` truly held (every live writer observed-past `e_a`, with a
  closed-epoch fold per the V-EBR-1 fix), then deleting at `E_cur = e_a+1` would already be safe. Crossbeam
  uses `+2` because its "epoch" advances by *one* between pinned critical sections and a thread can be at
  `global-1`; here the analogue is the **≤1-epoch-lag** writer (§2.4), and `+2` is what tolerates a writer
  sitting at `E_cur-1`. So `+2` is the correct bound *given* the ≤1-lag rule — it ensures even a maximally-lagged
  live writer has observed `e_a`. **In that role it is necessary** (for the lag rule) and is doing real work.
- **It masks V-EBR-1**: the extra round between condemn and delete gives an in-flight `+` more time to become
  durable and be re-folded. This is why V-EBR-1 needs the *adversarial* "`+` never becomes durable until after
  the e+1 re-fold AND `O_W` advanced anyway" schedule — the `+2` defeats the *easy* version. It does not defeat
  the hard version, because the hard version's defect is the missing *causal* coupling, not a missing *delay*.
  **Time/round delays mask but never fix a causality gap — exactly the lesson the prior review hammered.**

So keep `+2` (it bounds garbage to a 2-epoch window and supports the ≤1-lag rule), but **do not treat it as
contributing to `INV-NO-LOSS`** — the no-loss authority must be `safe_epoch > e_a` over a *closed-epoch* fold.

---

## 3. The early-`+` (hazard) pin — does it pin? {#hazard}

The plan's §6.2 / §5.1-step-2 claim:

> A dependency is `+`-logged at decision time, so it is pinned for the whole long operation by the `+` (fold
> sees it → never condemned), and `O_W` may advance freely. Thus a long op does not hold `safe_epoch` low
> pool-wide — only the sub-second decide→`+` window does.

### 3.1 V-EBR-2 — the `+` is read only through an epoch-window-bounded fold, so it does not pin across epochs — **VIOLATION** {#v-ebr-2}

The hazard-pointer claim requires the GC to **read the `+` as a per-object protection at delete time**. But the
plan's only consumer of `+` is `R1.fold` over `log/<=E_cur>`, folded *by epoch*. A `+` for `(H,e)` is appended
into `log/<O_W>` — the epoch the writer observed *when it decided* — not into a per-object hazard slot the GC
re-reads before each delete. So the `+` "pins" `(H,e)` **only within the epoch fold that includes
`log/<O_W>`** and only while that count is carried forward in `snap`. Two failures:

**(i) The hazard does not span the decide→`+`-durable window for a long op.** The plan admits "only the
sub-second decide→`+` window" is exposed, but that window is exactly where a 100 GB op is dangerous: the
dependency is *decided* (writer will reuse `(H,e)`), the writer has NOT yet appended `+` (or it is in flight),
and the writer then proceeds into a multi-hour upload of a *different* blob in the same part. During that
window, if `(H,e)` was already count-0 from other drops, the leader can fold (count 0), condemn, and — across
the hours of the upload — advance two epochs and delete `(H,e)`. The writer's `O_W` is pinned low *only if it
does not refresh*; but the plan explicitly wants `O_W` to "advance freely" during the long op (that is the
whole point of the hazard). If `O_W` advances past `e` while the `+(H,e)` is still not durable-and-folded, the
QSBR gate `safe_epoch > e` opens and `(H,e)` is deleted — **same root cause as V-EBR-1, surfaced by the long
op.** The hazard pin is supposed to substitute for the lost low-`O_W` protection, but a hazard pin you have not
yet durably written and the GC does not point-read is not a pin.

```
   WRITER W (long 100 GB op; decides to reuse dep (H,e))        LEADER L
   ─────────────────────────────────────────────────────       ────────
   W.head(H,e): tombstone ABSENT  (decide to reuse (H,e))
   W.plus(H,e): ISSUE +  -- not yet durable (or appended to log/<e>)
   ── W advances O_W := e+1 on a refresh (plan says "freely") ──
   ── W begins the 100 GB CompleteMultipartUpload of the NEW blob B (hours) ──
                                                                R1.fold(<=e):  +(H,e) not durable -> count(H,e)==0
                                                                R2.cond(H,e);  R3.adv; ... rounds pass; E_cur >= e+2
                                                                R4.quad: safe_epoch = min O_W; W is e+1 -> > e
                                                                R5.del: DELETE blobs/H/<e>   ←── LOSS of the dep
   ── W finishes upload, publishes ref naming (H,e) AND B ──    (ref -> deleted (H,e) dangles)
```

**(ii) Even a *durable* `+` only protects within its epoch's snap carry-forward — the doc never specifies that
`snap` carries a still-`+`'d count forward indefinitely.** If the fold/snap compaction drops `(H,e)` once its
*folded* count returns to 0 (e.g. a later `-` from a different, aborted writer, or a fold that does not carry
forward a count it believes settled), the hazard evaporates. The plan does not state the snap retention rule
for `+`-pinned-but-otherwise-0 objects.

**Minimal fix (V-EBR-2).** Two options, pick one and specify it:
- **(a) Make the `+` a real hazard pointer the GC point-reads.** Before `R5.del(H,e_a)`, the leader must do a
  fresh edge-creating read for any live `+(H,e_a)` — e.g. `LIST log/.../+(H,e_a)` across *all* open epochs (not
  a window-bounded fold) at delete time, the way the prior review's "directly-listed pin read fresh at delete"
  worked. This restores the hazard semantics but reintroduces an O(open-log) read at delete time.
- **(b) Forbid `O_W` advance past `e` until every decided-at-`≤e` dependency's `+` is durable-and-folded** (the
  V-EBR-1 fix, item 2, applied to the long-op path). Then the long op *does* hold `safe_epoch` at `e` until its
  deps are durably pinned — costing the pool-wide liveness the hazard was meant to avoid, **but only for the
  decide→`+`-durable window, which is sub-second if the `+` is flushed before the long upload starts.** This is
  the honest version of the doc's "rule of thumb," and it is safe: order the `+`-flush *before* the long upload,
  hold `O_W` until the `+` is durable, then advance. The doc's claim that this "is a non-constraint" is wrong —
  it is *the* constraint; it is merely cheap if implemented as "flush `+` then advance."

**Verdict on the hybrid:** the epoch+hazard hybrid is sound *only if* the hazard (`+`) is either (a) read as a
true per-object protection at delete time, or (b) coupled to the `O_W`-advance barrier. As written it is
neither — it is an epoch fold relabeled "hazard pointer," and a relabel is not a mechanism.

---

## 4. The time-bounded lease + self-fence + `Δ_skew` wait, under clock skew {#lease}

The plan's §7 / §5-step-0 safety hinge (S3 mode): writer self-fences `now < lease_until` (local clock) before
the ref-publish; GC treats a lease "definitely expired" only at `GC_now > lease_until + Δ_skew`.

### 4.1 V-EBR-3 — fast GC clock + renewal-vs-expiry gap deletes under a self-fenced writer — **VIOLATION (S3-only)** {#v-ebr-3}

The intended safety argument (epoch-reclamation doc §5): "either W is still counted ⇒ saw tombstone; or GC
dropped W ⇒ waited past `lease_until + Δ_skew` ⇒ W's self-fence already forced it read-only." This is correct
**iff** `Δ_skew` truly bounds the *signed* clock difference `GC_clock − W_clock` from below by the right
quantity. Attack the bound. Let `lease_until` be a wall-clock instant W computed on **W's clock** as
`t_observe + T_lease`. The GC compares its own `GC_now` to `lease_until + Δ_skew`. The writer's self-fence
compares **W's** `now` to the same `lease_until`. For the safety argument we need:

> If GC concludes "expired" (`GC_now > lease_until + Δ_skew`) then W's self-fence has *already* fired
> (`W_now ≥ lease_until`) at every real instant at-or-after the one where GC concluded expired.

That requires `W_now ≥ GC_now − Δ_skew`, i.e. **W's clock is at most `Δ_skew` *behind* GC's clock.** If the GC
clock runs *fast* relative to W's by more than `Δ_skew` (or W's clock runs slow), then at the real instant GC
sees `GC_now > lease_until + Δ_skew`, W's own clock still reads `< lease_until`, so W's self-fence has **not**
fired and W can still commit:

```
   real time ─────────────────────────────────────────────────────────────────►
   W.lease0:  O_W=e_a, lease_until = t0 + T_lease  (computed on W's SLOW clock)
   W decides to reuse (H,e_a); pauses just before W.ref (GC pause / TCP stall)
                                                    GC clock is FAST by δ > Δ_skew
   GC_now (fast) > lease_until + Δ_skew  -> GC drops W from live set
   R4.quad: safe_epoch advances past e_a    R5.del: DELETE blobs/H/<e_a>
   W resumes;  W_now (slow) still < lease_until  -> self-fence PASSES
   W.ref: commit ref -> (H,e_a)   ←── reuse of a just-deleted blob  -> LOSS
```

The doc says "size `Δ_skew` conservatively (minutes)," which is *necessary* but the doc never states the
**direction** of the bound (it must bound `GC_clock − W_clock` from above, not just `|skew|` loosely) nor that
NTP step/slew, a GC host with a fast TSC, or a writer in a throttled VM (clock appears to run slow) can exceed
any fixed minutes-bound. **More dangerous: the renewal-vs-expiry gap.** A writer that *refreshes* its lease
extends `lease_until` by writing `writers/<W>` — but on S3 that PUT is not atomic with the GC's read. The GC
can read the *old* `lease_until`, conclude expired, and delete, while the writer believes it renewed in time:

```
   W (renewing)                                  GC
   ────────────                                  ──
   compute new lease_until = t1 + T_lease
   PUT writers/<W> {lease_until = t1+T_lease}    (in flight, not yet durable)
                                                 GET writers/<W> -> OLD {lease_until = t0+T_lease}
                                                 GC_now > t0+T_lease+Δ_skew -> "expired" -> drop -> DELETE
   PUT completes                                 (too late)
   W self-fence: W_now < t1+T_lease -> PASS -> W.ref to deleted blob -> LOSS
```

**Minimal fix (V-EBR-3).** (1) State the bound directionally and require **the writer to renew well inside
`T_lease/2 − Δ_skew`** *and* to set its self-fence deadline to `lease_until − Δ_skew` (not `lease_until`), so
the writer self-quiesces *before* the GC could possibly consider it expired regardless of skew direction; the
GC drops only at `lease_until + Δ_skew`; the resulting *gap of `2·Δ_skew`* between "writer stops committing"
and "GC starts deleting" is the safety margin, and it is symmetric. (2) For the renewal-vs-expiry gap, the
writer must treat *its own* effective lease as `min(local deadline, last-confirmed-durable renewal + T_lease)`
— i.e. it may only rely on a renewal the PUT of which it has *read back* (RAW), never an in-flight one;
equivalently, self-fence on the *previous* confirmed `lease_until` until the new one is durable. (3) **Recommend
Keeper** — the ephemeral session makes the lease the single-arbiter liveness signal, no cross-clock comparison,
no renewal-gap (the session keepalive is the renewal and Keeper linearizes it). The doc already says this; the
review *confirms* S3-only is a genuinely weaker (skew-dependent) mode and the gap is a real loss, not just
"awkward."

### 4.2 Pause-at-commit, specifically {#pause-commit}

The self-fence is checked "immediately before" `W.ref`. A pause *between* the self-fence check and the `W.ref`
completing is unbounded and **not** covered by the local check (the check already passed). So the true exposure
is `[self-fence check] → [W.ref durable]`, and the GC must not delete `(H,e_a)` until *that* interval is
provably over. With the §4.1 fix (self-fence at `lease_until − Δ_skew`, GC drop at `lease_until + Δ_skew`), the
covered margin is `2·Δ_skew` of real time — the `W.ref` PUT must complete within `2·Δ_skew` of the self-fence
check, **or W must re-check the fence after a long stall before relying on the commit.** The doc's "no
end-of-commit network re-read needed" is therefore only safe if `W.ref` is guaranteed to complete within the
margin; for a writer that can pause arbitrarily *between local-check and ref-PUT-ack*, the doc is **unsafe as
written** — it needs either a bounded ref-PUT or a post-stall re-check. State the assumption explicitly.

---

## 5. ≤1-epoch-lag re-sync and read-only-on-stale {#lag}

The rule: a writer's local epoch may lag the global by at most 1; if `O_W < E_cur − 1`, re-sync before reuse.
This is the Crossbeam ≤1-lag rule and it is what makes `E_cur ≥ e_a+2` the right limbo bound (§2.2).

**Boundary case — writer exactly at the boundary as the leader advances.** Suppose `O_W = E_cur − 1` (maximally
lagged, still legal) and the leader is mid-`R3.adv`. The writer reads tombstones for a reuse decision. Because
`O_W = E_cur − 1 ≤ e_a` would only arise if `e_a = E_cur − 1` or higher; the limbo gate requires
`E_cur ≥ e_a + 2`, i.e. `e_a ≤ E_cur − 2 < O_W`. So a maximally-lagged-but-legal writer has `O_W = E_cur−1 >
e_a` for any *deletable* `e_a` — meaning it observed past `e_a` and (with the V-EBR-1 closed-epoch fix) saw the
tombstone. **So the lag bound is *exactly* tight: `+2` limbo + ≤1-lag ⇒ every live writer observed past every
deletable epoch.** This part is correct *given* the V-EBR-1 fix. **Without** the V-EBR-1 fix, "observed past
`e_a`" does not imply "saw the tombstone" (the fold raced the `+`), so the tightness is illusory.

**Re-sync soundness.** "Re-sync before reuse" must mean: re-read `epoch/current`, advance `O_W`, **and re-read
the tombstone/candidate state for the new `O_W` before any reuse decision.** The plan §5.1-step-0 says exactly
this ("re-read candidate sets up to the new `O_W`"). Sufficient — provided re-sync is *mandatory before the
reuse decision*, not merely before commit (a writer that decided reuse at stale `O_W` then re-syncs `O_W` has
already made an unprotected decision; it must re-decide). State: **the reuse decision (`W.head`) must be made
under a fresh, non-lagged `O_W`.**

---

## 6. Epoch rotation vs in-flight `+` (the doc's open item §13.3) {#rotation}

Covered as the root cause of V-EBR-1. Restating the decision the doc defers: *"a `+` appended to `log/<e>` as
the leader closes `e` — reappend-to-open-epoch, or fold a short overlap window."*

- **Reappend-to-open-epoch** is the correct, provable choice: the writer must learn `e` is closed (a fenced
  `epoch/closed=e` marker it reads, or a Keeper guard) and append to `log/<E_cur>` instead, *and* it must not
  advance `O_W` past `e` until the reappended `+` is durable. This makes the fold-of-closed-`log/<e>` complete.
- **Fold a short overlap window** (fold `log/<e>` again next round) is the *masking* approach — it is what the
  `+2` limbo already does, and §2.1 V-EBR-1 shows it does not close the hole when the `+` durability is delayed
  past the overlap. **Reject the overlap-window option as a safety mechanism.**

**Does any fold/condemn/reclaim path act on an epoch before all its `+`s are durable?** Yes — `R1.fold` does,
because there is no close barrier. That is the defect. With the barrier, condemn (R2) and reclaim (R5) act only
on closed epochs, and the proof's edge (a) holds.

---

## 7. New-writer registration (doc open item §13.4) {#new-writer}

Claim to verify: a freshly-registering writer reads `E_cur` and so *cannot* lower `safe_epoch` below an
already-reclaimable epoch, nor reuse a blob condemned in an epoch it never observed.

**Holds, with one stated condition.** A new writer `Wn` does `W.lease0: O_W := read(epoch/current) = E_cur`.
Since `E_cur ≥ e_a + 2 > e_a` for any reclaimable `e_a`, `Wn` has `O_W > e_a`, so it does **not** lower
`safe_epoch` below `e_a`. And `O_W = E_cur` means (with the closed-epoch fold) `Wn`'s tombstone reads
happens-after every condemnation through `E_cur − 1`, so it routes around them. **The one condition: lease
registration must be *publish-then-observe* in the right order** — `Wn` must make `writers/<Wn>` live with
`O_W` set *atomically/before* it does any `W.head` reuse decision, AND the GC's `R4.quad` must read the
registration. The race to rule out: `Wn` registers `writers/<Wn> {O_W=E_cur}` but the GC's `R4.quad` LIST of
`writers/` already passed `Wn`'s key (HB-RAW miss) → GC computes `safe_epoch` *without* `Wn` → but that only
*omits* a high `O_W=E_cur`, which cannot *lower* `min` below an existing live writer, and `Wn` itself has
`O_W=E_cur > e_a` so its omission is harmless for the epoch being reclaimed. The dangerous direction
(registering a *low* `O_W`) is impossible because a new writer always reads current `E_cur`, never an old epoch.
**Verdict: SAFE by construction**, provided a writer never registers with a stale cached `O_W` from a prior
incarnation (a re-registering writer after lease loss must re-read `epoch/current`, not reuse its old `O_W` —
this is the §5.1 fencing rule and must be mandatory).

---

## 8. `active/<H>` cross-epoch updates (doc open item §13.2) {#active}

`active/<H>` is a plain PUT, last-writer-wins, never trusted; reads fall back to `LIST blobs/<H>/`. Attack:
can a reorder point `active` *below* a present generation, or at a condemned/deleted one, unrecoverably?

- **Below a present generation.** Two resurrectors at different epochs PUT `active/<H>=e1` and `=e2` (e1<e2);
  reorder lands `=e1` last. A reader gets `e1`, `GET blobs/<H>/<e1>`. If `e1` still exists → valid bytes (reads
  don't care which generation, content is identical). If `e1` was deleted → `404` → reader LISTs and reads any
  present epoch. **Recoverable. SAFE.**
- **At a condemned/deleted epoch.** `active/<H>=e_a` where `e_a` is condemned-then-deleted. Reader `GET
  blobs/<H>/<e_a>` → `404` → LIST fallback → reads a present epoch. **Recoverable. SAFE** — *provided some
  present epoch exists*, which `INV-NO-LOSS` guarantees (the live ref's content is not fully deleted). The only
  way this breaks is if `INV-NO-LOSS` is already violated (V-EBR-1/2/3) — then `active` pointing at a hole is a
  symptom, not the cause.
- **One must-state caveat:** the reader's `404 → LIST` retry must be **mandatory and unbounded-generation-aware**
  (read *any* present epoch, not just `active±1`). The plan §5.3 says this. Also: a reader mid-`GET` of an
  epoch that is deleted *during* the GET gets a `404`/aborted read and must restart via LIST — state the
  in-flight-read retry rule (the prior review's F19). **SAFE, conditional on INV-NO-LOSS and the stated retry.**

---

## 9. Failure-injection catalog {#catalog}

Verdicts: **SAFE** / **VIOLATION** (loss or permanent dangle) / **LEAK** (liveness-only) / **NEEDS-FIX** (safe
only with a stated fix the plan omits).

| # | Injected fault | Trace (abbrev.) | Verdict | Minimal fix |
|---|---|---|---|---|
| E1 | `+` lands in `log/<e_a>` after `R1.fold`'s LIST page passed; writer commits ref @ `e_a`, then advances `O_W` past `e_a`; `+` durability delayed past the `e_a+1` re-fold | `safe_epoch>e_a` opens though a live ref names `(H,e_a)`; `R5.del` deletes it (V-EBR-1) | **VIOLATION** | Fenced window-close barrier on `log/<e_a>` before fold; couple `O_W`-advance to `+`-durable-and-folded |
| E2 | Long op decides reuse `(H,e)`, advances `O_W` "freely" before its `+(H,e)` is durable, uploads 100 GB for hours; `(H,e)` already count-0 | leader condemns+deletes `(H,e)` mid-upload; final ref dangles (V-EBR-2) | **VIOLATION** | `+` read as a true hazard at delete time, OR hold `O_W` until `+(H,e)` durable (flush-then-advance) |
| E3 | GC clock fast by δ>`Δ_skew`; writer pauses pre-`W.ref`; GC drops writer, deletes; writer resumes, self-fence (slow clock) passes, commits reuse (V-EBR-3) | reuse of a just-deleted blob | **VIOLATION (S3-only)** | Directional skew bound; self-fence at `lease_until−Δ_skew`, GC drop at `+Δ_skew`; Keeper has no skew |
| E4 | Lease-renewal PUT in flight; GC reads old `lease_until`, deletes; writer self-fences on the not-yet-durable renewal | reuse of deleted blob (V-EBR-3 variant) | **VIOLATION (S3-only)** | Writer relies only on read-back-durable renewals; self-fence on last confirmed lease |
| E5 | Pause between self-fence local check and `W.ref` ack, longer than `2·Δ_skew` | self-fence already passed; GC's margin elapsed; delete races the late ref | **NEEDS-FIX** | Bound the ref-PUT, or re-check fence after a long stall before trusting the commit |
| E6 | Writer paused with a *live* (still-renewing-by-keepalive on Keeper) lease across a rotation | `safe_epoch` pins at `O_W`; reclamation of `>O_W` stalls | **LEAK / LIVENESS** | None needed; this is EBR working as designed (pause→liveness, not loss) |
| E7 | Writer crash between `+`/upload and `W.ref` | no ref; blob ages to count-0; condemned+reclaimed after lease lapse; over-count meanwhile | **SAFE** | — (Δ-after-lease before reclaim; incomplete multipart never a visible blob) |
| E8 | Writer crash after `W.ref`, before `W.minus` | over-count (blob kept), reconciled later | **LEAK** | Reconcile mandatory |
| E9 | 100 GB multipart upload crashes mid-flight | object invisible until CompleteMultipartUpload; never a blob; S3 lifecycle abort-incomplete reclaims parts | **SAFE** | — (lifecycle rule is orthogonal; state it as required infra) |
| E10 | GC leader crash between `R2.cond` and `R5.del` | tombstone durable, blob present; successor re-derives candidate from the fold (count still 0) and reclaims; if a `+` arrived meanwhile, fold>0 → spared | **SAFE** *iff* successor re-folds before deleting | Successor must re-FOLD+re-QUIESCE under its own fence before any `R5.del`; do not trust a prior round's candidate list blindly |
| E11 | GC leader crash between `R1.fold` and `R2.cond` | snap maybe written; condemn maybe not; idempotent `create-if-absent` tombstone + monotone epoch PUT → successor re-folds cleanly | **SAFE** | State fold+snap publish idempotent/atomic |
| E12 | Split-brain: L1(F=7) pauses past lease; L2(F=8) steals, legitimately spares `(H,e_a)` (new `+` arrived); L1 resumes and `R5.del` | if L1 re-reads fence and sees F=8 → aborts (SAFE); if L1's fence-read races L2's not-yet-durable `gc.lock` PUT → L1 deletes under stale token | **VIOLATION (S3-only)** | `gc.lock` read-after-write-strong; freshly-stealing leader must out-wait the prior lease+`Δ_skew` before its first mutation; every `R5.del` re-reads fence. Keeper: SAFE |
| E13 | Duplicated/retried `+` (same `event_id`) | `create-if-absent` idempotent; fold dedups by `event_id` | **SAFE** | Keep `event_id` dedup (plan mentions it) |
| E14 | Duplicated/reordered DELETE replay of `blobs/<H>/<e_a>` | DELETE idempotent; second a no-op; generation-keyed so a recreated `(H,E_cur)` is a different key | **SAFE** | Relies on tombstone persisting so no same-`e` recreate; if tombstone GC'd and `e` re-used as `E_cur` later — see E15 |
| E15 | Epoch counter wraps / is reset, re-using an `e` value that had a deleted blob (ABA on the epoch namespace) | a new `blobs/<H>/<e>` at a re-used `e` collides conceptually with a replayed `DELETE e` | **NEEDS-FIX** | `epoch/current` must be strictly monotone and never reset/wrap within a pool's lifetime (64-bit, never reused); state it |
| E16 | Torn/partial `snap/<e>/<shard>` write | a folder reading a partial snap under-counts → premature condemn | **VIOLATION** unless atomic | Single-object atomic publish (temp key then `create-if-absent` final), version+checksum; readers read only completed epochs |
| E17 | LIST pagination misses a just-created `log/.../+` during `R1.fold` | same effect as E1 (under-count → premature condemn) | **VIOLATION** | Window-close barrier (E1 fix) makes the folded set stable |
| E18 | Reconcile (full scan) races a live writer mid-upload (blob exists, no ref, `+` not yet durable) | scan reclaims an in-flight blob | **VIOLATION** unless reconcile honors the same pin roots + retention backstop | Reconcile must respect `Δ_skew`+lease (never delete an object newer than `now−retention`, retention > max op) AND honor the `+`/hazard rule; plan §10 has the retention backstop — make it cover in-flight uploads |
| E19 | New writer registers `writers/<Wn> {O_W=E_cur}`; GC `R4.quad` LIST misses it | omitting a *high* `O_W` cannot lower `min` below a live writer; `Wn` itself observed past all reclaimable epochs | **SAFE** | Re-registering writer must re-read `epoch/current` (never reuse stale `O_W`) |
| E20 | `active/<H>` reorder points at a condemned/deleted epoch | reader `404` → LIST fallback → reads a present epoch | **SAFE** (conditional on INV-NO-LOSS) | Mandatory unbounded-generation `404→LIST` retry; in-flight-read restart rule |
| E21 | Writer at `O_W = E_cur−1` (max legal lag) makes a reuse decision as leader advances | `e_a ≤ E_cur−2 < O_W` for any deletable `e_a` ⇒ writer observed past it | **SAFE** (given E1 fix) | Re-sync must precede the *reuse decision*, not just commit |
| E22 | Two writers concurrently resurrect condemned `(H,e_a)` → both create `blobs/<H>/<E_cur>` | `create-if-absent` one-winner; both reuse `E_cur`; `active` LWW to same value | **SAFE** | — |
| E23 | Resurrection leaves the old condemned `(H,e_a)` with a stale `+(H,e_a)` if `+` was logged at decide-time before the tombstone was seen | `+(H,e_a)` with no matching `-` pins `(H,e_a)` in the count forever | **LEAK** | Log compensating `-(H,e_a)` on resurrect-away, or make reconcile mandatory |

---

## 10. Did it actually fix V1 / V3 / V4? {#v-recheck}

The prior review's three losing schedules were all the *same root cause*: **the leader had no fresh,
edge-creating read of a writer-side object the writer set before its reuse decision, and the `+` (consumed only
through a window-bounded fold) could not supply it.** EBR attacks this differently — not by adding a
directly-listed pin, but by **forbidding the leader from deleting until every live writer has provably observed
the condemnation** (`safe_epoch > e_a`). Re-running each:

### V1 (fold/window race — `+` invisible to the deciding fold) {#v1-recheck}

Prior V1: a writer's `+` lands in the open window the deciding fold skipped, the final fold also skips it, the
leader deletes a blob a live ref names.

```
   ORIGINAL V1 outcome: DELETE despite a live ref      EBR outcome:
   ──────────────────────────────────────────────     ─────────────────────────────────────────────
   leader folds closed windows, misses +, deletes      the writer that emitted + had O_W ≤ e_a
                                                        (it appended to log/<O_W>=log/<e_a>); so its
                                                        lease pins safe_epoch ≤ e_a; R5.del requires
                                                        safe_epoch > e_a -> BLOCKED. No delete.
```

**Genuinely fixed — by the epoch-quiescence gate, *provided* the writer cannot advance `O_W` past `e_a` while
its `e_a`-era `+` is not yet durable-and-folded.** This is the V-EBR-1 caveat: if the writer *does* advance
`O_W` (background refresh) before its `+` is durable, `safe_epoch` climbs and V1 *reopens as V-EBR-1*. So: **V1
is fixed for the paused/slow writer (the original V1 actor), and only reopens under the new, narrower
`+`-durability-vs-`O_W`-advance race**, which the V-EBR-1 fix (barrier-coupled advance + closed-epoch fold)
closes. Net: the EBR mechanism is the right shape and *does* close original-V1; the implementation detail it
must add is the barrier. **Materially better than the prior design.**

### V3 (upload→ref window with a correctly-late `+`) {#v3-recheck}

Prior V3: between upload-complete and the `+`/ref, the blob exists with nothing pinning it; the writer pauses;
the leader folds count-0 and deletes.

```
   EBR outcome: the writer doing the upload has a LIVE lease at O_W. For the leader to delete blobs/H/<O_W>
   it needs safe_epoch > O_W, i.e. EVERY live writer (incl. this one) past O_W. This writer is AT O_W and
   paused -> it pins safe_epoch ≤ O_W -> R5.del BLOCKED. The pause costs liveness, not the blob.
```

**Genuinely fixed.** This is EBR's cleanest win: the *pause itself* (the thing that made V3 lose) now holds
reclamation back via the live lease. The only residue is the lease-expiry race — if the writer's pause exceeds
`T_lease` and its lease lapses, the GC may drop it; but then the writer's **self-fence forces read-only** and it
never commits the ref (V-EBR-3 territory, S3-skew-dependent; Keeper exact). **Fixed, modulo the §4 lease
hinge.**

### V4 (final-fold/delete gap; new `+` between final-fold and delete) {#v4-recheck}

Prior V4: a fresh commit dedups onto `H` between the leader's final fold (count 0) and the delete; recheck saw
no tombstone; ref published; blob deleted; dangle.

```
   EBR outcome: the fresh writer that dedups onto (H,e) did W.head AFTER the tombstone existed?
     - If the writer's O_W > e (it observed past the condemn epoch): with the closed-epoch fold (E1 fix), its
       W.head happens-after the tombstone -> it sees it -> resurrects to E_cur, never names (H,e). Safe.
     - If the writer's O_W ≤ e: it pins safe_epoch ≤ e -> R5.del(e) BLOCKED. Safe.
   Either way the delete of (H,e) is blocked or the writer routes away. The "new + between final-fold and
   delete" cannot come from a writer that both (a) hasn't observed the condemn AND (b) doesn't pin safe_epoch.
```

**Genuinely fixed** — again contingent on the closed-epoch fold (E1 fix) for the `O_W > e` branch; the
`O_W ≤ e` branch is fixed unconditionally by quiescence. The prior V4 actor (a writer that rechecks-absent then
pauses across the grace) is now exactly the V3 case: its live lease blocks the delete.

**Summary:** V1/V3/V4 are **genuinely closed by the EBR mechanism** for their original actors (paused/slow
writers). The mechanism is sound and is the correct fix. What remains are **new, narrower races the EBR proof
glosses** — V-EBR-1 (`+`-durability vs `O_W`-advance + missing close barrier), V-EBR-2 (the hazard `+` not
read as a hazard), V-EBR-3 (S3 lease skew/renewal gap) — all of which are *implementation-completeness* gaps in
the same proof, not a return of the old "the `+` can't pin" unsoundness. The directly-listed-pin remedy the
prior review demanded is **no longer required**: the per-writer epoch lease is a legitimate, cheaper substitute
*if* the three gaps are closed.

---

## 11. S3 ↔ Keeper portability + the clock-skew asymmetry {#portability}

The data plane (`epoch/current`, `blobs`, `tombstone`, `active`, `log`, `snap`, refs) uses only
`create-if-absent`/`read`/`delete`/`list` — maps cleanly to both. **Confirmed: only the writer lease and the
fence allocator differ.** What correctness is lost in S3-only mode:

- **Writer lease.** Keeper ephemeral session = single-arbiter liveness, **no cross-clock skew, no renewal
  gap** (keepalive is linearized). S3 = TTL object + local self-fence + `Δ_skew` wait — **V-EBR-3 (E3/E4) is a
  real S3-only loss** unless the directional-skew + read-back-durable-renewal fixes are applied, and even then
  safety *assumes* `GC_clock − W_clock ≤ Δ_skew` holds always (an assumption, not a guarantee). **Quantified
  loss: S3-only trades a proof for a bounded-skew assumption; a skew excursion beyond `Δ_skew` = data loss.**
- **Fence allocator.** Keeper ephemeral-sequential / czxid + linearizable reads ⇒ a leadership steal is
  observable atomically by the old leader's pre-delete fence-read; **E12 split-brain is SAFE on Keeper.** S3 =
  `create-if-absent` counter + `GET gc.lock`; a paused leader's fence-read can race a not-yet-durable steal ⇒
  **E12 is a real S3-only VIOLATION** unless (a) `gc.lock` is read-after-write-strong (S3 single-object: yes)
  AND (b) a freshly-stealing leader out-waits the prior lease + `Δ_skew` before its first mutation. State both.

**Conclusion:** the plan's "S3-only works; Keeper is a strictly-cleaner drop-in (and removes the one
clock-skew assumption)" is **almost right but understates the asymmetry**: Keeper removes *two* correctness
hazards (the lease skew/renewal gap AND the fence-steal-visibility gap), and S3-only is safe only under *two*
bounded-skew/out-wait assumptions, not one. The plan names only the lease skew. **Must-fix: enumerate both S3
assumptions and the out-wait rule.**

---

## 12. Top-line verdict {#verdict}

**The EBR design is conceptually correct and a genuine fix — the central idea (a live writer holds reclamation
back instead of losing data) genuinely closes the prior V1/V3/V4 — but the plan is NOT correct as written.** It
is *spec-ready in shape* and *not spec-ready in detail*: the load-bearing happens-before edge of its own proof
(condemn-durable-before-advance, over a *closed*-epoch fold) is left unsequenced, and two of its headline
mechanisms (the early-`+` hazard pin; the S3 time-lease) are asserted to be safe without the couplings that
would make them so. None of the gaps require re-introducing the directly-listed per-blob pin the prior review
demanded — the per-writer epoch lease legitimately replaces it — so the simplification's core claim survives.

### VIOLATIONS / must-fix gaps, most important first {#must-fix}

1. **(VIOLATION — V-EBR-1, the proof's missing edge) No closed-epoch fold + no `O_W`-advance/`+`-durability
   coupling.** `R1.fold` must read a *closed* `log/<e_a>` (restore a fenced window-close barrier;
   reappend-to-open-epoch, not overlap-window), and a writer must not advance `O_W` past `e` until every `+` it
   appended at `e` is durable-and-folded. Without both, the case split in §4 of the EBR doc is non-exhaustive
   and a committed-then-advanced writer loses its `e`-era dependency (E1/E17). This is the single gap that most
   directly breaks the proof.
2. **(VIOLATION — V-EBR-2, the hazard pin doesn't pin) The early-`+` is read only through an epoch-window
   fold.** Either make the `+` a true hazard the leader point-reads at delete time (fresh `LIST log/.../+(H,e_a)`
   across all open epochs before `R5.del`), or couple `O_W`-advance to `+`-durability (flush-`+`-then-advance)
   so the long op holds `safe_epoch` for only the sub-second decide→`+`-durable window. As written, the long-op
   safety claim is false (E2).
3. **(VIOLATION — V-EBR-3, S3-only) The time-lease skew bound is undirected and ignores the renewal gap.**
   Self-fence at `lease_until − Δ_skew`, GC drop at `lease_until + Δ_skew` (symmetric `2·Δ_skew` margin), bound
   `GC_clock − W_clock ≤ Δ_skew` directionally, and let a writer rely only on *read-back-durable* lease
   renewals. Bound or re-check the self-fence→ref-PUT stall (E5). Recommend Keeper, which removes this entirely
   (E3/E4).
4. **(VIOLATION — S3-only) Split-brain fence-steal visibility (E12).** `gc.lock` read-after-write-strong + a
   freshly-stealing leader out-waits the prior lease + `Δ_skew` before its first mutation + every `R5.del`
   re-reads the fence. Keeper: safe by construction. The plan calls fencing the "only divergence" but treats it
   as ergonomic, not a *safety* asymmetry.
5. **(NEEDS-FIX) Successor leader must re-FOLD+re-QUIESCE under its own fence before any delete (E10);
   `epoch/current` must be strictly monotone, 64-bit, never reset/wrapped (E15); `snap` publish must be
   single-object atomic with version+checksum (E16); reconcile must honor the lease/`+`/retention roots so it
   cannot reclaim an in-flight upload (E18).**
6. **(LEAK) Resurrection-away from a condemned epoch leaves a `+(H,e_a)` with no matching `-` (E23)** — log a
   compensating `-`, or make reconcile a *mandatory* backstop (not default-off).

### Are V1/V3/V4 genuinely fixed? {#v-fixed-summary}

**Yes — for their original actors (paused/slow writers), genuinely and by the right mechanism (epoch
quiescence + the live lease holding `safe_epoch` low).** V3 is the cleanest win (the pause now causes a
liveness stall, not loss). V1 and V4 are fixed *contingent on the closed-epoch fold* (must-fix #1) for their
`O_W > e_a` branch and unconditionally for their `O_W ≤ e_a` branch. The old unsoundness ("the `+` cannot pin
because it's only folded by window") does **not** return; the new residual races (V-EBR-1/2/3) are
completeness gaps in the EBR proof, all closable without a per-blob pin.

### Under-specified — could not be decided as written {#underspecified}

- **§13.3 epoch rotation vs in-flight `+`** — the doc explicitly defers the decision; the *whole proof* depends
  on it. Tested under the natural "fold the open epoch then advance" reading → **unsafe** (V-EBR-1). Under the
  "reappend-to-closed-epoch + barrier-coupled advance" reading → safe. The doc must pick the latter and prove
  it; as written, correctness is undecidable.
- **Whether `O_W` may advance on `+`-issued vs `+`-durable** — §6's "rule of thumb" says decided deps must be
  "pinned or resurrected" before advance, but calls it "a non-constraint." Tested under "`+`-issued suffices" →
  **unsafe** (V-EBR-2/E2). Under "`+`-durable-and-folded required" → safe.
- **`snap` carry-forward retention for `+`-pinned-but-0 objects** — unspecified; if a `+`'d object's count can
  return to 0 in the fold and be dropped, the hazard evaporates (V-EBR-2 (ii)).
- **`Δ_skew` direction + renewal atomicity on S3** — §7 sizes `Δ_skew` "in minutes" but does not state the
  directional bound or the read-back-durable-renewal rule; correctness undecidable until stated (V-EBR-3).
- **Successor-leader candidate re-derivation** — §9 says "successor takes a higher fence" but not that it must
  re-fold before trusting a prior round's candidates (E10).

**Bottom line for a spec:** adopt EBR — it is the correct shape and it really does fix V1/V3/V4 — but the spec
is not done until it (1) folds only *closed* epochs and couples `O_W`-advance to `+`-durability, (2) makes the
early-`+` an actual hazard (point-read at delete, or flush-then-advance), and (3) on S3, states the directional
skew bound, the read-back-durable renewal, the symmetric self-fence/drop margins, and the leader out-wait. With
those, the EBR proof's case split becomes exhaustive and `INV-NO-LOSS` holds for any pause length — the property
the design set out to deliver.
