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
