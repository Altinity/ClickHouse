---
description: 'Adversarial distributed-systems correctness review of the simplified content-addressed (CA) MergeTree garbage-collection design (the §6 page-sized protocol in 2026-06-07-ca-gc-simplification-analysis.md), grounded in the exact S3/MinIO and Keeper primitive semantics. Models state/events/happens-before, analyzes per-invariant interleavings, runs a failure-injection catalog, and delivers a verdict with must-fix gaps.'
sidebar_label: 'CA simplified GC — correctness review'
sidebar_position: 21
slug: /superpowers/reports/ca-gc-simplified-correctness-review
title: 'Adversarial Correctness Review — Simplified CA GC (the page-sized protocol)'
doc_type: 'reference'
---

# Adversarial Correctness Review — Simplified CA GC (the page-sized protocol) {#review}

Panel: (1) a distributed-systems correctness specialist (happens-before / linearizability / invariant
reasoning); (2) an object-storage (S3 / MinIO) systems expert. Subject under test: the **§6 "page-sized
protocol"** of `docs/superpowers/reports/2026-06-07-ca-gc-simplification-analysis.md`, with context from
`docs/superpowers/reports/2026-06-07-ca-protocol-and-lockless-gc.md` and the I1–I8/G1–G4 backlog
(`docs/superpowers/deferred_backlog/cas-mergetree-integration.md`).

This review is deliberately adversarial. The job is to find the schedules that lose data, strand a ref, or
leak forever — not to bless the design.

## 0. The protocol as written (restated so we test exactly it) {#restated}

> One per-pool **log** (`+`/`-` deltas, `create-if-absent`) and **snapshot** (folded counts). A single fenced
> **leader** folds the log and, for any blob count-0 past a grace and still 0 on a final fold, **condemns** it
> (`<g>.tombstone`, `create-if-absent`) then **deletes** `<g>`. Writers log `+` BEFORE upload and `-` AFTER
> ref-removal; on dedup they recreate into `g+1` if `g` is condemned. Reads resolve the present generation by
> `LIST`. Full scan only rebuilds the snapshot. No sessions, no `active` hint, no `gc/sealed`, no resurrection
> cap. P1 handled by the `+`-as-pin; P2 by stripped generations.

The single most consequential change from the implemented branch (per the companion doc §4.4) is that the
simplified design **deletes the load-bearing Scan-B reachability gate** and makes the **folded count the sole
delete authority**. The implemented branch explicitly does *not* trust the count yet (B78, DO-NOT-MERGE). So
this review is, in essence, a stress test of "is the count actually safe to be authoritative, given only the
four primitives + fence, once you also delete sessions and rely on the `+` as the only pin?"

**Spoiler / top-line:** No, not as written. The `+`-as-pin claim (P1) is **unsound** under the stated S3
primitives, and the "final fold then delete" step has a **fold/delete gap** that loses data. Both are
fixable, but the fixes re-introduce structure the simplification claims to shed. Details below.

---

## 1. Formal-ish model (Lamport) {#model}

### 1.1 Durable state (the only ground truth — the bucket) {#durable-state}

Per pool, the durable objects (S3 keys; Keeper holds no durable state, G4):

- `blobs/<H>/<g>` — immutable content bytes for content hash `H` at generation `g` (`g=0` common).
- `blobs/<H>/<g>.tombstone` — condemn marker (the "seal"), created only by the leader.
- `parts/<part_id>/<mg>` and `.tombstone` — manifest, identical lifecycle (we fold blobs and manifests into
  one "object" abstraction below; everything proven for blobs holds for manifests).
- `store/<srv>/<uuid>/refs/<part>` — the live ref (GC root, commit point).
- `gc/log/<window>/<event_id>` — append-only `+`/`-` delta objects (`create-if-absent`).
- `gc/snap/<epoch>` — folded `CountKey{kind,identity,g} → count}` run.
- `gc.lock` + `fence/<n>` — leader lock + monotonic fence token.

Note: the simplified design **removed** `sessions/<id>`, `active`, `gc/sealed`, `gc/current_epoch` per-shard
hint object. We test under that removal.

### 1.2 Actors {#actors}

- **Writers** `W1..Wn` — concurrent, uncoordinated. Each can pause unboundedly between any two operations
  (GC pause, VM freeze). A writer commits a part referencing a set of `(H,g)`.
- **Leaders** `L` — 1+ contending GC leaders (split brain possible under pause). Exactly one *should* act;
  fencing is supposed to enforce it.
- **Readers** `R` — resolve a live ref → manifest → blobs, by `LIST blobs/<H>/` to find the present `g`.

### 1.3 Atomic events (each is one primitive op; each may be retried/duplicated/delayed/reordered) {#events}

Writer `W` committing a part touching content `H` at resolved generation `g`:

```
W.list   : LIST blobs/<H>/                        (resolve present g; detect tombstone)
W.plus   : createIfAbsent gc/log/<win>/<eid(+,H,g)>     ("+"; the claimed pin)
W.up     : createIfAbsent blobs/<H>/<g>           (upload, or dedup-skip if present)
W.recheck: HEAD/GET blobs/<H>/<g>.tombstone       (tombstone re-check)
W.gpp    : createIfAbsent blobs/<H>/<g+1>         (recreate into g+1 on dedup-into-condemned)
W.ref    : createIfAbsent store/.../refs/<part>   (commit point, written LAST)
W.minus  : createIfAbsent gc/log/<win>/<eid(-,H,g)>   ("-"; AFTER ref removal, in drop)
W.unref  : DELETE store/.../refs/<part>           (drop: ref removed FIRST)
```

Leader `L`:

```
L.close  : (fenced) advance the open log window / snapshot epoch pointer
L.fold   : LIST gc/log/<win>/ + read gc/snap/<E>  → fold → write gc/snap/<E+1>
L.recl   : DELETE folded gc/log + old gc/snap      (idempotent)
L.cond   : (fenced) createIfAbsent blobs/<H>/<g>.tombstone     (condemn)
L.final  : (fenced) re-fold open log; confirm count(H,g)==0
L.del    : (fenced) DELETE blobs/<H>/<g>           (physical reclaim)
L.fence? : read gc.lock; confirm fence_token still mine  (gates every fenced op)
```

### 1.4 Happens-before (→) edges the primitives actually give us {#hb}

S3/MinIO guarantees (the *only* ones we may assume):

- **HB-RAW (read-after-write).** If `createIfAbsent K` (or PUT `K`) completes at real time `t`, any `GET/HEAD
  K` or `LIST` of the enclosing prefix that *starts after `t`* sees `K`. This gives an edge **only when the
  reader provably starts after the writer's op completed**. There is *no* edge from "writer issued the PUT"
  alone — an in-flight, not-yet-acknowledged PUT is invisible and may even land *after* a later reader's LIST.
- **HB-CIA (create-if-absent linearization).** All `createIfAbsent K` for the same `K` are totally ordered;
  exactly one wins, losers observe failure. This is a per-key linearization point.
- **HB-PROG (program order within one actor).** A single actor's ops are ordered *as issued*, but **not as
  durably-visible to others** unless each completed (HB-RAW). A paused actor contributes no edges during the
  pause.
- **No edge** is created by DELETE toward anything (DELETE is unconditional/idempotent, gives no ordering).
- **LIST is strongly consistent but paginated**: a single LIST is a consistent snapshot *per page*; across
  pages a concurrent create/delete may or may not appear. We assume an object created-and-acknowledged before
  the *first* page request is seen; an object created *during* pagination may be missed. This matters for
  `L.fold` (LIST of `gc/log`) and `W.list`.

The crucial, repeatedly-load-bearing fact: **`W.plus` (a `createIfAbsent` of a `gc/log` object) creates an
HB edge to the leader only after `W.plus` has *completed* AND the leader's fold-LIST *starts after* that
completion.** A `+` that is issued-but-not-acknowledged, or that lands in a log window the leader has already
closed/folded, gives the leader **no edge** and is invisible to that fold.

### 1.5 Safety invariants (precise) {#invariants}

- **INV-NO-LOSS.** No `blobs/<H>/<g>` is `DELETE`d while it is reachable from (a) a live ref, or (b) a `+`
  that any correct future fold must still count. Equivalently: the leader must never `L.del(H,g)` unless a
  fold that *causally observes every committed-or-in-progress `+` for `(H,g)`* shows count 0.
- **INV-NO-DANGLE.** Every live ref resolves to present bytes: for every `refs/<part>` → `part_id` → manifest
  → `(H,*)`, at least one generation of `H` exists.
- **INV-NO-LEAK-FOREVER.** Every object not reachable from any live ref is eventually deleted (liveness).
- **INV-NO-ABA.** A `DELETE blobs/<H>/<g>` never destroys bytes a concurrent writer is committing under the
  same key `(H,g)` (P2).

`INV-NO-LOSS` and `INV-NO-DANGLE` are the data-loss invariants. `INV-NO-ABA` is P2. `INV-NO-LEAK-FOREVER` is
liveness. `INV-NO-LOSS` reachability via in-progress uploads is exactly P1.

---

## 2. Per-invariant interleaving analysis {#per-invariant}

### 2.1 INV-NO-ABA (P2 / generations) — HOLDS {#aba}

This is the part the simplification gets right, and it survives stripping. Claim: a writer that dedups into a
condemned `g` recreates into `g+1`; the leader only ever `DELETE`s the specific condemned `g`.

```
   WRITER W                              LEADER L
   ────────                              ────────
                                         L.cond:  createIfAbsent blobs/H/<g>.tombstone   (wins)
   W.recheck: HEAD blobs/H/<g>.tombstone  ── HB-RAW (recheck starts after cond done) ─►  PRESENT
   W.gpp:    createIfAbsent blobs/H/<g+1>  (different key)
   W.ref:    ref → bare H (reads resolve g+1 by LIST)
                                         L.del:  DELETE blobs/H/<g>   (g+1 untouched)
```

If instead `W.recheck` *precedes* `L.cond` completing (writer saw tombstone absent), then `W` proceeds on
`g`. For ABA-safety we need `L.del(H,g)` not to fire while `W` still wants `g`. That is **not** guaranteed by
generations alone — it is guaranteed by the *count/pin* (INV-NO-LOSS). Generations only guarantee that the
**recreate** does not collide with the **delete** on the same key. The same-key collision is the only thing
"stripped generations" must prevent, and `createIfAbsent` into `g+1` does prevent it: `L.del` targets exactly
`g`, the writer's bytes are at `g+1`. **Verdict: INV-NO-ABA holds.** (This is also why the open decision in
§8 of the analysis — generations vs delete-marker — should resolve to generations; a same-key delete-marker
would *not* give this.)

Caveat carried forward: ABA-safety presupposes the writer's `W.recheck` happens-after the leader's `L.cond`
when the leader is about to delete `g`. The handshake in the companion doc enforced this via "seal (M) before
ref-check (L)". The simplified protocol's ordering for this is examined in 2.3 — and it is where the
`sessions` removal bites.

### 2.2 INV-NO-DANGLE — HOLDS (modulo the count being correct) {#no-dangle}

A live ref names bare `H`; reads `LIST blobs/<H>/` and read any present generation. As long as INV-NO-LOSS
holds (the leader never deletes the last generation of an `H` a live ref needs), every live ref resolves.
The LIST-resolves-present-generation step is robust: it does not depend on the `active` hint (correctly
dropped), only on strongly-consistent LIST. **Verdict: holds, conditional on INV-NO-LOSS.** The risk is
entirely pushed into INV-NO-LOSS.

### 2.3 INV-NO-LOSS via the `+`-as-pin (P1) — **VIOLATION** {#no-loss-pin}

This is the heart of the review. The analysis §3 asserts:

> Log the `+` before the upload … and the `+` *is* the durable pin — the delete gate folds the open log and
> sees it.

The hidden assumption is: **"the leader's fold sees the `+`."** Under the real primitives that is *not*
guaranteed, because there is no happens-before edge from "writer issued `+`" to "leader folded" unless the
`+` both (i) completed durably and (ii) landed in a window the leader had not yet closed before folding. Two
distinct violating schedules:

#### Violation V1 — the fold/window race (a `+` invisible to the deciding fold) {#v1}

The leader folds *closed* windows (it must — folding an open window racing live appends is itself unsound;
the companion doc closes the window via `gc/current_epoch` before LISTing, §4.2 step 1). So there is always a
"current open window" `Wopen` that the deciding fold does **not** include. A writer's `+` that lands in
`Wopen` after the leader closed it for this round is, by construction, not in the fold that decides count==0.

```
   WRITER W (new content H, g=0, first ever)        LEADER L (deciding to delete some OTHER H' that nets 0)
   ────────────────────────────────────────        ───────────────────────────────────────────────────
   ... H happens to collide-dedup with an H        L.close:   advance window: Wopen := win+1
       the leader is about to reclaim ...           L.fold:    LIST gc/log/<win>  (does NOT include win+1)
   W.list: LIST blobs/H/  → H present (g=0)          → count(H,0) == 0  (the old refs all dropped)
   W.plus: createIfAbsent gc/log/<win+1>/+(H,0)     L.cond:    createIfAbsent blobs/H/0.tombstone   (wins)
        (lands in the OPEN window the fold skipped)
   W.recheck: HEAD blobs/H/0.tombstone
```

Now the decisive ordering: `W.recheck` vs `L.cond`. The protocol as written says the writer rechecks the
tombstone and, if present, recreates into `g+1`. **But the leader's `L.final` ("still 0 on a final fold")
also folds only closed windows.** So:

```
   W.recheck happens-before L.cond completes?   →  writer sees NO tombstone, proceeds on g=0, publishes ref to bare H at g=0
   L.final: re-fold closed windows → still count(H,0)==0  (the + is in the still-open window+1)
   L.del:   DELETE blobs/H/0      ←──────────  DATA LOSS: the ref published by W now dangles
```

The `+`-as-pin failed because the `+` is in a window the final fold does not read. The companion design did
*not* have this hole because the **session pin** (`sessions/<id>`) is a *directly-listed object*, not a log
entry folded by-window: the leader's fresh re-check `computeReachability` does `LIST sessions/` at delete
time (HB-RAW: that LIST starts after the writer's `persistSession` completed), giving a real edge. The `+`
log entry has no such direct-LIST-at-delete-time read; it is only ever consumed *through a window-bounded
fold*. **Removing sessions removed the only object the leader reads with a fresh, edge-creating LIST
immediately before delete.** The `+` cannot replace it unless the leader also does an edge-creating read of
the `+` at delete time — i.e. folds *the open window too*, or LISTs the raw log prefix for any `+(H,*)`
before `L.del`. Neither is in the page-sized protocol.

Minimal fix for V1: the `L.final` gate must not be "re-fold closed windows"; it must be **"LIST the entire
`gc/log` prefix (all windows incl. the open one) and confirm no live `+(H,g)` exists that a future fold would
count, AND confirm no ref names `H`."** That is a fresh, edge-creating read at delete time. But note this
re-introduces a read whose cost is the open log size, and it still races V2 below.

#### Violation V2 — the `+`-before-recheck inversion (pin raised after condemn observed absent) {#v2}

Even if the leader reads the open window, the *writer's* ordering in §6 is `+` **before** `recheck`
(`W.plus` then `W.recheck`). Consider the leader reading the open window at `L.final`:

```
   WRITER W                                LEADER L
   ────────                                ────────
                                           L.cond: createIfAbsent blobs/H/0.tombstone   (wins)
   W.plus: createIfAbsent gc/log/+(H,0)    L.final: LIST whole gc/log → SEES +(H,0)  → spare? OR count fold nets 0?
   W.recheck: HEAD .tombstone → PRESENT
   W.gpp: createIfAbsent blobs/H/1
   W.ref: ref → bare H  (intends g=1)
```

Here the writer *did* recheck and route to `g+1` — good for that writer. But the leader saw a `+(H,0)` in the
open window. What does the leader do with it? The fold *counts* it: `+(H,0)` makes count(H,0) == 1, so the
leader would **spare** `blobs/H/0` even though *no ref will ever name g=0* (the writer abandoned it for g=1).
That is a **leak**, not a loss — tolerable for safety but it directly contradicts the analysis's claim that
"an aborted writer leaves a stale `+` … cleaned by reconcile": here a *successful* writer leaves a stale
`+(H,0)` because it logged `+` for `g=0` *before* discovering it had to move to `g=1`, and the protocol as
written **never logs a compensating `-(H,0)`** (the `-` is only emitted on drop, after ref-removal, and there
is no ref to `g=0`). So `blobs/H/0` is pinned forever by a `+` with no matching `-`. **Verdict: a real
INV-NO-LEAK-FOREVER violation** (only `reconcile`/full-scan can clean it; the count alone never will). The
companion design avoided this precisely by logging `+` **after** the tomb re-check (`appendAndFlushForCommit`
runs at step 6, *after* the step-4 re-check), so the `+` records the *settled* `(H,g)`. The simplification's
"`+` BEFORE upload" reordering broke that, because "before upload" forces "before recheck" (recheck needs the
upload's generation resolved).

So V1 and V2 are a scissor: log `+` early enough to pin (before upload) and it pins the *wrong, pre-recheck*
generation (V2 leak / and still misses the window V1); log `+` late enough to be correct (after recheck) and
it no longer pins during the upload→recheck window (P1 re-opens — see V3).

#### Violation V3 — the upload→ref window with a *correctly-late* `+` {#v3}

Suppose we "fix" by logging `+` after the recheck (as the companion does). Then between `W.up` (upload
completes) and `W.plus` there is a window where the blob exists but no `+` and no ref pins it:

```
   WRITER W                          LEADER L
   ────────                          ────────
   W.up: createIfAbsent blobs/H/0    (bytes now exist; no + yet, no ref)
        ── W pauses (VM freeze) ──
                                     L.fold: count(H,0)==0 (no + anywhere)
                                     L.cond + L.final + L.del → DELETE blobs/H/0
        ── W resumes ──
   W.recheck: HEAD .tombstone → ABSENT (leader already deleted it AND its tombstone? — see note)
   W.plus / W.ref → ref to a deleted blob  → DANGLE
```

This is the canonical P1 window, and it is exactly what `sessions/<id>` existed to close: the session pin is
raised *before* the upload, is a directly-LISTed root, and the leader's fresh re-check sees it. With sessions
removed and `+` logged after recheck, **nothing pins the blob during upload→`+`**. The analysis's "`+`
BEFORE upload" was trying to avoid V3 — but that is what causes V1/V2. **There is no single placement of a
single `+` that closes all three.** You need *two* signals: an early pin (before upload, P1) and a late
authoritative count edge (after recheck, correctness). The companion design used two objects for exactly this
reason: the session (early pin, directly listed) and the `+` (late count). The simplification's thesis that
"the `+` is the pin" collapses these two into one and is therefore **unsound**.

**Minimal fix:** re-introduce an early, directly-listed pin that the leader reads with a fresh LIST at delete
time. It need not be the heavy `WriteSession` lifecycle — a single `create-if-absent pin/<H>/<writer-uuid>`
object (deleted after the ref is published) suffices and stays within the four primitives. But that is "a
session by another name," and it brings back lease-based reaping for crashed writers (else INV-NO-LEAK-FOREVER
fails: a crashed writer's pin is immortal). So the simplification *cannot* truthfully claim "no sessions."

### 2.4 INV-NO-LOSS via the final-fold/delete gap — **VIOLATION (V4)** {#v4}

Independent of P1, the "final fold then delete" step has its own gap even for the *drop* side. The protocol:
writer logs `-` AFTER ref-removal. The leader deletes if "count 0 past grace and still 0 on a final fold."
But a *new* `+` (a brand-new commit that dedups onto `H`) can land between `L.final` and `L.del`:

```
   LEADER L                              WRITER W (fresh commit dedup-onto H)
   ────────                              ────────────────────────────────────
   L.final: fold → count(H,0)==0
                                         W.list: blobs/H/ → g=0 present, no tombstone yet
                                         W.up:   dedup-skip (H/0 exists)
                                         W.plus: +(H,0)   (open window)
                                         W.recheck: .tombstone ABSENT  →  proceed on g=0
                                         W.ref:  ref → bare H @ g=0
   L.del:  DELETE blobs/H/0     ←──────  DANGLE (ref names a deleted blob)
```

The condemn step (`L.cond` writes the tombstone) is supposed to slam the door: once `blobs/H/0.tombstone`
exists, `W.recheck` sees it and routes to `g+1`. So the *order* `L.cond` → `L.final` → `L.del` matters. If
`L.cond` truly precedes the writer's `W.recheck`, V4 becomes the ABA-safe case 2.1 (writer goes to g+1, leader
deletes g=0 safely). The danger is only when `W.recheck` linearizes **before** `L.cond` completes but
`W.ref` lands after `L.final`. Timeline:

```
   W.recheck (.tombstone ABSENT)  ──HB?──►  L.cond (.tombstone created)   : recheck BEFORE cond
   L.final (count==0)             ────────►  L.del                         : final BEFORE del
   W.ref (→ H@0)                  must be ordered vs L.final
```

For loss we need: `W.recheck` before `L.cond`, and `W.ref` after `L.final`'s count read. Is that schedulable?
Yes: the writer rechecks (absent), then **pauses** (VM freeze) for the whole grace window, then publishes the
ref after the leader's final fold. The leader's final fold counted 0 because the writer's `+` either was in
the open window (V1) or the writer logs `+` after recheck and the `+` also landed post-final-fold. **Loss.**

The companion design closes V4 with the handshake invariant `end(publish) ≤ start(recheck) < end(seal) ≤
start(refcheck) < end(publish)` being unsatisfiable — but that chain relies on the **session pin being read at
`refcheck` time** (the leader's `L` step reads live sessions). Remove sessions and the chain breaks: there is
nothing the leader reads at `L.final` that the paused writer wrote before its recheck. **The `+`-as-pin does
not reconstitute this edge** for the same reason as V3. So V4 is the same root cause as V1–V3: the leader has
no fresh, edge-creating read of a writer-side object that the writer set *before* its recheck.

**Minimal fix:** identical to V3 — an early, directly-listed pin read at `L.final`/pre-`L.del`. With such a
pin, the paused writer's pin is visible at delete time → spare. Without it, no fix to the *log folding* alone
closes V4, because the log is window-bounded and the writer can always be the one whose `+` is in the open
window.

### 2.5 INV-NO-LOSS under the fence (paused leader) — HOLDS *iff* every DELETE is fence-gated {#fence}

The simplification keeps "a single fenced leader" and the S3 lock+monotonic-fence / Keeper ephemeral-seq. The
companion §5.5 gates *every* delete (and seal and epoch-close) on a fresh `gc.lock` fence re-read. **This is
load-bearing and must be stated in the spec.** The page-sized §6 text says "a single fenced leader … condemns
… then deletes" but does **not** explicitly say the fence is re-checked immediately before each DELETE. Under
the fenced-token hazard (leader pauses past lease, successor steals with higher fence), a leader that does
*not* re-check the fence between `L.final` and `L.del` will delete under a stale token → loss. Timeline:

```
   L1 (fence=7)                          L2
   ───────────                           ──
   L1.final: count(H,0)==0
        ── L1 pauses past lease ──
                                         steal gc.lock {fence=8}
                                         (L2 may legitimately spare H now: a new + arrived)
        ── L1 resumes ──
   L1.del: DELETE blobs/H/0   ← if NOT fence-gated: LOSS under stale fence
```

**Verdict: holds only if the spec mandates `L.fence?` immediately before `L.del` (re-read `gc.lock`, confirm
token still mine).** As written, §6 is under-specified here. Must-fix: state it.

Two further S3-specific fence subtleties the spec must pin down:

- The S3 fence is a *lock object + monotonic counter* read with `GET gc.lock`. The "re-read confirms my
  token" gives HB-RAW only relative to the *steal's `createIfAbsent`/PUT completion*. A steal that is
  in-flight (not yet acknowledged) when L1 re-reads is invisible → L1 believes it is still leader. This is
  fine for safety **only because** the steal can itself not have started deleting until its own
  `createIfAbsent fence/<n>` completed; but two leaders can both pass their own fence-check in overlapping
  windows. Safety then depends on: a DELETE by L1 (fence 7) and a DELETE by L2 (fence 8) of the *same blob*
  are both only issued after each independently concluded count==0; if L2 concluded "spare," L2 issues no
  delete, but **L1 still issues its delete** under the stale fence unless L1's fence-recheck observes fence 8.
  So the safety reduces to: *L2's steal must be durably visible to L1's pre-delete fence-read*. That requires
  L1's fence-read to start after L2's `gc.lock` PUT completed. A paused L1 that resumes and immediately
  deletes with a fence-read that races L2's not-yet-durable steal **can still lose**. The Keeper backend does
  not have this hole (the ephemeral-seq + linearizable read make the steal observable atomically); the S3
  backend does, unless the fence object write is read-your-writes-strong *and* the steal completes-before
  L1's recheck. **This is a genuine S3-vs-Keeper divergence the analysis under-states** (§2 calls fencing the
  "only divergence" but treats it as merely "awkward O(n) scan", not as a *correctness* asymmetry).

### 2.6 INV-NO-LEAK-FOREVER (liveness) — HOLDS only with reconcile; stale-`+` leaks accrue {#leak}

Even setting aside V2's stale-`+`, the simplified design relies on `reconcile` (full scan) to clean: aborted
writers' `+` with no `-`, leaked `g=0` from resurrection, and (if the V3 fix adds pins) crashed-writer pins.
The analysis correctly notes reconcile exists. But it claims the *common path* never leaks. V2 shows a
**successful** writer leaks `blobs/H/0` whenever it resurrects to `g+1`, and the count never reclaims it
(no `-` is ever logged for the abandoned `g=0`). So leak accrual is on the *normal resurrection path*, not
just the crash path. **Verdict: liveness holds only because reconcile is a backstop; the claim that
resurrection is leak-free under the count is false.** Must-fix: either log a compensating `-(H,g)` when
resurrecting away from a condemned `g`, or accept reconcile as mandatory (not "default 0 = off").

---

## 3. Failure-injection catalog {#catalog}

Verdicts: **SAFE** / **VIOLATION** (data loss or permanent dangle) / **LEAK** (liveness-only) / **NEEDS-FIX**
(safe only with a stated fix the §6 text omits).

| # | Injected fault | Trace (abbrev.) | Verdict | Minimal fix |
|---|---|---|---|---|
| F1 | Writer crash between `W.plus` and `W.up` | `+` exists, no bytes, no ref. Fold counts `+` → spares a nonexistent blob (no-op); reconcile drops the stale `+`. | **LEAK** (bounded) | Reconcile mandatory; or `-`-on-abort (no actor to do it → reconcile). |
| F2 | Writer crash between `W.up` and `W.ref` (no pin) | Bytes exist, no `+` (if `+` is late) or `+` only (if early). Late-`+`: blob has count 0 → leader deletes it → harmless (no ref). | **SAFE** for loss; **LEAK** if `+` early. | — |
| F3 | Writer crash between `W.ref` and `W.minus` (drop) | Ref gone, `+` still counted → over-count → blob spared. Reconcile reclaims. | **LEAK** (I8 over-count bias) | Reconcile. |
| F4 | `+` lands in the open window the deciding fold skipped (V1) | Leader folds closed windows, sees count 0, condemns+deletes a blob a live ref will name. | **VIOLATION** | `L.final` must LIST the *whole* log (open window incl.) + confirm no ref; or re-introduce a directly-listed pin. |
| F5 | `+` logged before recheck; writer resurrects to `g+1` (V2) | `+(H,0)` never gets a `-`; `blobs/H/0` pinned forever by count. | **LEAK** | Log `-(H,g)` on resurrect-away, or rely on reconcile. |
| F6 | Writer pauses between recheck(absent) and ref, across grace (V4) | No fresh writer-object read at delete time; leader deletes; ref dangles. | **VIOLATION** | Early directly-listed pin read at pre-delete; OR forbid recheck-then-pause by requiring pin-before-recheck that the leader reads. |
| F7 | Upload→`+` window with late `+`, writer pauses (V3) | Bytes exist, no pin, leader deletes. | **VIOLATION** | Early directly-listed pin (a `pin/<H>/<uuid>`), reaped by lease. |
| F8 | Leader crash between `L.cond` and `L.del` | Tombstone exists, blob exists. Successor re-discovers candidate (must re-LIST `blobs/` for open tombstones — but `gc/sealed` index was *removed*!). | **NEEDS-FIX (liveness)** | Re-deriving candidates "from the compaction each round" works only if count is still 0; if a `+` arrived, RECOVER path must un-seal — but §6 has no RECOVER/DRAIN state machine. Must specify tombstone-resolution on successor. |
| F9 | Leader crash between `L.fold` and `L.cond` | Snapshot may be written (`gc/snap/<E+1>`) or not; old log reclaimed or not. Idempotent PUTs/DELETEs → successor re-folds cleanly. | **SAFE** (idempotent) | — (state that fold + reclaim are idempotent). |
| F10 | Leader pause past lease, resume, `L.del` without fence re-check | Stale-fence delete after successor legitimately spared. | **VIOLATION** | Mandate `L.fence?` immediately before every `L.del`/`L.cond`. §6 omits this. |
| F11 | Split brain: two leaders both fence-checked in overlapping windows (S3) | Steal not durably visible to paused leader's fence-read; both pass; one deletes a spared blob. | **VIOLATION (S3 only)** | S3 fence-read must complete-after the steal's lock PUT; needs a read-after-write-strong fence object + a rule that a *fresh* leader does not delete until it has out-waited the previous lease (bounded-drift assumption). Keeper: SAFE. |
| F12 | Duplicated/retried `W.plus` (same `event_id`) | `createIfAbsent` idempotent; fold dedups by key/`event_id`. | **SAFE** | — (keep `event_id` dedup; §6 doesn't mention it but it's needed). |
| F13 | Duplicated/retried `L.del` (DELETE replay) | DELETE idempotent; second is a no-op — *unless* a writer recreated `(H,g)` between the two DELETEs. Generations prevent same-key recreate while condemned, so recreate is at `g+1`; replayed `DELETE g` still only hits `g`. | **SAFE** | — (relies on tombstone staying ⇒ no g-recreate; if tombstone was RECOVER-deleted, replay-DELETE can kill a re-attached g — see F8). |
| F14 | Reordered log appends (`+` after `-` for same key arrive out of order) | Fold sums counts commutatively; order within a folded window irrelevant. But a `-` folded in window E and its `+` in open window E+1 → transient count goes negative→clamp? | **NEEDS-FIX** | Fold must tolerate negative running counts (clamp/defer) or guarantee `+` precedes `-` causally (it does per writer, but cross-window split breaks it). Specify signed-count handling. |
| F15 | LIST pagination misses a just-created `gc/log/+` during `L.fold` | The `+` is omitted from the fold → count too low → premature condemn. Same effect as V1. | **VIOLATION** | Close window before LIST (HB barrier): only LIST a window after a fenced "no more appends here" marker; appends after close go to next window. Companion does this via `gc/current_epoch`; §6ust keep an equivalent close barrier (it says "folds the log" without specifying the close). |
| F16 | Snapshot write torn / partial (`gc/snap/<E+1>` half-written) | A reader/folder of a partial snapshot under-counts. | **SAFE** (fail-closed) iff snapshot is single-object atomic PUT + magic/version + checksum; **VIOLATION** if multi-object or read-before-complete. | Write snapshot to a temp key, then `createIfAbsent` the final epoch key atomically; readers only read the completed epoch. §6 doesn't specify atomic snapshot publish. |
| F17 | Reconcile (full scan) races a live writer mid-commit | Scan sees blob with no ref yet, no `+` yet → reclaims it. | **VIOLATION** unless reconcile also honors the early pin (F7) | Reconcile must use the *same* pin roots as the hot path; a scan that ignores pins loses in-flight uploads. (Companion's reconcile reads sessions; §6 removed them.) |
| F18 | Two writers dedup-commit the same new `H` concurrently | Both `createIfAbsent blobs/H/0` (one wins, both see present), both `+(H,0)` (distinct refs ⇒ distinct `event_id` ⇒ count 2). | **SAFE** | — |
| F19 | Writer recreates into `g+1`, leader had not yet deleted `g`; later `g` becomes count-0 | DRAIN vs RECOVER decision needed (companion has it; §6 removed the state machine). Without it, leader may delete `g` while `g`'s tombstone is the only condemn record and a *reader* is mid-`GET` of `g`. | **NEEDS-FIX** | A reader mid-GET of `g` while `g` is deleted gets a 404 → must re-LIST and read `g+1` (companion's reader fallback). §6 says "reads resolve present generation by LIST" — but a read in flight when its generation is deleted needs the retry rule stated. |

---

## 4. S3-vs-Keeper portability check {#portability}

The protocol's *data-plane* (log, snapshot, blobs, tombstones, refs, pins) uses only `createIfAbsent` /
`read` / `delete` / `list` — these map cleanly to both backends. No hidden CAS, no conditional DELETE, no
atomic rename, no ordered/total LIST is required by the data plane. Good — that part of the simplification's
portability claim holds. Specific checks:

- **Condemn is `createIfAbsent` of `<g>.tombstone`** — exactly-one-winner on both. ✓
- **Recreate is `createIfAbsent` of `<g+1>`** — different key, no CAS. ✓
- **Fold/snapshot** is PUT/GET/LIST/DELETE — but **requires a window-close barrier** (F15) so the fold LISTs a
  stable set. On Keeper this can be a linearizable counter bump; on S3 it is a fenced PUT of
  `gc/current_epoch` (which the companion has and §6 dropped from the key list — must restore conceptually).
- **Snapshot publish must be a single atomic object** (F16) — fine on both (one PUT), but §6 must say it.
- **Fence gating of DELETE** is where the asymmetry is *correctness-relevant*, not merely "awkward":
  - **Keeper:** ephemeral-sequential znode + linearizable reads ⇒ a stolen leadership is observable
    atomically by the old leader's pre-delete check; **F10/F11 are SAFE** on Keeper.
  - **S3:** the fence is a lock object + counter read with `GET`. The pre-delete fence-read gives HB-RAW only
    against a *completed* steal PUT. A paused leader resuming and deleting can race a not-yet-durable steal.
    **F11 is a real S3-only VIOLATION** unless the spec adds: (a) the fence object is read-after-write-strong
    (S3 is, for a single object), and (b) a freshly-stealing leader must **wait out the previous lease's
    maximum clock-skew-adjusted deadline before its first DELETE** (so a paused old leader either sees the new
    fence or has had its own lease provably expire). The analysis's claim that fencing is "the only
    divergence" and merely "awkward" under-states this — it is a *safety* divergence, not just ergonomic.

**Conclusion of portability check:** the four primitives suffice for the data plane, but the protocol
secretly assumes (i) a window-close barrier (an ordered/fenced epoch counter, dropped from §6) and (ii) a
fence whose steals are observable-before-delete — trivially true on Keeper, requiring an explicit
lease-out-wait rule on S3. The simplification's "identical on both backends except leadership rows" is true
for the data plane and **false for the safety of DELETE gating**.

---

## 5. Under-specified points (could not be decided as written) {#underspecified}

1. **What does the leader read at delete time?** §6 says "still 0 on a final fold." It does not say whether
   the final fold includes the open window, nor whether any directly-listed writer object is read. The entire
   P1/V1/V3/V4 analysis hinges on this. *Tested under the assumption* that the final fold folds only
   closed windows (as the companion does) — under which it is **unsafe**. If §6 instead means "LIST the whole
   log + all refs naming `H`," state it; that closes V1/V4 for the drop side but still not V3 (upload→`+`).
2. **`+` placement vs recheck.** §6 says `+` "BEFORE upload"; the companion logs `+` *after* the recheck.
   These conflict and have opposite failure modes (V2 vs V3). The doc must pick one and accept its leak/loss
   profile, or (correct answer) use two signals.
3. **No RECOVER/DRAIN/un-seal state machine.** §6 drops `gc/sealed` and the resurrection cap and says the
   "seal lifecycle is leader-local." But F8/F19 show a condemned generation that becomes referenced again
   (writer raced the seal) needs an un-seal (RECOVER) decision, and a successor leader after a crash needs to
   re-discover open tombstones. With `gc/sealed` removed, candidate re-derivation "from the compaction each
   round" only finds count-0 keys — a *recovered* (count>0) condemned generation is then invisible and its
   tombstone is immortal, *blocking all future attach to `g`* (everyone routes to `g+1`, `g+2`, … — unbounded
   generation drift, the very thing the resurrection cap guarded). **Under-specified and likely a liveness
   regression.**
4. **Grace window's exact role.** §6 says "count-0 past a grace." The companion is explicit that grace is
   *liveness-only, never a safety fence*. If §6 intends grace to provide any safety (e.g. "a writer's commit
   completes within grace"), that is a **time-based safety assumption** the backlog B32 explicitly forbids
   ("time may protect only failure detection, never live work"). Tested under "grace is liveness-only" — under
   which it provides **zero** protection against the paused-writer schedules V3/V4/F6/F7.
5. **Window-close barrier** (F15) — not in §6's object list at all (`gc/current_epoch` was dropped). Whether
   the fold LISTs a *closed* set is undefined.
6. **Atomic snapshot publish** (F16) and **signed-count fold semantics** (F14) — unspecified.

---

## 6. Top-line verdict {#verdict}

**The simplified §6 protocol is NOT correct as written.** It is a good *compression* of the bookkeeping
(dropping `active`, `gc/sealed` as a mere index, and the heavy `WriteSession` *lifecycle* are all reasonable),
and its P2 / generations core (`INV-NO-ABA`) is sound and portable. But the central simplification — **"delete
sessions; the `+` is the pin" — is unsound**, because the `+` is consumed only through a window-bounded fold
and gives the leader no fresh, edge-creating read of a writer-side object at delete time. That breaks
`INV-NO-LOSS` (P1) in at least three distinct schedules. Making the count the *sole* delete authority while
*also* removing the directly-listed pin removes the one object that made the companion's handshake chain
unsatisfiable. The companion branch is right to keep Scan-B as the load-bearing gate and to mark the
count-authoritative step DO-NOT-MERGE (B78).

### Must-fix gaps, most important first {#must-fix}

1. **(VIOLATION, P1 / INV-NO-LOSS) The `+`-as-pin does not pin.** Re-introduce an *early, directly-listed*
   pin object the leader reads with a fresh LIST immediately before every delete — e.g.
   `createIfAbsent pin/<H>/<writer-uuid>` raised *before* upload and deleted *after* ref publish. This is "a
   session by another name" and brings back **lease-based reaping of crashed writers' pins** (else
   `INV-NO-LEAK-FOREVER` fails). The doc's claim "no sessions" is therefore not achievable while keeping P1
   safe; at best it can shed the *lease/delta_epochs/sticky-fail* machinery, not the pin object itself.
   (Closes V3/V4/F6/F7/F17.)
2. **(VIOLATION) Final-fold/condemn must read the open window and all refs naming `H`, and every DELETE must
   be fence-re-checked.** Restore the window-close barrier (a fenced epoch counter, the dropped
   `gc/current_epoch`) so the fold LISTs a stable closed set (F15), and make the pre-delete gate a fresh
   whole-log + ref LIST under a fresh fence read (F4/F10). §6's "final fold" is under-specified into
   unsafety. (Closes V1/F4/F10/F15.)
3. **(VIOLATION, S3-only) Fence steals must be observable-before-delete on S3.** Add an explicit
   lease-out-wait rule: a freshly-stealing leader must not issue its first DELETE until the previous lease's
   skew-adjusted deadline has passed, and every DELETE re-reads the read-after-write-strong fence object.
   Keeper is safe by construction. The analysis under-states this as merely "awkward." (Closes F11.)
4. **(LEAK / liveness regression) Restore an un-seal (RECOVER) path and bound generation drift.** Dropping
   `gc/sealed` and the resurrection cap leaves recovered-but-condemned generations with immortal tombstones
   that force unbounded `g`-drift and are invisible to count-0 candidate re-derivation. Either keep a minimal
   open-tombstone index (it need not be the full `gc/sealed` design) or define how a successor leader
   re-discovers and un-seals condemned-but-now-referenced generations. (Closes F8/F19 and §5.3.)
5. **(LEAK) Resurrection leaks the abandoned generation under the count.** A writer that resurrects from `g`
   to `g+1` logged `+(H,g)` (if `+` is early) with no matching `-`, pinning `blobs/H/g` forever in the count.
   Either log a compensating `-(H,g)` on resurrect-away, or make `reconcile` mandatory (not default-off) and
   stop claiming the resurrection path is leak-free. (Closes V2/F5.)
6. **(SAFE-but-must-state) Spell out the fold's required guarantees:** `event_id` dedup of retried appends
   (F12), atomic single-object snapshot publish (F16), and signed/clamped running counts for cross-window
   split `+`/`-` (F14). These are individually easy but absent from §6.

### What the simplification gets right (keep) {#keep}

- **Generations + `<g>.tombstone` for P2** (`INV-NO-ABA`) — sound, portable, no CAS. The §8 open decision
  should resolve to *generations*, not a same-key delete-marker, for exactly the reason §4 of the analysis
  gives.
- **Dropping the `active` hint** in favor of `LIST blobs/<H>/` to resolve the present generation — correct;
  removes a lie-prone best-effort PUT with no safety cost (`INV-NO-DANGLE` survives on strongly-consistent
  LIST).
- **Demoting the full bucket scan to `reconcile`-only** is the right *shape* — but reconcile must be
  **mandatory backstop** (not default-off) given fixes #1 and #5, and must honor the same pin roots (F17).
- **The single `Coordination` interface** (four primitives + leader/fence) is the right abstraction; just
  recognize that DELETE-gating safety is *not* backend-symmetric (#3).

**Bottom line for a spec:** the analysis is a good *demolition* of incidental bookkeeping but over-reaches in
demolishing the *load-bearing* pin. A correct page-sized protocol is: **log + snapshot (count) for candidates
and liveness; a tiny lease-reaped `pin/<H>/<uuid>` (directly listed, read fresh at delete) for P1; generations
+ tombstone for P2; a fenced epoch-close barrier + fence-re-checked deletes for the leader.** That is one
object more than §6 wants, and it is the object §6 cannot do without.
