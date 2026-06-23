# CaBuildRootPrecommit — TLA+ results (2026-06-18, B171)

TLA+ validation of the **build-root / precommit** redesign that fixes B140-dangle.
Two-config proof: a **buggy** config (today's protection-as-revocable-hint, no build root)
**reproduces** the `INV_NO_DANGLE_COMMITTED` violation; the **fixed** config (build-root
reachability + fail-closed commit + reclaim) holds **all four** invariants exhaustively.

- Model: `CaBuildRootPrecommit.tla`
- Design spec: `../specs/2026-06-18-ca-build-root-precommit-design.md`
- Root-cause report: `../reports/2026-06-18-ca-b140-dangle-trigger-pinned.md`

## Model

Two root kinds over a small object space:

- **TABLE root** (reader-facing): `tableRefs \subseteq Trees`. `INV_NO_DANGLE_COMMITTED` is
  enforced here and only here.
- **BUILD root** (precommit intent): `precommit[bld] \in Trees \cup {None}`. An intent
  structure; legitimately references absent objects (pending / aborting).

Shared blob (essential to the dangle): `Trees={t1,t2}`, `Blobs={b1}`; both trees reference
the single shared `b1` (dedup). Two builds (`Builds={bld1,bld2}`) so one build's protection
lapse races another's reference: `bld1` writes/owns `b1`, `bld2` adopts `b1`.

Actions: `WriteBlob`, `AdoptBlob` (dedup reference, no re-stamp), `PublishTableRef` (the
pre-existing independent pin), `Precommit`, `Commit` (fail-closed or blind), `CommitAbort`,
`DropTableRef`, `RemovePrecommit`, `BuildDie` (real crash/retire), `BuildFreeze` (false-positive
heartbeat freeze — judged dead but still running), `GcFold`, `GcReclaimPrecommit`, `GcDelete`.

Two independent flags select buggy vs fixed:

- `UseBuildRoot` — `FALSE`: protection is the revocable per-object owner hint (`cas_owner` /
  `protectedByLiveBuild`); `AdoptBlob` never re-stamps owner, so protection tracks the
  byte-writer, not the referencing build, and lapses when the writer is judged dead. `TRUE`:
  the precommit edge gives a structural build-root reference; a present object reachable from a
  live precommit has in-degree ≥ 1 and `GcDelete` cannot fire.
- `FailClosedCommit` — `FALSE`: `Commit` publishes the table ref with no final presence
  re-check (structural flaw #2). `TRUE`: `Commit` publishes iff the whole closure is present,
  else takes `CommitAbort` (re-checks even after a premature reclaim).

## Results (2×2, `Builds={bld1,bld2}`, `Trees={t1,t2}`, `Blobs={b1}`)

| `UseBuildRoot` | `FailClosedCommit` | cfg | result | distinct states |
|---|---|---|---|---|
| FALSE | FALSE | `CaBuildRootPrecommit_buggy.cfg`          | **INV_NO_DANGLE_COMMITTED violated** | 553 (to CE) |
| TRUE  | FALSE | `CaBuildRootPrecommit_buildrootonly.cfg`  | **INV_NO_DANGLE_COMMITTED violated** | 1891 (to CE) |
| FALSE | TRUE  | `CaBuildRootPrecommit_failclosedonly.cfg` | **No error; exhaustive** | 2193 |
| **TRUE** | **TRUE** | `CaBuildRootPrecommit_fixed.cfg`      | **No error; exhaustive** | **24205** |

Invariants checked in every cfg: `TypeOK`, `INV_NO_DANGLE_COMMITTED`, `INV_BUILDROOT_PROTECTS`,
`INV_COMMIT_FAILCLOSED`.

**Conclusion.**

- The buggy config (FALSE/FALSE) **reproduces** the B140-dangle (below) — the model is not
  vacuous on the bug side.
- Build-root **alone** (TRUE/FALSE) **still dangles**: a blind commit publishes a table ref
  whose blob was deleted before the precommit edge existed (the ordering window, §4.5). So
  fail-closed commit is **independently necessary**.
- Fail-closed commit **alone** (FALSE/TRUE) is already clean for `INV_NO_DANGLE_COMMITTED` in
  this object space (every published table ref re-verifies presence; a deleted blob is never
  table-pinned at the gate). It is exhaustively clean, but `INV_BUILDROOT_PROTECTS` is
  vacuously true (`UseBuildRoot=FALSE`), so it does **not** give the build-root protection
  guarantee or bound wasted work — the design wants both halves.
- The **full fix** (TRUE/TRUE) holds all four invariants, exhaustive over **24205** states,
  and is proven **non-vacuous** (reachability witnesses below).

## The reproduced B140-dangle (buggy cfg, 6-state counterexample)

Final state: `tableRefs={t1}`, `committed={t1}`, `present=(t1↦TRUE, t2↦FALSE, b1↦FALSE)` —
`b1 \in TableClosure` but `~present[b1]` → `INV_NO_DANGLE_COMMITTED` violated.

1. **`WriteBlob(bld1, b1)`** — build `bld1` writes the shared blob `b1`; `owner[b1]=bld1`
   (the `cas_owner` stamp). `b1` present.
2. **`AdoptBlob(bld2, b1)`** — build `bld2` adopts `b1` (dedup; no bytes moved, **owner stays
   `bld1`** — structural flaw #1). `bld2` now intends to publish a tree over `b1`.
3. **`BuildDie(bld1)`** — the *owner* build `bld1` retires/crashes; `min_active` rises past it
   (`judgedDead={bld1}`). `OwnerProtected(b1)` flips **false** — the byte-writer is gone — even
   though the live adopter `bld2` still holds `b1`.
4. **`GcDelete(b1)`** — `b1` is in-degree 0 (no table ref pins it yet) and now unprotected →
   GC deletes it. `present[b1]=FALSE`.
5. **`Commit(bld2, t1)`** — `bld2` publishes the table ref `t1` (manifest tree) with **no
   fail-closed presence re-check** (structural flaw #2). The closure child `b1` is absent →
   **the committed table ref dangles**.

This matches the root-cause report exactly: **adopt-without-ownership-transfer** (owner stays
the byte-writer), **owner retires → protection lapses while a still-live adopter is in flight**,
**`GcDelete` of the in-degree-0 blob**, then **publish that references the now-deleted blob**.
It is the soak's `…28118_28624_64` → `3715345a4150` → `d3e1ba56…` dangle in the small.

## How the fix closes it (TRUE/TRUE, exhaustive clean)

- **`INV_BUILDROOT_PROTECTS`.** In the fixed model, before relying on the adopt, `bld2`
  publishes a `Precommit` → build-root edge over its tree. `PinnedByBuildRoot(b1)` then makes
  `InDegZero(b1)` false and `Protected(b1)` true, so `GcDelete`'s guard
  (`InDegZero /\ ~Protected`) can never fire on a present, precommit-reachable blob —
  protection is **structural reachability**, not a revocable liveness hint, and is independent
  of *who wrote* the bytes. (Buggy step 4 cannot occur once the precommit edge exists.)
- **`INV_COMMIT_FAILCLOSED`.** Even in the ordering-window case (blob deleted *before* the
  precommit) or after a **premature reclaim**, the fail-closed `Commit` re-verifies the whole
  closure present and otherwise takes `CommitAbort` — it never publishes a dangle (buggy step 5
  cannot occur). Premature reclaim costs work (a retry), never data.

### Premature-reclaim interleaving (witness `W_PrematureReclaimAbortReached`, reached + survived)

`WriteBlob(bld1,b1)` → **`BuildFreeze(bld1)`** (false-positive: judged dead, still running) →
`Precommit` → **`GcReclaimPrecommit`** drops the precommit of the still-live build → the adopted
`b1` is deleted → the build's real **`Commit` ABORTS** (`CommitAbort`) instead of publishing.
`everDangle` stays FALSE. This is §4.6's residual-fragility scenario, absorbed by
`INV_COMMIT_FAILCLOSED`.

### Ordering-window interleaving (the `buildrootonly` counterexample)

`WriteBlob(bld1,b1)` → **`GcDelete(b1)`** (no precommit yet → in-degree 0) → `Precommit` →
**blind `Commit`** → dangle. This is exactly why build-root reachability needs the §4.5 ordering
rule *and* the fail-closed gate: the precommit must exist before the blob can reach in-degree 0,
and the commit must re-check presence. With `FailClosedCommit=TRUE` this trace ends in
`CommitAbort`, not a dangle.

## Non-vacuity of the fixed config (reachability witnesses)

The fixed config's green result is **not** vacuous. Four NEGATED reachability probes (in the
`.tla`, run ad hoc with a witness cfg, `UseBuildRoot=TRUE`, `FailClosedCommit=TRUE`) each report
a "violation", i.e. the dangerous configuration **is** reachable and the model survives it:

| witness | proves reachable |
|---|---|
| `W_GcDeleteReached`                | `GcDelete` actually fires on an adopted blob (model not frozen pre-delete) |
| `W_BuildRootProtectReached`        | a present blob pinned *only* by a build-root edge exists (protection exercised) |
| `W_LiveFrozenReclaimDeleteReached` | a live-but-frozen build's precommit reclaimed + its adopted blob deleted (§4.6 input) |
| `W_PrematureReclaimAbortReached`   | the full chain: live frozen build → reclaim → delete → fail-closed **abort** |

All four are reported violated (reachable). Therefore TRUE/TRUE is clean over 24205 states
*because the fix works*, not because the dangerous interleavings are unreachable.

## Run

```
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
for cfg in buggy buildrootonly failclosedonly fixed ; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta-$cfg \
       -workers auto -config CaBuildRootPrecommit_$cfg.cfg CaBuildRootPrecommit.tla
done
# buggy, buildrootonly => INV_NO_DANGLE_COMMITTED violated ; failclosedonly, fixed => clean

# reachability witnesses (non-vacuity of the fixed config): use a cfg with the SAME constants
# as CaBuildRootPrecommit_fixed.cfg but a single INVARIANT W_* line; each is reported violated.
```

## B199-S2 extension — inline closure + `INV_NO_LEAK` (2026-06-23)

TLA+ gate for **B199-S2** (the never-expanded-tree leak class). Design spec:
`../specs/2026-06-23-ca-precommit-inline-closure-design.md` (§3/§4). This task models the
inline-closure protect/reclaim protocol and adds a liveness `INV_NO_LEAK`; it is the gate
**before** any C++ is written.

### The S2 leak (what this closes)

GC reclaims an object only if it appears in the in-degree **snap**, built by **expanding** the
tree object (`readTree`, recording `tree→child` edges). If the manifest tree object is **gone**
before that read (a stale/competing-leader delete — the lease is work-dedup, not safety), the
expansion 404s, the closure is **never recorded**, and on reclaim the build's unique blobs are
never released → they leak as `unreachable` debris **forever**. This is **space-only**
(`dangling=0`, not data loss). There is no safe late recovery: a condemned object must never be
GET-ed ([[feedback-ca-resurrect-invariant]]), and a deleted one can't be read at all.

**The fix (§3/§4):** the precommit records its closure **inline** at precommit time (the writer
holds the staged structure in hand — the only safe capture point). GC **seeds** the protection
edges from that **recorded** closure (no tree read, never 404s → S2 closed by construction). On
reclaim GC **mirror-drops** the closure edges; the children fall to in-degree 0 and go through the
existing retire→delete tail. A never-uploaded member is already absent → its delete is an
**idempotent no-op**.

### New constant, variables, and actions

- **`InlineClosure`** (constant): `TRUE` = fix (precommit records the full closure inline; GC
  seeds + reclaims from it; never depends on the tree object). `FALSE` = old lazy path (closure
  recorded **only if** the tree object is present to be expanded at fold time; a gone tree →
  **empty** recorded closure → the S2 leak).
- **`closure[bld]`**: the **recorded** inline closure (set of object ids). `Precommit` seeds it
  (`InlineClosure ∨ present[t] → Children(t)`, else `{}`). Protection (`BuildRootProtected`) and
  pinning (`PinnedByBuildRoot`) now source edges from `BuildRootEdges(bld) = {precommit[bld]} ∪
  closure[bld]` — **not** from re-deriving `Reach` by reading the tree. `GcReclaimPrecommit` and
  `RemovePrecommit` **mirror-drop** it to `{}`.
- **`uploaded[h]`** `[Hashes → BOOLEAN]`: the object's bytes were actually uploaded. A closure
  member can be `recorded ∧ ¬uploaded` (the partial-build / never-uploaded case); `present[h] ⇒
  uploaded[h]`. `WriteBlob`/`PublishTableRef`/`Commit` set it.
- **`everSnapped`** (monotone `SUBSET Hashes`): objects GC ever **enumerated** into the snap
  (table-reachable, or recorded in a precommit closure). `GcDelete` gains the enabling guard
  `UseBuildRoot ⇒ h ∈ everSnapped`: an object never snapped is invisible debris GC **cannot**
  reclaim. (Gated on `UseBuildRoot` so the legacy owner-hint configs reproduce their CEs
  unchanged.) The S2 leak surface is exactly: lazy path records an empty closure → child never
  enters `everSnapped` → `GcDelete` can never fire on it.
- **`staged`** (monotone `SUBSET Hashes`): the **true** staged structure of every precommit
  (`{t} ∪ Children(t)`), recorded **regardless** of `InlineClosure`. This is the
  *should-be-reclaimable* set `INV_NO_LEAK` ranges over; the fix records it into the snap, the lazy
  path drops the remainder.

Actions changed: `Precommit` (records `closure`/`everSnapped`/`staged`), `GcReclaimPrecommit` and
`RemovePrecommit` (mirror-drop `closure`), `GcDelete` (`everSnapped` visibility guard),
`WriteBlob`/`PublishTableRef`/`Commit` (set `uploaded`, accumulate `everSnapped`). No new build/GC
*actions* were added — reclaim reuses the existing edge-drop, faithful to §4 ("reclaim is the
EXISTING path, now always effective; no new walk").

### New invariants

- **`INV_NO_LEAK`** (liveness, leads-to under `FairSpec`):
  `∀ h : (h ∈ staged ∧ present[h] ∧ ¬OtherLiveRef(h)) ⟿ (¬present[h] ∨ OtherLiveRef(h))` —
  a staged member with no legitimate live reference is **eventually** either reclaimed or
  re-referenced; it never stays present-and-unreferenced forever. `OtherLiveRef(h)` = pinned by a
  table ref **or** by the build-root edges of a **still-live** build. Checked under **`FairSpec`**
  (= `Spec` plus weak fairness on `BuildDie`, `GcReclaimPrecommit`, `GcDelete`; **no** writer
  fairness — a build may abandon at any point).
- **`INV_NO_RETURN`** (safety): `∀ h : present[h] ⇒ uploaded[h]` — a reclaimed object is never
  resurrected by a GET of the condemned object; it can only come back as a fresh re-upload
  (honors [[feedback-ca-resurrect-invariant]]).
- Kept: **`INV_NO_DANGLE_COMMITTED`** (no-dangle / no-loss for the reader-facing ref),
  **`INV_BUILDROOT_PROTECTS`**, **`INV_COMMIT_FAILCLOSED`** (the NO-LOSS guarantee), `TypeOK`.

### Bounds and results (`Builds={bld1,bld2}`, `Trees={t1,t2}`, `Blobs={b1}`)

The model is finite and explored exhaustively (no depth/state-count constraint needed — well under
the ≤2 builds / ≤2 objects bound). `TLC` v2.19, OpenJDK 21, `-workers auto`.

| cfg | flags | spec | result | distinct states |
|---|---|---|---|---|
| `CaBuildRootPrecommit_fixed.cfg`          | UBR=T, FCC=T, IC=T, b1 | `Spec`     | **clean** (all 4 safety invariants) | **45161** |
| `CaBuildRootPrecommit_buggy.cfg`          | UBR=F, FCC=F, IC=T, b1 | `Spec`     | `INV_NO_DANGLE_COMMITTED` violated (CE preserved) | ~505 (to CE)* |
| `CaBuildRootPrecommit_buildrootonly.cfg`  | UBR=T, FCC=F, IC=T, b1 | `Spec`     | `INV_NO_DANGLE_COMMITTED` violated (CE preserved) | ~3031 (to CE)* |
| `CaBuildRootPrecommit_failclosedonly.cfg` | UBR=F, FCC=T, IC=T, b1 | `Spec`     | clean (closure/snap inert under UBR=F) | 2193 |
| **`CaBuildRootPrecommit_inlineclosure.cfg`** | **UBR=T, FCC=T, IC=T, b1** | **`FairSpec`** | **clean** — safety + **`INV_NO_LEAK` HOLDS** | **45161** |
| `CaBuildRootPrecommit_lazyleak.cfg`       | UBR=T, FCC=T, IC=**F**, b1 | `FairSpec` | **`INV_NO_LEAK` VIOLATED** — S2 leak reproduced | 71953 (to CE) |
| **`CaBuildRootPrecommit_inlineclosure_b2.cfg`** | **UBR=T, FCC=T, IC=T, b1+b2** | **`FairSpec`** | **clean** — safety + **`INV_NO_LEAK` HOLDS** (shared-spared + unique-reclaimed) | **310993** |

(* "to CE" counts are reported at counterexample discovery and vary slightly with parallel-worker
scheduling; the verdict — `INV_NO_DANGLE_COMMITTED` violated — is deterministic.)

No deadlock (`CHECK_DEADLOCK FALSE` on the fair configs, as the model legitimately reaches
terminal quiescent states). The legacy 2×2 safety results are **unchanged in verdict** (state
counts shifted from the extra `closure`/`uploaded`/`everSnapped`/`staged` state: `fixed` 24205 →
45161; `failclosedonly` 2193 unchanged since the new logic is inert under `UseBuildRoot=FALSE`).
The fixed config remains **non-vacuous** (witness `W_BuildRootProtectReached` still reported
reachable under the new state).

**Per-tree blob references (topology parameter).** To make "unique vs shared" expressible,
`Children(t)` is now derived from two model-value constants instead of the old `Children(t)==Blobs`:
`BuildTree` (the abandoned-precommit's manifest tree, which references all of `Blobs`) and
`UniqueToBuildTree` (blobs referenced *only* by `BuildTree`); every other tree references
`Blobs \ UniqueToBuildTree`. With `UniqueToBuildTree={}` this reproduces the original
single-shared-blob topology exactly (verified: `fixed`/`failclosedonly` state counts and all
verdicts unchanged).

### `INV_NO_LEAK` holds — and the leak is real

- **`inlineclosure` (the fix, IC=T): `INV_NO_LEAK` HOLDS** exhaustively over 45161 states. When a
  build precommits, its closure (`{b1}`) is recorded → `b1 ∈ everSnapped`. After the build is
  abandoned and reclaimed (`GcReclaimPrecommit` mirror-drops the closure), `b1` is a visible
  in-degree-0 object that `GcDelete` reclaims under fairness. A never-uploaded member is already
  absent (idempotent no-op). No staged member leaks.
- **`lazyleak` (IC=F): `INV_NO_LEAK` VIOLATED**, proving the property is non-trivial and the fix is
  load-bearing. Counterexample (6 states + stutter): `Precommit(bld2, t2)` while the tree object
  `t2` is **absent** → lazy path records `closure[bld2]={}` (empty!), so `staged={t2,b1}` but
  `everSnapped={t2}` — **`b1` is never snapped**. `WriteBlob(b1)` → both builds die →
  `GcReclaimPrecommit(bld2)`. Final state: `b1` is `present`, `staged`, unreferenced, but
  `b1 ∉ everSnapped` → `GcDelete` can **never** fire → `b1` leaks forever. This is exactly the
  never-expanded-tree S2 leak; the inline closure (IC=T) closes it by construction.

### Two-blob run: shared spared, unique reclaimed (`Blobs={b1,b2}`)

The single-blob model exercises the leak only indirectly (every tree shares the one blob). The
`inlineclosure_b2` config makes the S2 prose literal — *"the abandoned build's unique blobs leak;
shared blobs stay spared"* — with `Blobs={b1,b2}`, `BuildTree=t2`, `UniqueToBuildTree={b2}`:

- `t1 → {b1}` — a committed table ref over the **shared** blob `b1`.
- `t2 → {b1, b2}` — an abandoned precommit's manifest tree over the shared `b1` **plus** a blob
  `b2` **unique** to that build.

**Result: clean, exhaustive over 310993 distinct states** — all four safety invariants **and**
`INV_NO_LEAK` hold. Both halves are confirmed explicitly:

- **Shared spared.** `INV_NO_DANGLE_COMMITTED` guarantees `b1` (in committed `t1`'s closure) stays
  present even after `t2`'s build is abandoned and reclaimed — `t1`'s edge keeps `b1` pinned, so
  `GcDelete` never fires on it. The shared blob is spared by pure edge arithmetic (two distinct
  `tree→b1` edges; reclaiming `t2`'s does not drop `t1`'s).
- **Unique reclaimed.** `INV_NO_LEAK` guarantees `b2` (referenced only by the abandoned `t2`,
  recorded in its inline closure → snapped) is eventually reclaimed: after `GcReclaimPrecommit`
  mirror-drops `t2`'s closure, `b2` is a visible in-degree-0 object that `GcDelete` removes.

Non-vacuity: the negated reachability probe `W_SharedSparedUniqueReclaimed`
(`CaBuildRootPrecommit_b2_witness.cfg`) is reported **violated == reachable** — TLC actually reaches
a state with all builds abandoned, `b1` present (shared, table-pinned) and `b2` not present
(unique, reclaimed). So the green b2 result is *because the fix spares-and-reclaims correctly*, not
because the topology is never exercised.

### Caveat — flat manifests only (subtree recursion is gtest-covered, not modeled here)

This is a **flat** model: a tree references blobs directly, with **no nested subtrees**. The design's
subtree recursion / nested-manifest handling (design §2's `walk` recursing on `Subtree`, §3's
inline-sourced expansion of subtrees) is validated by the C++ gtest (the new nested-manifest test in
`src/Disks/tests/gtest_cas_gc_leak.cpp`), **not** by this model. A green result here is **not** a
claim about the recursion fix.

### Run

```
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
# safety 2x2 (unchanged verdicts) + the B199-S2 liveness configs (single-blob and two-blob):
for cfg in fixed buggy buildrootonly failclosedonly inlineclosure lazyleak inlineclosure_b2 ; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta-brp-$cfg \
       -workers auto -config CaBuildRootPrecommit_$cfg.cfg CaBuildRootPrecommit.tla
done
# inlineclosure, inlineclosure_b2 => clean (INV_NO_LEAK holds) ; lazyleak => INV_NO_LEAK violated.
# b2 non-vacuity: run CaBuildRootPrecommit_b2_witness.cfg => W_SharedSparedUniqueReclaimed reachable.
```
