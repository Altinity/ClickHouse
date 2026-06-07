---
description: 'Adversarial distributed-systems + object-storage correctness review of the Keeper-only coordination profile for the content-addressed (CA) MergeTree EBR garbage collector. Tests the profile claims (Keeper holds only O(active-writers) ephemeral state; S3-COMPLETE; Keeper-linearizability deletes the S3-only machinery; the safety hinge is "writer self-fences on session loss" with no clock-skew assumption; sharded fold is O(delta)) against the real Keeper/ZooKeeper session model (a partitioned client may not know its session expired) and the hard fact that Keeper linearizability does NOT extend across to S3. Re-derives the Lamport model, runs a failure-injection catalog, and delivers ordered must-fix gaps.'
sidebar_label: 'CA Keeper-only GC profile — correctness review'
sidebar_position: 23
slug: /superpowers/reports/ca-gc-keeper-only-review
title: 'Adversarial Correctness Review — CA GC Keeper-only Coordination Profile'
doc_type: 'reference'
---

# Adversarial Correctness Review — CA GC Keeper-only Coordination Profile {#review}

Panel: (1) a distributed-systems correctness specialist (Lamport happens-before, linearizability, EBR/QSBR/Crossbeam,
ZooKeeper/Keeper session semantics, epoch reclamation); (2) an object-storage (Amazon S3 / MinIO) consistency expert.
Subject under test: `docs/superpowers/reports/2026-06-07-ca-gc-keeper-only-profile.md` — the "Keeper-only" coordination
profile of the EBR GC, which claims Keeper's linearizability lets us *delete* the S3-only coordination machinery
(`2·Δ_skew` time-lease, `create-if-absent` fence counter, strong-read fence + steal-out-wait, closed-epoch S3 barrier)
and reduce the safety hinge to "writer self-fences on session loss" with **no clock-skew assumption**.

This review is adversarial. The job is to find the schedules that lose data, strand a ref, leak forever, or violate
INV-S3-COMPLETE — not to bless it.

**Top-line spoiler.** The profile is **correct in spirit and wrong in two load-bearing places, both data-loss
VIOLATIONS.** Keeper genuinely removes *Keeper-internal* clock skew (the lease/renewal/fence-steal hazards of the
S3-only mode — E3/E4/E12 of the prior review — really do collapse). **But the profile makes two unsound jumps:**

1. **It treats "Keeper linearizable" as if it ordered S3 operations. It does not.** The flush-`+`-then-advance
   coupling now *spans two systems*: the `+` is durable in **S3**, the `O_W` advance is published in **Keeper**.
   Keeper linearizability orders the `O_W` write against other Keeper writes — it says **nothing** about whether the
   writer's S3 `+` is durable/visible. The closed-epoch S3 barrier the profile *deletes* in §3 was doing real work in
   S3, and Keeper cannot replace it. This re-opens **V-EBR-1 / V-EBR-2 verbatim** (**VIOLATION**, the must-fix #1 of
   the base review reappears — the profile silently drops the very fix the prior review demanded).
2. **Claim (d) "no clock-skew assumption" is false. The assumption is not removed — it is *relabeled* into a
   session-timeout-vs-client-awareness timing assumption.** A partitioned writer whose session the *server* already
   expired (its `writers/<S>` gone → leader computes `safe_epoch` without it → reclaims) does **not yet know** it is
   expired: it sees only `Disconnected`, and its in-flight S3 commit can land *after* the leader deleted the blob it
   reuses. The hinge "ops start failing so the writer knows" is true only **after the client re-contacts a server**;
   during the partition the client's S3 ops do **not** go through Keeper and do **not** fail. Safety therefore
   requires a rule the profile does not state — **fail-stop on `Disconnected`, not on confirmed `Expired`** — and even
   that only bounds the window, it does not eliminate it for already-issued-but-not-durable S3 writes
   (**VIOLATION**).

INV-S3-COMPLETE (claim (b)) is **almost** achieved but has one real gap (epoch monotonicity across an empty-Keeper
restore — E15-K) and one under-specified gap (what is a "root" for reconcile when Keeper is freshly wiped and no
leases exist yet). Claims (a) and (e) hold. Details, timelines, and minimal fixes below.

---

## 1. Model (Lamport) {#model}

### 1.1 Durable state vs ephemeral state {#state}

Per the profile §2 (keys wrapped as code):

- **S3 (durable, the only ground truth):** `epoch/current` (monotone `E_cur`, written by the fence-holder),
  `blobs/<H>/<e>`, `parts/<id>/<e>`, `blobs/<H>/<e>.tombstone`, `active/<H>`, `store/.../refs/<part>`,
  `log/<e>/<shard>` (`+`/`-` deltas), `snap/<e>/<shard>` (folded counts).
- **Keeper (ephemeral, O(active writers)):** the leader-election znode (ephemeral-sequential; fence = sequence
  number), and `writers/<S> = O_W` (one ephemeral session node per live writer carrying its observed epoch).
- **Per-actor local state:** a writer's cached `O_W`, its S3 in-flight ops, and — critically — **its local belief
  about whether its Keeper session is still valid** (which can be stale during a partition).

### 1.2 Actors {#actors}

- **Writers `W1..Wn`** — each = a Keeper session `S` + a cached `O_W` + S3 write activity. Can pause unboundedly
  (VM freeze, STW GC pause) and can be *partitioned* from Keeper while still reachable to S3 (independent network
  paths — the case the profile must survive).
- **GC leaders `L`** — the holder of the lowest-sequence election znode. Others contend.
- **Readers `R`** — `GET active/<H>` → `GET blobs/<H>/<e>`; on `404`, `LIST blobs/<H>/`.
- **Recovery procedure** — runs on (possibly empty) Keeper restore: re-elect, bump `epoch/current` once, resume.
- **Reconcile** — the off-hot-path full S3 reachability scan that rebuilds `snap` from `refs/`.

### 1.3 The five happens-before sources (and the one that does NOT exist) {#hb}

- **HB-KEEPER (linearizable writes, *within Keeper*).** All Keeper writes are totally ordered; a `getChildren` /
  `getData` *that the client issues to a server and the server answers* reflects a linearization point. Caveat:
  ZooKeeper/Keeper **reads are sequentially consistent, not linearizable** — a client may read **stale** state
  unless it issues `sync` first. The leader's `getChildren(writers/)` is a read; without `sync` it may miss a write
  that linearized just before. (Usually benign here — see §3.3 — but must be stated.)
- **HB-RAW-S3 (read-after-write, *within S3*).** A `GET/HEAD/LIST` that *starts after* an S3 `PUT`/`createIfAbsent`
  *completed* observes it. An in-flight, not-yet-acked S3 op is invisible and may land arbitrarily later.
- **HB-CIA-S3.** All `createIfAbsent K` for the same S3 key are totally ordered; one wins.
- **HB-SESSION.** A Keeper session is expired **by the server** after the timeout; that expiry deletes the session's
  ephemerals. The expiry is a *server-side* event. **A partitioned client learns of it only when it re-contacts a
  server** — until then it observes `Disconnected` and *believes its session may still be alive*. The transition
  `Connected → Disconnected → (reconnect ? Connected : Expired)` means **`Disconnected` is the only signal a
  partitioned client has, and it is ambiguous** (could be a 50 ms blip or a dead session).
- **HB-SEQ.** Leadership = lowest-sequence ephemeral child; this is a Keeper-internal order (HB-KEEPER), so it is
  linearizable *for actors that can read Keeper*. A **partitioned** old leader cannot read the new election and still
  believes it leads.

**THE EDGE THAT DOES NOT EXIST — HB-CROSS.** *There is no happens-before edge between a Keeper operation and an S3
operation.* Keeper linearizing the `O_W` publish does **not** order it against the writer's S3 `+` PUT, the leader's
S3 fold LIST, or the leader's S3 DELETE. **This is the single most important fact the profile gets wrong by
omission.** Every place the profile says "Keeper makes X exact / linearizable / a clean snapshot," X must be checked
for whether the *S3* side of the coupling is ordered. It usually is not.

### 1.4 Invariants {#invariants}

- **INV-NO-LOSS** — no `blobs/<H>/<e>` is `DELETE`d while reachable from a live ref or while any correct future
  fold/pin-read must still count a reference to it.
- **INV-NO-DANGLE** — every published `refs/<part>` resolves to present bytes (reads tolerate stale `active` via LIST).
- **INV-NO-ABA** — `DELETE blobs/<H>/<e_a>` never destroys bytes a writer is creating under the same key.
- **INV-OVER-COUNT-ONLY** — every failure mode biases to over-count (leak), never under-count (loss).
- **INV-S3-COMPLETE** — S3 alone determines and rebuilds the full state; losing Keeper (even wiping it) loses no
  durable state.

---

## 2. The make-or-break hinge — session expiry vs client awareness {#hinge}

The profile §3/§4 reduces the entire safety story to:

> a writer self-fences (goes read-only) the instant its Keeper session is lost — and it *knows*, because its Keeper
> ops start failing. No clock-skew assumption survives.

This is the claim to break. **It conflates two events that are not simultaneous:**

- `t_expire` (server side): the leader's quorum expires session `S`; `writers/<S>` is deleted; from this instant the
  leader's `getChildren(writers/)` will not see `S`, so `safe_epoch` is computed *without* `W`.
- `t_aware` (client side): `W` re-contacts a server, learns its session is `Expired`, and self-fences.

Between `t_expire` and `t_aware`, `W` is partitioned. It sees `Disconnected`. **Its S3 path is independent and still
works.** So `W` can issue (or have in flight) an S3 `+`/`ref`/dedup-reuse that targets a blob the leader is about to
reclaim *because it already dropped `W` from the live set*.

```
   real time ───────────────────────────────────────────────────────────────────────────►
   WRITER W (session S, O_W = e_a, decided to dedup-reuse (H,e_a))     LEADER L (fence = lowest seq)
   ───────────────────────────────────────────────────────────────    ─────────────────────────────
   W.head S3: blobs/H/<e_a>.tombstone ABSENT  (decides reuse)
   <network partition W↔Keeper begins; W↔S3 still up>
                                                                       (heartbeat to S maps to nothing;
                                                                        quorum expires S at t_expire;
                                                                        writers/<S> ephemeral deleted)
   W sees Disconnected (NOT Expired) — believes it may still be alive
   W issues S3 + and ref for (H,e_a)  ← Keeper not consulted; S3 up    R3 QUIESCE getChildren(writers/) — S gone
                                                                       safe_epoch := min(O_W) over survivors > e_a
                                                                       R4 RECLAIM: E_cur>=e_a+2, safe_epoch>e_a,
                                                                          unreferenced(fold), stillLeader
                                                                          DELETE blobs/H/<e_a>      ←── LOSS
   W's ref -> (H,e_a) is now durable, names a deleted blob  ←── DANGLE
   <partition heals> W learns Expired -> goes read-only      (too late; the bytes are gone)
```

**The hinge does NOT fire in time.** "Ops start failing" is a property of the *Keeper* path; the writer's *S3* path
never failed. The profile's claim that the writer "knows" is true only at `t_aware`, and the dangerous S3 writes
happen in `[t_expire, t_aware]` — a window whose width is governed by the **session timeout plus the partition
duration plus in-flight S3 latency**, none of which the profile bounds.

### 2.1 The exact rule the profile must state (and does not) {#hinge-rule}

To make the hinge sound, a writer must **fail-stop on `Disconnected`, not on confirmed `Expired`**, and it must do so
**conservatively early enough that no S3 commit it issues can outlive the server-side `t_expire`.** Precisely:

> A writer may perform a *consequential* S3 op (publish `ref`, dedup-reuse an existing blob, advance `O_W`) only while
> it is **`Connected` to Keeper AND within a self-imposed local deadline `t_last_confirmed_contact + (T_session −
> Δ_margin)`**. On `Disconnected`, it must immediately stop issuing new consequential S3 ops AND must not *trust*
> any consequential S3 op whose durability it cannot confirm completed before the deadline.

This is **structurally identical to the S3-only `2·Δ_skew` self-fence** — it is a *local-clock deadline relative to a
timeout*, fenced before the coordinator could possibly consider the writer dead. **Keeper has not removed the timing
assumption; it has moved it from "clock skew between writer and GC" to "the writer's local elapsed-time estimate of
how long it has been since it last confirmed a live session, vs the server's session timeout."** That is a *weaker
and cleaner* assumption (one timeout vs two clocks, and the keepalive is the renewal), but it is **emphatically not
"no timing assumption."** See §6 claim (d).

**Additional sharp edge: in-flight S3 ops issued while `Connected` but not yet durable at `t_expire`.** Even a writer
that fail-stops perfectly on `Disconnected` may have an S3 `+`/`ref` PUT *already issued and acked-pending* when the
partition starts. That PUT can land after the leader's reclaim. So the rule above is necessary but **not sufficient**
on its own — it must be paired with the cross-system fix of §3 (the leader must not reclaim until the writer's S3 side
is provably durable+folded, which the closed-epoch S3 barrier provided and Keeper does not).

---

## 3. Cross-system ordering — Keeper `O_W` vs S3 `+` {#cross}

### 3.1 V-K1 — the deleted closed-epoch S3 barrier re-opens V-EBR-1/V-EBR-2 — **VIOLATION** {#v-k1}

The profile §3 table explicitly *deletes* the "closed-epoch fold S3 barrier + the `Δ_skew` math" and replaces it
with: "the leader reads a linearizable snapshot of `{O_W}` via `getChildren(writers/)`; quiescence is exact." This
is the unsound jump. The base review's **must-fix #1** required *two* couplings:

(1) fold only a **closed** `log/<e_a>` (a fenced window-close barrier *in S3*, because `log` is in S3), and
(2) a writer may not advance `O_W` past `e` until every `+` it issued at `≤ e` is **durable in S3 and folded**.

Keeper's linearizable `{O_W}` snapshot addresses **neither**. `getChildren(writers/)` tells the leader the set of
observed epochs; it does **not** tell the leader whether a given writer's **S3** `+` is durable, nor does it close the
S3 `log/<e_a>` against late appends. The profile's own §4 protocol line — "advance [`O_W`] only after its `+`s are
durable+folded (the plan's flush-`+`-then-advance rule)" — *names* the rule but then asserts Keeper enforces it
("Keeper makes 'no live writer is still in epoch e' a linearizable read"). **It does not.** Keeper makes the *epoch
observation* linearizable; the *S3 `+` durability* is on the far side of HB-CROSS, unordered.

Concrete loss schedule (V-EBR-1, restated for Keeper):

```
   WRITER W (session S alive throughout, O_W = e_a)              LEADER L
   ────────────────────────────────────────────                ────────
   W.head S3: tombstone(e_a) ABSENT (decide reuse (H,e_a))
   W issues S3 + (H,e_a)  -- ack lost / retried / MinIO buffer; NOT yet durable
   W ref -> (H,e_a)        (durable in S3)
   W advances O_W := e_a+1 in writers/<S>  (Keeper write, linearizes fine)
                                                                R1 FOLD closed epochs; LIST log/<=e_a+1>
                                                                   W's + still not durable -> count(H,e_a)==0
                                                                R3 QUIESCE getChildren(writers/): W is e_a+1
                                                                   safe_epoch > e_a  ✓  (W "passed" — Keeper agrees)
                                                                R4 RECLAIM E_cur>=e_a+2 ✓, unreferenced(fold) ✓,
                                                                   stillLeader ✓  DELETE blobs/H/<e_a>  ←── LOSS
   W's + finally lands (too late; bytes gone; W's ref dangles)
```

Keeper did exactly what it promised — it gave a perfectly linearizable `{O_W}` showing `W` at `e_a+1`. The bug is
that **`O_W = e_a+1` in Keeper does not imply `W`'s `e_a`-era S3 `+` is durable**, because nothing ordered the Keeper
write after the S3 write. The profile deleted the only mechanism that did (the closed-epoch S3 barrier + `+`-durable
coupling). **This is the same V-EBR-1, undiminished.**

The 100 GB / long-op variant (V-EBR-2) re-opens identically: the profile wants `O_W` to "advance freely" during a
long upload; if it advances past `e` before the decided dep's `+` is S3-durable+folded, `safe_epoch > e` opens and
the dep is deleted mid-upload.

### 3.2 The exact ordering the writer must guarantee {#cross-rule}

> A writer may publish `O_W = e+1` **into Keeper** only after it has confirmed (S3 read-after-write, or an S3 write
> primitive that returns durability) that **every `+` it appended into `log/<≤e>` is durable in S3**, AND the leader
> must fold only a **closed** `log/<e>` (a fenced close marker in S3 that writers honor — `epoch/closed=e`).

**Does Keeper buy anything here at all?** Marginally yes, but not what the profile claims:
- Keeper makes the *publication* of `O_W` linearizable and makes a dead writer's `O_W` *disappear* atomically (no
  `lease_until` arithmetic) — that is genuinely cleaner for the **quiescence read**.
- But the **flush-`+`-then-advance** rule is a purely **S3-durability** obligation on the writer, enforced by the
  *writer ordering its own ops* (S3-`+`-durable **before** Keeper-`O_W`-write), not by Keeper. Keeper cannot observe
  S3 durability. **So the closed-epoch S3 barrier cannot be deleted; it must be retained in the Keeper profile.** The
  profile's §3 claim that Keeper "collapses" it is the error.

### 3.3 Stale-read nuance on the quiescence read {#cross-stale}

Even the quiescence read needs care: ZooKeeper/Keeper reads are sequentially consistent. A leader doing
`getChildren(writers/)` without a preceding `sync` may read a **stale** membership — specifically it might **miss a
brand-new writer** that just registered. As the base review's E19 showed, missing a *high* `O_W` newcomer cannot
*lower* `min`, so it cannot cause an unsafe reclaim; it is over-count-safe. **But the leader must still `sync`
before the quiescence read** if it wants the "no live writer ⇒ `safe_epoch := E_cur`" branch to be sound, because
that branch *aggressively* sets `safe_epoch` high — a stale read that misses *all* surviving writers would wrongly
take the empty branch. **Must-fix: the `safe_epoch := E_cur` (empty) branch requires a `sync`-ed, linearizable
membership read; the profile says "linearizable" but Keeper reads are not, by default.**

---

## 4. Leader split-brain under partition {#splitbrain}

The profile §4 R4 gate includes "I am still the lowest-seq leader (re-check F)" and §3 claims leadership is
"linearizable, no visibility race." Attack: an **old leader partitioned from Keeper** still believes it holds the
lowest-seq node (its local belief is stale; HB-SEQ requires reading Keeper, which it cannot).

```
   OLD LEADER L1 (seq=5, partitioned from Keeper)          NEW LEADER L2 (seq=6 after L1's ephemeral expired)
   ──────────────────────────────────────────────         ────────────────────────────────────────────────
   <partition L1↔Keeper; L1↔S3 still up>
   (quorum expires L1's ephemeral seq=5;
    L2 becomes lowest-seq leader)
   L1 "re-check F": reads its LOCAL cache / a stale         L2 folds, condemns, and legitimately spares
   Keeper read it cannot complete -> believes seq=5            (H,e_a) because a fresh + arrived
   L1 R4 RECLAIM: DELETE blobs/H/<e_a>  ←── deletes          ...
   under a fence it no longer holds; split-brain delete
```

The "re-check I am still lowest-seq" only works **if the re-check is a fresh Keeper read that succeeds**. A
partitioned L1 cannot do a fresh read; if it falls back to a cached/last-known leadership belief, it deletes under a
revoked fence. **Same root as §2: the leader must fail-stop on `Disconnected`** — it must not perform an S3 DELETE
unless it has *just* confirmed (a successful, `sync`-ed Keeper read) that it still holds the lowest sequence. On
Keeper this is clean *because the re-check is a real Keeper op that fails under partition* — **but only if the leader
treats a failed/stale re-check as "I am not leader" (fail-close), which the profile does not state.** If the leader's
re-check can succeed against a stale local view, this is a VIOLATION. With the explicit fail-close-on-disconnect rule,
**E12 is SAFE on Keeper** (this is the one place Keeper is genuinely cleaner than S3 — no out-wait needed, because the
ephemeral expiry is the single arbiter and the re-check is a live linearizable op). Verdict: **SAFE iff the
fail-close-on-`Disconnected` rule is stated; VIOLATION as written (rule omitted).**

---

## 5. Recovery / INV-S3-COMPLETE {#recovery}

### 5.1 The one-time epoch bump on restore — V-K2 (epoch monotonicity) {#v-k2}

Profile §5.2: "It reads `epoch/current` from S3 and bumps it once." `epoch/current` is in **S3**, so this is sound
**provided `epoch/current` survived** — which it does (S3 is durable). **But the base review's E15 (epoch
monotonicity) is *more* dangerous in the Keeper-only profile, not less**, because of the "restored-from-stale-backup"
case the profile does not address:

- **Empty-Keeper restore:** `epoch/current` is read from S3, bumped once. Monotone. **SAFE** — and this is the
  profile's strongest point: because the epoch is in S3, not Keeper, wiping Keeper cannot reset the epoch. Good.
- **Keeper-restored-from-stale-backup:** irrelevant for the epoch (epoch is in S3, Keeper holds none). **SAFE** —
  confirms the design choice pays off. The danger would only arise if the epoch counter were ever cached in or
  derived from Keeper; it is not. Confirm and *state* that `epoch/current` is read with S3 read-after-write (a fresh
  GET, not a cached value) on every restore.

So V-K2 is **SAFE given the stated design**, and is actually a *vindication* of INV-S3-COMPLETE — **but only because
the epoch lives in S3.** The must-fix is defensive: **state explicitly that no actor ever caches `epoch/current` in
Keeper or derives the next epoch from any Keeper sequence number** (a tempting "optimization" that would silently
break E15 on Keeper wipe). The fence (seq number) and the epoch must remain *independent* counters.

### 5.2 Can recovery/reconcile delete a blob a partitioned-but-alive pre-outage writer is about to reference? {#recovery-writer}

This is the cross-outage writer trace, and it is **the deepest hole in INV-S3-COMPLETE.** Consider a writer `W` that
*survives across* a total Keeper outage:

```
   WRITER W (decided reuse (H,e_a) BEFORE the outage; S3 + issued, not yet durable)
   ─────────────────────────────────────────────────────────────────────────────
   <total Keeper loss>                       writers/<S> gone (Keeper down); W sees Disconnected
   W still reachable to S3; its in-flight + / ref to (H,e_a) is still landing
   <Keeper restored empty>                   GC re-elects; bumps epoch; reconcile runs to rebuild snap
   reconcile: full reachability scan over refs/ -> if W's ref not yet durable, (H,e_a) looks unreferenced
   reconcile reclaims (H,e_a)  ←── LOSS, if reconcile's root set excludes the in-flight writer
   W's ref -> (H,e_a) lands -> DANGLE
```

The profile §5.1 claims "during the outage every writer goes read-only and aborts in-flight commit." **This is the
§2 fallacy again:** a *partitioned* writer does not know Keeper is down (it sees `Disconnected`), and its **S3 ops do
not abort just because Keeper is unreachable** — aborting an in-flight S3 multipart or `createIfAbsent` is a local
decision the writer must make, and it can only make it *after* it notices `Disconnected` and *if* the rule of §2.1 is
in force. An S3 `+`/`ref` already acked-pending will still land. **So "the system is paused but safe" is only true if
every writer fail-stops on `Disconnected` AND no consequential S3 op was in flight at outage start.** The second
condition is not guaranteed.

**What is a "root" for reconcile when Keeper was just wiped (no leases exist yet)?** The base review (E18) made
reconcile honor "live leases, live `+`s, and the retention window" as roots. **After a Keeper wipe there are no live
leases** — the entire `writers/` set is empty until writers re-register. So reconcile's lease-root set is *empty* at
exactly the moment it most needs it. The only surviving roots are **S3-side**: live `refs/`, durable `+` deltas in
`log/`, and the **time-retention backstop** (never delete an object newer than `now − retention`). **Must-fix: after
a Keeper restore, reconcile MUST NOT run until either (a) a quiescence grace ≥ the max in-flight S3 op duration has
elapsed so any pre-outage in-flight `+`/`ref` is durable, or (b) it relies solely on the time-retention backstop with
`retention > max op duration`.** The profile's §5.3 says "reconcile rebuilds from S3 ground truth... refs are
durable and written-last, so S3 reachability is the authority" — **this is unsafe for a ref that is in flight across
the outage**, because "written-last" does not mean "already durable." The retention backstop (from the base plan
§10) is **load-bearing here and the profile drops all mention of it.**

### 5.3 Reconcile racing live writers (scan-window race) {#reconcile-race}

Even in steady state (not just post-outage), reconcile's full S3 reachability scan races a concurrent ref publish:
the scan reads `refs/` (sees no ref for the part yet), the writer then publishes the ref naming `(H,e)`, reconcile
concludes `(H,e)` unreferenced and reclaims it. The grace/root set that makes it safe is exactly the base review's
E18 fix: **reconcile honors the time-retention backstop (never delete newer than `now − retention`, `retention >
max op`) and the live `+`/lease roots.** The Keeper profile inherits this requirement unchanged; it must not be
dropped. **Under-specified in the profile → tested under "no retention backstop" → VIOLATION; under "retention
backstop retained" → SAFE.**

### 5.4 `active/<H>` cross-epoch + new-writer registration + epoch monotonicity {#misc}

- **`active/<H>` cross-epoch:** unchanged from the base review §8 — SAFE conditional on INV-NO-LOSS and the mandatory
  unbounded-generation `404 → LIST` retry. Keeper changes nothing here (`active` is S3-only).
- **New-writer registration lowering `safe_epoch`:** a new writer reads `epoch/current = E_cur` and registers
  `writers/<S> = E_cur > e_a` for any reclaimable `e_a`, so it cannot lower `safe_epoch` below a reclaimable epoch.
  SAFE by construction (base review §7 / E19), **provided** a re-registering writer (after session loss) re-reads
  `epoch/current` from S3 rather than reusing a stale cached `O_W`. **Must state this for Keeper explicitly: on
  session re-establishment the writer's old `O_W` is void; re-read S3.**
- **Epoch monotonicity across recovery:** see §5.1 — SAFE because the epoch is in S3, with the defensive must-fix that
  it is never derived from a Keeper sequence number.

---

## 6. Failure-injection catalog {#catalog}

Verdicts: **SAFE** / **VIOLATION** (loss or permanent dangle) / **LIVENESS** (liveness-only stall) / **NEEDS-FIX**
(safe only with a stated rule the profile omits). "K" suffix marks Keeper-profile-specific.

| # | Injected fault | Trace (abbrev.) | Verdict | Minimal fix |
|---|---|---|---|---|
| K1 | Writer partitioned from Keeper; server expires session `S`; `writers/<S>` gone; writer (seeing only `Disconnected`) issues S3 dedup-reuse `+`/`ref` for `(H,e_a)` | leader drops `S` from live set → `safe_epoch>e_a` → DELETE; writer's ref dangles | **VIOLATION** | Fail-stop on `Disconnected` with a local deadline `< T_session`; pair with the §3 closed-epoch S3 barrier for in-flight ops |
| K2 | Writer GC-pauses past `T_session`, then resumes and commits before noticing `Expired` | identical to K1: server already expired `S`; resumed writer commits reuse of reclaimed blob | **VIOLATION** | Same as K1 — the self-fence must be a *local elapsed-time* deadline checked before every consequential op, not "wait for `Expired`" |
| K3 | `+` issued to S3 but not yet durable; writer advances `O_W` in Keeper; leader folds (count 0), reclaims | V-K1: Keeper `O_W` advance does not order the S3 `+`; deleted under a live ref | **VIOLATION** | Retain closed-epoch S3 barrier; advance `O_W` only after S3 `+` is read-back-durable (flush-`+`-then-advance, ordered by the writer) |
| K4 | Long 100 GB op: decide reuse `(H,e)`, advance `O_W` "freely" before `+(H,e)` durable, upload for hours | V-EBR-2: leader condemns+deletes `(H,e)` mid-upload | **VIOLATION** | Hold `O_W` at `e` until `+(H,e)` S3-durable+folded (sub-second if flushed before the long upload) |
| K5 | Leader partitioned from Keeper; new leader elected; old leader's "re-check F" uses a stale/cached view; both DELETE | split-brain delete under a revoked fence | **VIOLATION** (as written) / **SAFE** (with rule) | Fail-close: a DELETE requires a *fresh, `sync`-ed, successful* Keeper read confirming lowest-seq; failed/stale re-check ⇒ "not leader" |
| K6 | Total Keeper loss mid-round (between any two protocol steps) | leader loses election node → GC stops; tombstones/epoch are S3-durable & idempotent; successor re-folds on restore | **SAFE** | State that all GC mutations are idempotent and a post-restore successor re-FOLDs under a fresh fence before any DELETE |
| K7 | Total Keeper loss while a pre-outage writer has an in-flight S3 `+`/`ref` to `(H,e_a)` | post-restore reconcile sees `(H,e_a)` unreferenced (ref not yet durable) and reclaims | **VIOLATION** unless gated | Post-restore reconcile must wait a grace ≥ max in-flight op duration, or rely on the retention backstop (`retention > max op`) |
| K8 | Keeper restored **empty** | epoch read from S3 + bumped once; writers re-register reading fresh `E_cur`; monotone | **SAFE** | State: `epoch/current` read with fresh S3 GET; never derive epoch from a Keeper seq |
| K9 | Keeper restored **from a stale backup** (old leader-election/writer znodes resurrected) | resurrected ephemerals are *stale durable data* in Keeper — a "ghost" leader znode or ghost `writers/<S>` | **VIOLATION** unless purged | On restore from any persistent Keeper snapshot, the GC namespace (election + `writers/`) MUST be treated as empty / purged; ephemerals from a backup are invalid. The profile assumes ephemerals vanish — a *backup* restore violates that. State: refuse to honor pre-restore session/leader znodes |
| K10 | S3 DELETE duplicated/delayed (replayed after a resurrection created `(H,E_cur)`) | DELETE is generation-keyed (`<e_a>` vs `<E_cur>`); replay hits the old key, no-op | **SAFE** | Tombstone must persist long enough that no same-`e` recreate occurs (INV-NO-ABA; base E14) |
| K11 | S3 DELETE delayed and lands after the leader lost leadership | DELETE has no HB edge; but it targets an already-condemned, quiescent `(H,e_a)` — still safe under INV-NO-LOSS for that epoch | **SAFE** *iff* the condemn/quiesce decision that authorized it was valid (i.e. K5 fixed) | Couple every DELETE to a fresh fence re-check (K5) so a delayed DELETE was authorized under a still-valid fence |
| K12 | Multipart 100 GB upload crashes mid-flight | object invisible until `CompleteMultipartUpload`; never a blob; S3 lifecycle abort-incomplete reclaims parts | **SAFE** | Lifecycle abort-incomplete-multipart rule is required infra |
| K13 | `snap/<e>/<shard>` torn (partial write) | a folder reading a partial snap under-counts → premature condemn | **VIOLATION** unless atomic | Single-object atomic publish (temp key + `createIfAbsent` final) + version/checksum; read only completed shards (base E16) |
| K14 | Reconcile races a live ref publish (steady state) | scan sees no ref → reclaims a blob the writer just referenced | **VIOLATION** unless retention backstop | `retention > max op`; honor live `+`/refs roots; never delete newer than `now − retention` (base E18) |
| K15 | Leader's `getChildren(writers/)` reads stale (no `sync`), missing all survivors → takes empty branch `safe_epoch:=E_cur` | aggressively high `safe_epoch` → premature reclaim | **VIOLATION** unless `sync`ed | The empty/`safe_epoch:=E_cur` branch requires a linearizable (`sync`-ed) membership read; sequential-consistent read insufficient |
| K16 | Writer session flaps (`Disconnected`→`Connected` within timeout, session survives) | server did NOT expire `S`; `writers/<S>` intact; `O_W` preserved; writer resumes | **SAFE** (LIVENESS at most) | None — this is the benign case; but it is *indistinguishable to the client* from K1/K2, which is why fail-stop-on-`Disconnected` is mandatory |
| K17 | New writer registers `writers/<S> {O_W=E_cur}`; leader's quiescence read misses it (stale read) | omitting a *high* `O_W` cannot lower `min` below a survivor; the newcomer itself observed past all reclaimable epochs | **SAFE** | Re-registering writer re-reads `epoch/current`; (and K15 for the empty branch) |
| K18 | Writer pauses between confirming `Connected`/deadline and the S3 `ref` PUT ack | self-fence passed; pause unbounded; ref lands after leader reclaimed | **NEEDS-FIX** | Bound the ref-PUT within the deadline margin, or re-confirm session + re-check tombstone after a long stall before trusting the commit |
| K19 | Resurrection-away from condemned `(H,e_a)` leaves a `+(H,e_a)` with no matching `-` | `+` with no `-` pins `(H,e_a)` in the count forever | **LIVENESS / LEAK** | Log a compensating `-(H,e_a)` on resurrect-away (base R2 leak fix); reconcile as backstop |
| K20 | Writer paused with a *live* session (keepalive still succeeding) across a rotation | `safe_epoch` pins at `O_W`; reclamation of `>O_W` stalls | **LIVENESS** | None — EBR working as designed (pause ⇒ liveness stall, not loss). The cleanest Keeper win |
| K21 | Two writers concurrently resurrect condemned `(H,e_a)` → both create `blobs/<H>/<E_cur>` | `createIfAbsent` one-winner; both reuse `E_cur` | **SAFE** | — |

---

## 7. Point-by-point verdict on the profile's claims (a)–(e) {#claims}

### (a) "Keeper holds only O(active-writers) ephemeral coordination, no durable state." {#claim-a}

**SAFE — holds.** Keeper = `{election znode, writers/<S> = O_W}`, both ephemeral, sized O(active writers). No
per-object, per-epoch, or per-tombstone state in Keeper. Confirmed. **One defensive caveat (K9):** if Keeper is ever
restored from a *persistent backup/snapshot*, stale leader/`writers` znodes can resurrect as "ghost durable state"
that violates the ephemeral assumption — the profile must state that a restored Keeper's GC namespace is treated as
empty. With that caveat, (a) holds, and it is the profile's genuine strength.

### (b) "S3-COMPLETE: total Keeper loss loses no durable state; full state recoverable from S3 alone." {#claim-b}

**MOSTLY SAFE, with one real gap and one defensive gap.** The architecture is right: every durable fact
(`epoch/current`, blobs, manifests, refs, tombstones, `log`, `snap`) is in S3, and `snap` is a rebuildable cache of
`refs/` reachability. The epoch surviving in S3 (not Keeper) is exactly what makes a Keeper wipe non-destructive
(K8). **But:**

- **Gap 1 (VIOLATION — K7/§5.2):** a writer with an *in-flight S3 op across the outage* can have its blob reclaimed
  by post-restore reconcile, because "recoverable from S3 alone" assumes all durable facts are *already durable*, and
  an in-flight ref is not. The fix (post-restore reconcile grace / retention backstop) is *not in the profile* — it
  silently drops the base plan's §10 retention backstop. **INV-S3-COMPLETE as stated ("S3 alone suffices") is true
  for the steady-durable state but false during the recover-with-live-writers transient.**
- **Gap 2 (defensive — K9):** restored-from-stale-backup Keeper. State the purge rule.

**Verdict on (b):** the *property* holds for durable state; the *recovery procedure* as written can lose an in-flight
ref. Fixable, but a real gap. **NOT safe as written; SAFE with the retention-backstop + reconcile-grace fix.**

### (c) "Keeper linearizability lets us delete the S3-only machinery." {#claim-c}

**PARTIALLY SAFE — overclaimed.** Of the four deleted mechanisms:

- `create-if-absent` fence **counter** → ephemeral-sequential: **SAFE to delete.** The seq number is a genuine
  monotone fence, free, linearizable. Correct collapse.
- writer time-lease + `2·Δ_skew` + read-back-durable renewals → ephemeral session: **PARTIALLY safe.** It removes the
  *clock-skew* form (no two clocks compared) and the *renewal gap* (keepalive is linearized). **But it does NOT
  remove the timing assumption** (see (d)) and it does NOT remove the writer's obligation to self-fence *before* the
  server-side expiry — it relabels it.
- strong-read fence + stealing-leader out-wait → "leadership = my node is still lowest seq, linearizable": **SAFE to
  delete the out-wait** *iff* the leader fail-closes on `Disconnected` (K5). The profile omits this rule, so as
  written it is **not** safe; with the rule, this collapse is legitimate and is Keeper's cleanest win.
- **closed-epoch S3 fold barrier → "linearizable `{O_W}` snapshot": UNSAFE to delete (V-K1/K3/K4).** This is the
  error. The barrier lived in **S3** (`log` is in S3); Keeper's linearizable `{O_W}` is on the far side of HB-CROSS
  and cannot order S3 `+` durability. **This mechanism must be retained.**

**Verdict on (c):** three of four collapses are legitimate (two needing an explicit fail-close-on-`Disconnected`
rule); **the fourth (closed-epoch S3 barrier) is a VIOLATION to delete.** Claim (c) is overclaimed.

### (d) "the safety hinge reduces to 'writer self-fences on session loss' with no clock-skew assumption." {#claim-d}

**VIOLATION — the "no clock-skew assumption" claim is false as a "no timing assumption" claim.** Precisely:

- It is **true** that there is **no cross-clock-skew comparison** between writer and GC anymore — Keeper's session
  timeout is a single arbiter. That specific S3-only hazard (V-EBR-3's `GC_clock − W_clock` bound) is genuinely
  gone. Credit where due.
- It is **false** that "no timing assumption survives." The hinge depends on the writer self-fencing *before* the
  server-side `t_expire`, which requires the writer to estimate elapsed time since last confirmed contact against
  `T_session` **on its own local clock** (§2.1). That is a timing assumption — a *local-elapsed-time-vs-session-
  timeout* assumption. It is weaker and cleaner than two-clock skew, but it is not nothing. A writer whose local
  clock *stalls* (throttled VM, suspended cgroup) under-estimates elapsed time, fails to self-fence, and commits
  after `t_expire` (K1/K2). **The assumption moved; it did not vanish.**
- Worse, even a *perfectly* self-fencing writer does not close the **in-flight S3 op** window or the **cross-system
  `+`-durability** gap (§3), which are unrelated to clocks entirely.

**Verdict on (d): VIOLATION.** The honest statement is: *"Keeper removes the inter-clock-skew assumption and the
renewal gap; the hinge becomes a local-elapsed-time-vs-session-timeout self-fence (fail-stop on `Disconnected`), plus
the unchanged cross-system requirement that the leader not reclaim until the writer's S3 side is durable+folded."*

### (e) "sharded snapshot fold is O(delta)." {#claim-e}

**SAFE — holds, orthogonal to coordination.** The streaming merge-sort over touched shards (sorted `snap` shard ⋈
sorted `log` deltas) is O(delta + touched shards), bounded memory; the only O(all-objects) pass is reconcile, which
is off-hot-path. This is a data-plane property independent of Keeper vs S3 coordination and the base review did not
challenge it. **SAFE.** (Caveat unrelated to correctness: the open item "round-trips per INSERT" from coalescing is a
performance question, not a safety one.)

---

## 8. Top-line verdict {#verdict}

**The Keeper-only profile is NOT correct as written.** Its architectural thesis — keep all durable state in S3, use
Keeper only as an O(active-writers) ephemeral accelerator — is **sound and worth building**, and Keeper genuinely
eliminates the S3-only mode's inter-clock-skew lease hazard, renewal gap, and fence-steal out-wait (the prior
review's E3/E4/E12). **But the profile makes two unsound jumps that re-open data loss**, plus several omitted rules:

The error is a single category mistake repeated: **treating Keeper's linearizability as if it created
happens-before edges into S3. It does not (HB-CROSS does not exist).** Every claim of the form "Keeper makes X exact"
must be checked for whether the S3 side of X is ordered — and twice it is not (the closed-epoch S3 barrier; the
session-expiry-vs-S3-commit window).

### VIOLATIONS / must-fix gaps, most important first {#must-fix}

1. **(VIOLATION — V-K1 / K3 / K4) Do NOT delete the closed-epoch S3 fold barrier.** Keeper's linearizable `{O_W}`
   snapshot does not order the writer's S3 `+` durability (HB-CROSS). Retain: (i) a fenced **closed-epoch barrier in
   S3** (`log/<e_a>` is closed-before-fold; late appends redirect to the open epoch), and (ii) the **flush-`+`-then-
   advance** rule enforced *by the writer ordering S3-`+`-durable before the Keeper `O_W` write*. This is the prior
   review's must-fix #1, which the profile silently drops. Without it, a writer that commits at `e_a` then advances
   `O_W` loses its `e_a`-era dependency.

2. **(VIOLATION — K1 / K2, the hinge) The self-fence must be fail-stop on `Disconnected`, not on confirmed
   `Expired`, with a local-elapsed-time deadline strictly inside `T_session`.** A partitioned/paused writer whose
   session the server already expired does not know it, and its independent S3 path keeps committing into a window
   the leader has already declared quiescent. State the rule, and pair it with #1 for in-flight S3 ops. This is the
   make-or-break hinge and the profile's "ops fail so the writer knows" reasoning is wrong for the partition case.

3. **(VIOLATION — K5) Leader must fail-close on `Disconnected`.** Every S3 DELETE requires a *fresh, `sync`-ed,
   successful* Keeper read confirming the leader still holds the lowest sequence; a failed or stale re-check means
   "not leader." Without this, a partitioned old leader split-brain-deletes under a revoked fence. (With it, Keeper's
   no-out-wait collapse is legitimate.)

4. **(VIOLATION — K7 + K14, INV-S3-COMPLETE transient) Restore the retention backstop and gate post-restore
   reconcile.** The profile drops the base plan's §10 time-retention backstop. After a Keeper wipe there are no live
   leases, so reconcile's only safe roots are S3 refs/`+`/durable state **plus** a `retention > max op` backstop, and
   reconcile must wait a grace ≥ max in-flight op duration before its first delete. Otherwise a writer's in-flight
   ref across the outage is reclaimed.

5. **(VIOLATION — K15) The `safe_epoch := E_cur` (no-live-writer) branch requires a `sync`-ed linearizable
   membership read.** Keeper reads are sequentially consistent by default; a stale `getChildren(writers/)` that
   misses all survivors would wrongly take the empty branch and reclaim aggressively. `sync` before the quiescence
   read (at least for the empty branch).

6. **(VIOLATION — K9, INV ephemeral assumption) On Keeper restore from any persistent backup/snapshot, purge / refuse
   to honor pre-restore election and `writers/` znodes.** Otherwise "ephemeral" durable-backup ghosts violate claim
   (a). State it.

7. **(NEEDS-FIX, carried from base) Successor leader re-FOLD+re-QUIESCE under its own fence before any delete (K6);
   `epoch/current` strictly monotone, 64-bit, never reset, never derived from a Keeper sequence number (K8/§5.1);
   `snap` published single-object-atomic with checksum (K13); re-registering writer re-reads `epoch/current` (K17);
   bound or re-check the deadline→ref-PUT stall (K18).**

8. **(LEAK, carried from base) Resurrection-away leaves a `+(H,e_a)` with no matching `-` (K19)** — log a compensating
   `-`, or make reconcile a mandatory backstop.

### Is the safety hinge truly clock-skew-free, or just a session-timeout assumption? {#hinge-answer}

**It is a session-timeout assumption, not skew-free.** Keeper removes the *inter-clock comparison* (writer vs GC) —
there is no longer a `GC_clock − W_clock ≤ Δ_skew` bound. That is real and good. But the hinge still requires the
writer to self-fence **before** the server expires its session, which the writer can only do by measuring elapsed
local time against `T_session` (fail-stop on `Disconnected` inside `T_session − Δ_margin`). A writer whose local
clock stalls or that waits for confirmed `Expired` instead of acting on `Disconnected` will commit after expiry. So
the assumption is **relocated** from "two clocks agree within `Δ_skew`" to "the writer's local elapsed-time estimate
is sound relative to a single session timeout, and the writer fail-stops on `Disconnected`." That is strictly weaker
and cleaner (one timeout, keepalive-as-renewal, no second clock) — **but it is emphatically not "no timing
assumption."** And it is entirely separate from the cross-system `+`-durability gap (§3), which involves no clock at
all.

### Does INV-S3-COMPLETE hold? {#s3-complete-answer}

**For steady-state durable data: yes** — and the design choice to keep `epoch/current` in S3 (not Keeper) is exactly
what makes a Keeper wipe non-destructive (K8). **For the recovery-with-live-writers transient: no, as written** — an
in-flight ref across the outage can be reclaimed by post-restore reconcile (K7), because "recoverable from S3 alone"
implicitly assumes all durable facts are already durable, and an in-flight ref is not yet. Restoring the retention
backstop + reconcile grace (#4) closes it. There is also the K9 backup-ghost caveat. **Verdict: INV-S3-COMPLETE is
the design's strongest claim and nearly holds; it needs the reconcile-grace/retention fix and the backup-purge rule
to hold unconditionally.**

### Under-specified — could not be decided as written {#underspecified}

- **`Disconnected` vs `Expired` self-fence semantics** — the profile says "session lost ⇒ read-only" without
  defining "lost." Tested under "act on confirmed `Expired`" → **unsafe** (K1/K2); under "fail-stop on
  `Disconnected` within `T_session`" → safe. The profile must pick the latter and state the local deadline.
- **Whether the closed-epoch S3 barrier is retained** — §3 says it is *deleted*; the proof needs it retained.
  Tested under "deleted" → **unsafe** (V-K1). Correctness undecidable until the profile states it is kept (it is an
  S3 mechanism Keeper cannot replace).
- **`sync` before the quiescence read** — §4 R3 says "linearizable read" but Keeper reads are sequentially
  consistent without `sync`. Undecidable for the empty branch until stated (K15).
- **Post-restore reconcile root set / grace** — §5.3 says "S3 reachability is the authority" but does not address an
  in-flight ref or the empty-lease-set problem; the retention backstop is dropped. Undecidable until restored (K7).
- **Restore-from-backup ephemeral handling** — §5 assumes ephemerals simply vanish; a *backup* restore violates that.
  Undecidable until the purge rule is stated (K9).

**Bottom line for a spec:** adopt the Keeper-only profile's *architecture* (S3-durable + O(active-writers) ephemeral
Keeper) — it is the right shape and (a)/(e) hold and (b) nearly holds — but the spec is not done until it (1) **keeps
the closed-epoch S3 barrier and the writer-ordered flush-`+`-then-advance** (Keeper cannot replace an S3-side
happens-before edge), (2) makes the writer **fail-stop on `Disconnected` inside `T_session`** (the hinge is a
session-timeout assumption, not skew-free), (3) makes the leader **fail-close on `Disconnected`** before any DELETE,
(4) **restores the retention backstop and gates post-restore reconcile**, and (5) `sync`s the quiescence read's
empty branch and purges backup-restored ephemerals. With those, Keeper delivers what it actually can — removing the
inter-clock-skew lease/renewal/out-wait hazards — without the profile's overclaim that it removes the cross-system
ordering requirement or the timing assumption entirely.
