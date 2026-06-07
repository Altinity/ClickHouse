# CA GC — simplification analysis (toward a dead-simple, S3/Keeper-portable design)

> **CORRECTION (2026-06-07, after an adversarial distributed-systems + S3 correctness review —
> `2026-06-07-ca-gc-simplified-correctness-review.md`):** §3's central claim, *"delete sessions; the `+`
> delta is the pin,"* is **UNSOUND** — the `+` is consumed only through a window-bounded fold and gives the
> leader no fresh happens-before read at delete time, so it does not pin (concrete data-loss schedules V1/V3/V4).
> P1 needs a minimal directly-**listed** pin object read fresh at delete (*"a session by another name"*) — so
> sessions reduce, they do **not** disappear. Also required and wrongly dropped below: the `gc/current_epoch`
> window-close barrier, fence-re-check on *every* DELETE, an S3 *lease-out-wait* before deleting under a fresh
> fence, and a minimal open-tombstone/RECOVER path. The **P2/generations core (§4) is confirmed sound and
> portable.** Read §§3,5,6 as refuted-in-part; the corrected minimal target lives in the review doc.

Working analysis (not yet a spec). Goal: **design clarity and simplicity** — the protocol should fit on a
page, and **all synchronization between writers, deleters, and GC must be expressible in primitives that map
trivially to S3 *and* Keeper**. Grounded in `2026-06-07-ca-protocol-and-lockless-gc.md`, the Milovidov review
(`2026-06-07-ca-design-review-milovidov.md`), and the backlog (`cas-mergetree-integration.md`).

Today the content-addressed (CA) backend has ~7,560 lines of CA source with **~599** references to
`WriteSession` / `tombstone` / `resurrect` / `generation` / `active` — i.e. these mechanisms *are* the bulk of
the complexity.

## 1. A CA GC has only two hard problems {#two-problems}

Everything else is machinery to solve these two *without a global lock*:

- **P1 — upload→ref window.** Don't delete a blob that has been uploaded/deduped but is not yet named by a ref.
- **P2 — ABA window.** Don't `DELETE(key)` while a writer concurrently recreates the same content key
  (delete-after-recreate = data loss).

Map every current mechanism to the problem it serves:

| Mechanism | Serves | Verdict |
|---|---|---|
| `gc/log` + `gc/snap` (refcount) | the GC itself | **essential — the core** |
| `Scan B` full reachability (current delete gate) | nothing the count can't | **demote to rebuild/reconcile only** |
| `sessions/<id>` (+ lease, `delta_epochs`, sticky-fail `#2`, `reapFoldedSessions`) | P1 | **shed — the `+` delta is the pin** |
| generations `<g>` + `<g>.tombstone` | P2 | **keep one ABA mechanism — stripped** |
| `active` hint + resurrection + `gc/sealed` index | P2 bookkeeping | **shed — re-derive from the bucket/compaction** |

## 2. The synchronization primitives (the portability rule) {#primitives}

The common denominator of S3 and Keeper is exactly four primitives — plus leadership:

| Need | S3 | Keeper | Portable |
|---|---|---|---|
| append/read/enumerate/reclaim the log & snapshot | PUT / GET / LIST / DELETE | (stays in S3; Keeper holds no durable state, G4) | ✓ S3-native |
| pin (P1), condemn + recreate (P2) | `create-if-absent` (`If-None-Match: *`) + `delete` | `create` + `delete` | ✓ trivial both |
| leader election + **fence token** | lock object + monotonic fence counter (O(n) `create-if-absent` scan — awkward) | **ephemeral sequential znode** (fence = seq no., free) | ⚠ only divergence |
| liveness / lease | deadline in the lock object + fence (clock guess) | **ephemeral** (auto-release on session loss) | ⚠ Keeper cleaner |

**Architectural consequence:** define one tiny interface

```
Coordination {
    bool   createIfAbsent(key, bytes)     // S3 If-None-Match:* ; Keeper create
    bytes  read(key)                       // GET ; getData
    void   delete(key)                     // DELETE ; delete
    list   list(prefix)                    // LIST ; getChildren
    Fence  acquireLeadership()             // S3 lock+counter ; Keeper ephemeral-seq
    bool   stillLeader(Fence)              // re-read lock fence ; check ephemeral/seq
}
```

Write the whole protocol once against this interface. The S3 impl and the Keeper impl differ **only** in the
leadership/fence rows; the refcount and the P1/P2 mechanics are pure `create-if-absent`/`read`/`delete`/`list`
and identical on both. This is "S3 is the source of truth; Keeper is an optional accelerator holding no
durable state" made literal — and it forbids CAS / atomic read-modify-write anywhere (S3 has none).

## 3. P1 — delete sessions; the `+` is the pin {#p1}

The only job of `sessions/<id>` is P1: keep a just-uploaded blob reachable until a ref names it. But the writer
already logs a `+` delta on commit. **Log the `+` before the upload** (the same pin-before-upload ordering the
session already uses), and the `+` *is* the durable pin — the delete gate folds the open log and sees it. An
aborted writer leaves a stale `+` → harmless over-count (the design already biases to over-count, I8), cleaned
by reconcile. This removes the entire `sessions/` subsystem (leases, `delta_epochs`, the sticky-fail `#2`
path, `reapFoldedSessions`).

## 4. P2 — keep generations (stripped), drop the rest {#p2}

P2 is **irreducible**: you cannot safely delete a content key a writer may recreate using only
create-if-absent/read/delete/list **unless** the recreate targets a *different* key. Generations do exactly
that — recreate routes to `g+1` via `create-if-absent`, so the leader's `DELETE` of a condemned `g` cannot race
a recreate. This is *more* portable and simpler-to-prove than a same-key delete-marker (which leaves the writer
racing the leader on the same key) and needs no CAS. So **keep generations + the `<g>.tombstone` condemn
marker** — but strip the bookkeeping around them:

- **Drop the `active` hint** (and its repair path): default to `g=0`; resolve the present generation by a single
  `LIST blobs/<H>/` (already the fallback). One fewer object class, no best-effort-PUT-that-can-lie.
- **Drop the `gc/sealed/<shard>` index:** re-derive count-0 candidates from the compaction each round
  (the candidate set is cheap to recompute; the index was a micro-optimization).
- **Drop the resurrection *cap* (B83/B84)** and the permanent-gravestone accrual concerns: with the count
  authoritative and the leader the sole deleter, the seal lifecycle is leader-local.

## 5. Authority — log+snapshot; full scan only to reconcile {#authority}

Make `gc/log`+`gc/snap` the **sole** delete authority (the deferred B78 decision, taken toward "counting is
authoritative" — Milovidov #1). Delete a blob iff its folded count is 0 for ≥ a grace window **and** a final
fold of the open log at delete time still shows 0. The full-bucket `Scan B` reachability walk is **demoted to
`reconcile` only** — rebuilding a lost/corrupt snapshot, abandoned-upload sweeps — and runs off the hot path.

## 6. The page-sized protocol {#target}

> One per-pool **log** (`+`/`-` deltas, `create-if-absent`) and **snapshot** (folded counts). A single fenced
> **leader** (S3 lock+fence, or Keeper ephemeral-seq) folds the log and, for any blob count-0 past a grace and
> still 0 on a final fold, **condemns** it (`<g>.tombstone`, `create-if-absent`) then **deletes** `<g>`. Writers
> log `+` before upload and `-` after ref-removal; on dedup they recreate into `g+1` if `g` is condemned. Reads
> resolve the present generation by `LIST`. Full scan only rebuilds the snapshot. No sessions, no `active`
> hint, no `gc/sealed`, no resurrection cap.

## 7. What this removes {#removed}

- Backlog **B78** (the Scan-B→§6.2 gate decision) — resolved by making counting authoritative.
- Backlog **B83/B84** (resurrection cap, gravestone accrual) — gone with the bookkeeping.
- The `sessions/` subsystem and `WriteSession` lifecycle.
- The `active` hint + its `404`→`LIST` repair, and the `gc/sealed` index.
- Most of the ~599 mechanism references; the remaining core is log + snapshot + generations + tombstone.

## 8. The one open decision {#decision}

P2 mechanism: **stripped generations** (recommended — pure `create-if-absent`, ABA-safe, portable, no CAS) vs a
single leader-serialized **delete-marker** (fewer object kinds but a same-key recreate race and more reliance on
the leader being the only mutator). The portability rule (§2) favors generations. Confirm this and the analysis
becomes the basis for a from-scratch spec.
