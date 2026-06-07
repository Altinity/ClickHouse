# CA garbage collection — improved design plan (Epoch-Based Reclamation, hardened with prior art)

Consolidated, prior-art-grounded plan for the content-addressed (CA) MergeTree GC. Supersedes the sketches in
`2026-06-07-ca-gc-simplification-analysis.md` and `2026-06-07-ca-gc-epoch-reclamation-design.md`; addresses the
correctness review (`2026-06-07-ca-gc-simplified-correctness-review.md`); and pulls battle-proven concepts from
RCU/QSBR, Crossbeam epoch reclamation, hazard pointers, Cassandra `gc_grace`, and Iceberg/Delta vacuum.

## 1. Goals {#goals}

- **Correct** under concurrency + crashes + pauses + leader hand-off, provable in the Lamport model.
- **Dead simple to operate and reason about** — one mechanism per problem; fits in your head.
- **Scales** to 10^11 objects with a ~10-min GC cadence — **no full-bucket scan on the hot path**.
- **Portable**: all coordination in `{create-if-absent, read, delete, list}` + a leader/fence + per-writer
  leases. S3-only works; Keeper is a strictly-cleaner drop-in (and removes the one clock-skew assumption).

## 2. The model in one paragraph {#model}

Objects are content-addressed blobs/manifests stamped with the **epoch** they were created in
(`blobs/<H>/<e>`); a **ref** is the only mutable root and the commit point. A monotone **epoch** is a logical
clock the **GC leader** advances. Each **writer** holds a **time-bounded lease** publishing its **observed
epoch** `O_W`; the GC condemns unreferenced objects (a `<e>.tombstone`) and physically deletes them only after
**every live writer has observed past the condemnation epoch** (Epoch-Based Reclamation). For long-lived
dependencies a writer additionally drops a fine-grained **`+` pin** (a hazard-pointer-style write barrier), so a
slow writer never stalls reclamation pool-wide. The reverse-index log/snapshot is a **sloppy candidate filter**
(over-count safe); the **only** delete authority is "unreferenced AND epoch-quiescent AND fenced." A
conservative full scan exists only to **reconcile/rebuild**.

## 3. Prior art → mechanism mapping {#prior-art}

| Battle-proven concept (system) | What we adopt |
|---|---|
| **QSBR grace = wait for all readers quiescent** (Linux RCU) | delete epoch-`e` garbage only when `safe_epoch = min(O_W over live writers) > e` |
| **3 epochs / limbo lists; reclaim at global ≥ e+2; local epoch lags ≤ 1; amortized advance** (Crossbeam) | bound condemned garbage to a small window; reclaim `tombstone(e)` when `E_cur ≥ e+2 ∧ safe_epoch > e`; a writer >1 epoch stale must re-sync; GC advances the epoch once per round, not per op |
| **Epoch + pointer (hazard) HYBRID** (Stamp-it / crossbeam hybrid) | coarse per-writer epoch for the common case **+** fine per-blob `+` pin for long-lived deps → no global stall |
| **Hazard pointer = per-object protection** (Michael) | the `+` delta IS the hazard pointer: "this specific blob is in use," read by the GC fold |
| **`gc_grace_seconds`, tombstones, zombie prevention, bounded tombstones** (Cassandra) | tombstones are the condemn marker; quiescence (not wall-clock) is the grace; reclaim tombstones at `e+2` so they never accumulate unbounded |
| **Retention > longest write; orphan-clean is separate + conservative + ordered; min-snapshots-to-keep** (Iceberg/Delta) | lease-quiescence replaces the time-retention guess; keep a conservative time-retention only as a reconcile backstop; reconcile runs *after* condemnation, never deletes objects newer than retention |
| **Snapshot-at-the-beginning / write barrier; over-approximate live set** (concurrent GC) | `+`-before-use is a write barrier; the index may over-count, never under-count |
| **Fencing tokens** (Kleppmann; HDFS/Ceph STONITH) | every DELETE and epoch-advance gated by a monotone fence on `gc.lock` |
| **Leases + master clock** (Chubby/GFS); **ephemeral sessions, leader election** (ZooKeeper) | time-lease + `Δ_skew` on S3; Keeper ephemeral session = skew-free lease + leader election |
| **Abort-incomplete-multipart lifecycle** (S3) | crashed 100 GB uploads never become a visible blob; reclaimed by an S3 lifecycle rule, orthogonal to GC |

## 4. State (objects / nodes) {#state}

```
epoch/current                  monotone epoch E_cur (advanced ONLY by the fenced GC leader, once per round)
blobs/<H>/<e>                  immutable content bytes of hash H, created in epoch e   (generation == epoch)
blobs/<H>/<e>.tombstone        per-object CONDEMN marker; records condemnation epoch e_a; create-if-absent
active/<H>                     hint: current reusable epoch for H (plain PUT; LIST-derivable; never trusted)
store/.../refs/<part>          live ref (commit point / GC root) -> part_id
parts/<part_id>/<e>            manifest; identical epoch/condemn lifecycle as a blob
log/<e>/<shard>/<event_id>     +/- reference deltas appended in epoch e  (sloppy candidate filter; event_id-deduped)
snap/<e>/<shard>               folded counts (reverse index)
writers/<W>                    writer lease { observed_epoch O_W, lease_until }   (Keeper EPHEMERAL session)
gc.lock                        fenced leader lock { server_id, fence_token, lease_deadline }
```

Generation == epoch: a resurrected blob is the same content re-created under the *current* epoch
(`blobs/<H>/<E_cur>`), a different key from the condemned one → ABA-safe with only `create-if-absent`.

## 5. Protocol {#protocol}

### 5.1 Writer (per part needing content `H`) {#writer}
```
0. LEASE: keep writers/<W> live (background heartbeat). O_W := read(epoch/current), refreshed.
   - local epoch may lag the global by AT MOST 1 (Crossbeam rule). If O_W < E_cur-1, RE-SYNC before reuse.
   - self-fence: a write may COMMIT (publish ref) only while `now < lease_until` (local clock). Else GO
     READ-ONLY and reject the write. (No end-of-commit network re-read needed.)
1. obtain H:
   a. read active/<H> (or LIST blobs/<H>/)
   b. ABSENT  -> createIfAbsent blobs/<H>/<O_W>; (stream-hash to local scratch then) upload; createIfAbsent active.
   c. PRESENT (H,e) -> HEAD blobs/<H>/<e>.tombstone:
        none      -> dedup-reuse (H,e)
        condemned -> resurrect: createIfAbsent blobs/<H>/<E_cur>; upload; PUT active/<H>=E_cur; use (H,E_cur)
2. PIN EARLY: append `+` for the chosen (H,e) into log/<O_W> AS SOON AS the dependency is decided
   (NOT at the end). This is the hazard pointer: it lets O_W advance freely afterward without endangering H,
   so a long operation never stalls pool-wide reclamation.
3. publish the live ref (commit point, written LAST; self-fence check immediately before).
4. drop: remove the ref FIRST, then append `-`.
```

### 5.2 GC leader (fenced, single deleter) {#gc}
```
ROUND (≈10 min):
  R0 CLOSE   E_cur := E_cur+1   (fenced PUT; the ROTATION BARRIER; amortized — once per round). Epoch e is now
             CLOSED: writers re-syncing to >e no longer append to log/<e> (they REAPPEND a still-needed + into
             the open epoch — reappendIfAdvanced, already in the codebase). Only CLOSED epochs are folded.
  R1 FOLD    only CLOSED epochs (log/<≤ e_closed>) -> snap; objects with no live reference become candidates.
             (V-EBR-1: never fold an open epoch; this + the O_W↔+-durability coupling in §6 are what make the
             "O_W>e_a ⇒ writer saw the tombstone / its + is folded" case of the proof actually hold.)
  R2 CONDEMN createIfAbsent blobs/<H>/<e>.tombstone for each candidate (condemnation epoch e_a).
  R3 QUIESCE safe_epoch := min(O_W) over writers whose lease is NOT definitely expired
             (definitely expired only at GC_now > lease_until + Δ_skew). If no live writer, = E_cur.
  R4 RECLAIM for each condemned (H,e_a) with  E_cur ≥ e_a+2  AND  safe_epoch > e_a  AND  still-unreferenced
             AND stillLeader(fence):  DELETE blobs/<H>/<e_a>; drop its tombstone; refresh active.
             (E_cur ≥ e_a+2 = Crossbeam 3-epoch limbo; safe_epoch > e_a = QSBR grace.)
```
`stillLeader(fence)` (re-read `gc.lock`, confirm fence) gates **every** mutation. A **successor** leader must
run a fresh R1→R3 under **its own** fence before any R4 delete (do not reclaim on inherited candidate state).

### 5.3 Reader {#reader}
`GET active/<H>` → epoch → `GET blobs/<H>/<e>`; on `404`, `LIST blobs/<H>/` and read any present epoch. A
tombstone gates *attachment*, never *reads*: a successfully-GET'd condemned blob is still valid bytes.

## 6. Long-running writes / large (100 GB) blobs {#long-writes}

The case that breaks naive lease/retention designs — handled by three rules:

1. **The lease is a heartbeat, not an operation timer.** `T_lease` bounds *death detection* (seconds), not
   operation length. A 100 GB / multi-hour upload simply keeps heartbeating `writers/<W>` in the background;
   the self-fence is checked at the *commit point*, which is after the upload. (Improvement over Iceberg's
   "retention > longest job": no max-duration guess; correct for any duration while the writer is alive.)
2. **Early `+` pins deduped dependencies (hazard pointer).** A dependency is `+`-logged at *decision time*, so
   it is pinned for the whole long operation by the `+` (fold sees it → never condemned), and `O_W` may advance
   freely. Thus a long op does **not** hold `safe_epoch` low pool-wide — only the sub-second decide→`+` window
   does. This is the epoch+hazard hybrid earning its keep.
3. **The fresh blob is invisible until multipart-complete.** With S3 multipart, `blobs/<H>/<e>` does not exist
   until `CompleteMultipartUpload`; the `+`/ref follow immediately, so the exposed window is tiny and covered by
   the live lease. A writer that dies mid-upload: lease lapses → read-only/dropped; the incomplete multipart is
   reclaimed by an **S3 lifecycle abort-incomplete-multipart** rule; no blob ever appeared → GC does nothing.

**LOAD-BEARING RULE (corrected per the review — this is *the* constraint, not a "non-constraint"):**
> `O_W` may advance past epoch `e` only once every `+` the writer issued for a dependency at `≤ e` is
> **durable AND folded** (and the writer has re-checked no decided dep was condemned). Equivalently:
> **flush-`+`-then-advance.**

Why it is load-bearing (V-EBR-2): the `+` is consumed only by the epoch-window fold, *not* point-read as a
hazard at delete time. If a long op advanced `O_W` "freely" while its `+` was not yet durable+folded, the
leader could condemn and delete the decided dependency mid-upload. Two acceptable implementations: **(i)**
gate `O_W`-advance on `+`-durability (above), or **(ii)** have RECLAIM additionally **point-`HEAD` the
dependency's `+`/pin** before deleting (treat `+` as a true hazard pointer). Pick (i); it composes with the
closed-epoch fold in §5.2.

## 7. Leases, fencing, and the one clock-skew assumption {#leases}

- **Writer lease (the safety hinge).** Keeper: an **ephemeral session** — skew-free, session loss ⇒ ops fail ⇒
  writer self-quiesces. S3 (corrected per V-EBR-3): a TTL object with a **directional, symmetric `2·Δ_skew`
  margin** — the writer self-fences at `lease_until − Δ_skew` (local clock), the GC drops the writer only at
  `GC_now > lease_until + Δ_skew`, under the assumption `GC_clock − W_clock ≤ Δ_skew`. Renewals count only
  once **read-back-durable** (issue→read-confirm before extending the self-fence), closing the
  renewal-vs-expiry gap. Renew well inside `T_lease`. **Keeper is the recommended coordinator** (removes the
  skew assumption entirely); S3-only is the documented weaker mode.
- **Leader fence (deleter safety).** `gc.lock` carries a monotone fence (Keeper: ephemeral-sequential / czxid;
  S3: a `create-if-absent` counter). Every DELETE / epoch-advance re-checks the fence (Kleppmann fencing).
  On S3 the fence re-check must be a **strong read**, and a **stealing** leader must **out-wait** the prior
  lease before deleting, so a paused-then-resumed leader cannot delete under a fence-steal that is not yet
  durable-visible to it (the split-brain visibility VIOLATION). Keeper is safe by construction. This is a
  **safety asymmetry** between the backends, not merely ergonomic.

## 8. Invariants {#invariants}

- **INV-NO-LOSS:** an object is deleted only if unreferenced in the fold **and** `safe_epoch >` its
  condemnation epoch **and** `E_cur ≥ e_a+2` **and** the leader still holds its fence. (QSBR grace ⇒ no live
  writer will newly reference it; the fold ⇒ none does now.)
- **INV-NO-DANGLE:** a published ref always resolves (reads tolerate stale `active` via LIST).
- **INV-NO-ABA:** delete (`<e_a>`) and recreate (`<E_cur>`, `E_cur>e_a`) never share a key.
- **INV-OVER-COUNT-ONLY:** every failure mode (lost/dup `+`, crash, reorder) biases to over-count (leak,
  reconciled later), never under-count (loss).

**Resurrection accounting (review R2 leak fix):** when a writer routes away from a condemned `(H,e_a)` to
`(H,E_cur)`, if it had already logged `+(H,e_a)` it must log a matching `-(H,e_a)` (and `+(H,E_cur)`), so the
abandoned generation nets to 0 rather than leaking under the count. (A missed `-` is over-count-only → safe,
reconciled — but logging it keeps the count tight.)

**Successor-leader / monotonicity / atomic-publish bundle (review R2 NEEDS-FIX):** a successor GC leader
re-folds + re-quiesces under its **own** fence before any delete; `epoch/current` is strictly monotone,
64-bit, never reset; `snap` is published as a single atomic object; `reconcile` treats live leases, live `+`s,
and the retention window as roots.

## 9. Failure handling {#failures}

| Fault | Outcome |
|---|---|
| writer crash mid-build / mid-100GB-upload | no completed blob (multipart) or no ref; lease lapses → dropped; incomplete multipart aborted by S3 lifecycle. Safe. |
| writer paused (alive lease) | pins `safe_epoch` at `O_W`; reclamation of newer epochs waits. Liveness only. |
| writer wrongly declared dead then resumes | self-fence (Keeper op-fail / S3 local deadline) ⇒ read-only before any reuse-commit. Safe iff §7 holds. |
| GC leader crash mid-round | idempotent (`create-if-absent` tombstones, monotone epoch PUT, fenced deletes); successor takes higher fence. Safe. |
| split-brain leaders | only the highest fence's mutations land; lower-fence DELETEs rejected. Safe. |
| lost/torn snapshot | rebuilt by reconcile (the only full scan). Safe (over-protective). |
| stale/lost `active` | hint only; LIST fallback. No safety impact. |

## 10. Reconcile (the only full scan) {#reconcile}

A slow, conservative, off-hot-path job (Iceberg "remove orphan files" analogue): rebuild a lost snapshot;
reclaim abandoned uploads and over-count leaks. **Never deletes anything newer than a conservative retention**
(the Iceberg lesson, kept as a backstop on top of quiescence) and runs *after* condemnation in the maintenance
order. Default cadence low (hours/off); it is a safety net, not the hot path.

## 11. S3 ↔ Keeper portability {#portability}

Everything is `create-if-absent` / `read` / `delete` / `list` + leader/fence + writer-lease. The **only**
differences: the writer lease (S3 TTL+skew vs Keeper ephemeral session) and the fence allocator (S3
`create-if-absent` counter vs Keeper ephemeral-sequential). The refcount log/snapshot is S3-native durable
state; Keeper holds **no durable state** (it is purely the lease/leader accelerator).

## 12. Phased implementation plan {#phases}

1. **P0 — the `Coordination` seam.** Interface `{createIfAbsent, read, delete, list, acquireLeadership→fence,
   stillLeader, writerLease}`. S3 impl first; Keeper impl second. Everything below is written once against it.
2. **P1 — epoch + writer leases.** `epoch/current`, the per-writer lease + heartbeat + self-fence + the
   ≤1-epoch-lag re-sync + read-only-on-stale.
3. **P2 — write path on epochs.** `blobs/<H>/<e>`, `active`, resurrection-into-`E_cur`, early-`+` pin, ref-last.
4. **P3 — GC rounds.** FOLD→CONDEMN→ADVANCE→QUIESCE→RECLAIM with the `e+2` limbo + `safe_epoch` gates +
   fence-checked deletes. Tombstone reclamation.
5. **P4 — reconcile.** Conservative full-scan rebuild/orphan cleanup with retention backstop.
6. **P5 — Keeper coordinator.** Ephemeral-session lease + ephemeral-sequential fence; flip the recommended mode.

Each phase is independently testable; P0–P3 give a working S3-only GC; P5 removes the clock-skew assumption.

## 13. Open questions to model-check before coding {#open}

1. **§7 S3 time-lease** — model-check the local self-fence + `Δ_skew` wait; size `Δ_skew`. (Keeper sidesteps.)
2. **`active/<H>` cross-epoch update** — confirm a reorder can never point `active` *below* a present
   generation in a way reads can't recover (reads LIST, so likely fine; state it).
3. **Epoch-rotation barrier vs in-flight `+`** — RESOLVED (review R2 / V-EBR-1): adopt the **closed-epoch
   fenced fold + reappend-to-open-epoch** (§5.2 R0/R1) and **couple `O_W`-advance to `+`-durability** (§6);
   the overlap-window option is rejected. (Model-check the reappend race itself.)
4. **New writer registering** — confirm it observes `E_cur` (cannot lower `safe_epoch` below a reclaimable
   epoch). Should hold by construction; verify.
5. **`Δ_skew` / `T_lease` / epoch-period / `min` interplay** — pick defaults (e.g. epoch 10 min, `T_lease` 60 s,
   heartbeat 15 s, `Δ_skew` 30 s) and check a busy-but-healthy long writer is never wrongly forced read-only.

## Sources {#sources}

- Crossbeam epoch reclamation (3 epochs / limbo lists / ≤1-epoch lag / amortized advance): [crossbeam_epoch docs](https://docs.rs/crossbeam/latest/crossbeam/epoch/index.html), [epoch-gc RFC](https://github.com/crossbeam-rs/rfcs/blob/master/text/2017-05-23-epoch-gc.md)
- Hybrid epoch+pointer reclamation: [Stamp-it (arXiv 1712.06134)](https://arxiv.org/pdf/1712.06134)
- Iceberg maintenance / retention-must-exceed-longest-write / safe order: [Apache Iceberg maintenance](https://iceberg.apache.org/docs/latest/maintenance/), [Iceberg maintenance runbook (IOMETE)](https://iomete.com/resources/blog/iceberg-maintenance-runbook)
