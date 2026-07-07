---
description: 'Design for fixing the dangling-precommit manifest orphan — a content-addressed GC leak where an abandoned precommit manifest binding on a content-static ref-shard is never reclaimed because the fold Skip optimization parks the shard forever. Fold-side, watermark-keyed force-Read of shards holding a provably-dead live precommit.'
sidebar_label: 'Dangling-precommit manifest orphan fix'
sidebar_position: 51
slug: /superpowers/specs/cas-dangling-precommit-manifest-orphan-fix
title: 'CAS GC — dangling-precommit manifest orphan fix design'
doc_type: 'guide'
---

# CAS GC — dangling-precommit manifest orphan fix design {#dangling-precommit-manifest-orphan-fix}

## Problem {#problem}

Under create/insert/DROP churn the content-addressed GC can leak a part-manifest body that `fsck` classes
`unreachable` (INV-2: "outside the whole GC view — should be impossible once GC has run"), `dangling=0`
throughout — a **liveness/space leak**, not data loss. Found 2026-07-07 in the `utils/ca-soak` S30 scenario
*after* the blob `RESURRECT-REUPLOAD-ORPHAN` fix landed (that fix moved the blob residual from stuck
`unaccounted` to a draining pipeline, exposing this manifest-layer leak as the sole remaining S30 failure).
Root-caused by decoding the raw root-shard journals from the pool. It is a **distinct** bug from the blob
resurrect leak: there is no token-replace (every `manifest_delete` outcome was `deleted`, none `replaced`).

### Mechanism {#mechanism-of-the-leak}

A part-manifest is owned in a ref-shard's single ordered journal by a `RootOwnerEvent`. A normal build's
lifecycle is `precommitAdd` (owner `{Precommit, manifest_ref}`) → `promote` (owner move to `Committed`) →
`drop` (removal). GC deletes an owner-removed manifest body via the R6 exact-token delete
(`Gc::foldManifestEdges` with `sign < 0` records `(id, token)` in `mf_cleanup`, R6 deletes it after the
round CAS adopts the decrements). A bounded orphan-manifest sweep (`CasOrphanManifestSweep`) is the backstop.

The leak is an **abandoned precommit that is never reclaimed**:

1. A build precommits a manifest (`RootOwnerEvent{new_binding = {Precommit, manifest_ref}}`) whose body is
   present, so it activates and folds normally. The build is then **abandoned** — never promoted, never
   explicitly `Build::abandon`'d. `Build::~Build` only calls `store->retireBuildSeq(build_seq)` (advances the
   watermark floor); it does **not** emit a precommit removal. The dangling precommit binding stays in the
   journal.
2. With no promote and no removal, the ref-shard is **content-static** — its LIST-observed token equals the
   sealed `folded_token`, so `computeDiscoverDecisions` marks it `Skip` (classification `1` = Unchanged) and
   the fold `continue`s at the top of the shard loop **before** the `reclaimAbandonedPrecommit` call.
3. `reclaimAbandonedPrecommit` is the intended safety net, but it runs **only on a fold visit**. The
   precommit becomes provably dead only **later** — when *other* builds retiring advance the namespace's
   watermark `min_active` past this precommit's `build_sequence` — by which point the static shard is parked
   by `Skip`. So reclaim never re-runs, no removal is ever emitted, R6 never folds a `-1`, and the orphan
   sweep spares the body forever because `activeManifestKeys` keeps every un-removed `Precommit` binding in
   `precommit_live`. `fsck` follows only committed refs, so it reports the body `unreachable`.

Net: the sweep says "live (precommit)", `fsck` says "leak (unreachable)" — a permanent manifest orphan plus
the precommit's blob edges pinned at in-degree ≥ 1 (their `-1` never folds). Confirmed in S30 (manifest
`1:35:1`, part `all_0_0_0`; builds `34`/`36` for the same table completed precommit→promote→drop and were
R6-deleted) and by `precommit_reclaim` firing **0** times across the whole run + 15 extra forced rounds,
while the manifest persisted unchanged.

Root-cause sites: `Gc::fold` Skip branch (`CasGc.cpp` ~L709–724, the `continue` before the reclaim call at
~L733) and the discover decision (`Gc::computeDiscoverDecisions`, `CasGc.cpp` ~L1367). The existing
classification `4` (clamped) guard forces a revisit only for "precommit body **not yet present**"
(`CasGc.cpp` ~L869); our case has a **present** body, so it is classification `1` and skip-eligible.

## Fix {#fix}

Fold-side, watermark-keyed **force-Read** guard, reusing the existing `reclaimAbandonedPrecommit`. It is the
direct sibling of the existing clamped-guard (classification `4`): that forces a revisit when a precommit's
body appears; this forces a revisit when the watermark **proves a live precommit dead**.

> When `computeDiscoverDecisions` would mark a shard `Skip` (LIST token equals the sealed `folded_token`),
> additionally consult the sealed coverage's recorded minimal live precommit. If that precommit is dead by
> the current namespace watermark (the same predicate `reclaimAbandonedPrecommit` uses), **override the
> decision to Read**. The Read path already calls `reclaimAbandonedPrecommit` before `readShard`, so the
> removal is emitted, the fold folds the `-1` (releasing the precommit's blob edges), R6 deletes the
> owner-removed body, and the sweep stops seeing it active.

The guard is **self-terminating**: once reclaim appends the removal, the shard journal changes → its token
changes → it is Read normally next round → coverage is recomputed with the precommit gone → it becomes
skip-eligible again. Each abandoned precommit costs exactly **one** forced revisit, at the round its
`build_sequence` crosses the floor.

### What we store {#what-we-store}

`ShardCoverage` gains ONE small fact: the lexicographically-minimal `{writer_epoch, build_sequence}` among
the shard's **live** precommit bindings (a sentinel meaning "none" when there are no live precommits). The
minimum is sufficient for the decision: a precommit is dead iff `writer_epoch < w.writer_epoch` OR
(`writer_epoch == w.writer_epoch` AND `min_active > build_sequence`); every other live precommit on the shard
has `writer_epoch ≥` the minimum's, so if the lexicographic minimum is not dead, none are. The field is
computed from the same journal owner-state replay `reclaimAbandonedPrecommit` performs (accumulate
`new_binding`, subtract `old_binding`; keep `Precommit`-kind survivors), stamped on **every** Read visit
(including the clamped classification-`4` path), and carried forward verbatim on `Skip` (the existing
`ShardCoverage carried = parent_it->second` copy at `CasGc.cpp` ~L716), so it persists across arbitrarily
many skipped rounds and survives process restart (the fold seal is durable in object storage).

## Components {#components}

1. **`isPrecommitDead(writer_epoch, build_sequence, const MountLease &)`** — a shared helper extracted from
   `reclaimAbandonedPrecommit`'s death judgment, so the predicate cannot drift between reclaim (which acts on
   it) and discover (which gates the force-Read on it). Encodes control #9 exactly: dead iff older epoch, the
   farewell/retired sentinel (`min_active == UINT64_MAX`), or `min_active > build_sequence`.
2. **`ShardCoverage` + its codec** (`CasGenerationSeal.h` / `CasGcFormats`): add the optional
   `{min_live_precommit_writer_epoch, min_live_precommit_build_sequence}` pair. Fail-closed decode. No
   compatibility scaffolding (pre-release, no persisted data — see the CA no-compat-scaffolding rule).
3. **`Gc::fold`** (`CasGc.cpp` ~L789, where `cov` is stamped): compute the minimal live precommit from the
   journal replay and write it into `cov`.
4. **`Gc::computeDiscoverDecisions`** (`CasGc.cpp` ~L1367): for a shard that is otherwise `Skip`, override to
   `Read` when the sealed coverage's minimal live precommit satisfies `isPrecommitDead` against
   `floorForNamespace(ns)`.
5. **Observability**: a `ProfileEvents` counter `CasGcPrecommitRevisitForced` incremented per forced
   revisit; the existing `PrecommitReclaim` CA-log event now actually fires on this path.

## Data flow {#data-flow}

discover reads the parent seal → for each Skip-eligible shard, if its sealed coverage records a
watermark-dead minimal live precommit → **Read** → `reclaimAbandonedPrecommit` appends the removal
(`old = the precommit binding, new = none`) → the fold re-reads the shard and folds the `-1` in the same
round (releasing the precommit's blob edges toward zero in-degree) → coverage recomputed (no live precommit →
field cleared) → R6 exact-token-deletes the now owner-removed manifest body → the orphan sweep no longer sees
it in `activeManifestKeys`. Self-terminating.

## Safety {#safety}

- **No live precommit is ever reclaimed.** The force-Read gate uses the exact conservative watermark fact
  `reclaimAbandonedPrecommit` already uses (control #9): a build at/above the floor, a future epoch, or a
  missing watermark is spared. The `Build::promote` guard remains the backstop — a wrongly-reclaimed
  precommit fails promote closed (`ABORTED`), never republishes a committed ref over reclaimed blobs.
- **Monotone-GC / skip invariant preserved.** force-Read is strictly **more** reading than `Skip`, never
  less; it cannot cause an in-degree under-count. It adds only a bounded, self-terminating set of reads
  (one revisit per abandoned precommit, at the crossing round).
- **Optimization preserved.** A shard with no live precommit, or with a live-but-not-yet-dead precommit,
  stays `Skip`. Only a shard whose recorded minimal live precommit is provably dead is forced to Read, once.
- **Idempotency under R5-retry.** The removal is keyed on the exact binding. A lost round CAS re-runs on the
  unchanged shard (whose carried coverage still records the dead precommit) → the same force-Read → the same
  removal; the `-1` folds once (the fold cursor advances past that `transition_version`).
- **Seal determinism.** The new coverage field is a pure function of the shard journal → seal determinism and
  crash-replay adoption hold.

## TLA+ {#tla}

A focused model in the incarnation/GC family (the approach that worked for the blob fix): scenario
`SkipParksDeadPrecommit` — a precommit activates, its shard goes content-static (skip-eligible), the
watermark advances past its `build_sequence`. Liveness property (the manifest analog of the blob
`NoLeakForever`): a present, unreachable, abandoned-precommit manifest is eventually reclaimed under weak
fairness of the fold. The bug config (skip parks the shard) **violates** it; the fix config (force-Read on a
watermark-dead live precommit) **holds**. Gate before the C++ change: bug cfg violates, fix cfg holds.

## Testing {#testing}

TDD, unit-first, in the CA GC gtest suite (`src/Disks/tests/`):

1. **RED — dangling-precommit reclaimed:** `precommitAdd` a manifest and activate it (fold), do **not**
   promote or abandon, advance the watermark past its `build_sequence` (`retireBuildSeq` of a later build),
   run GC to fixpoint. Assert before the fix: the manifest is present + `unreachable` and `precommit_reclaim`
   never fired. After the fix: reclaim fires, the manifest is deleted, the precommit's blob edges are
   released, and `fsck` is clean for it.
2. **Idempotency:** drive extra rounds after reclaim; assert no repeated reclaim and no duplicate removal
   event.
3. **Optimization preserved:** a shard with a live (not-yet-dead) precommit stays `Skip` (no forced Read); a
   shard with no precommit stays `Skip`.
4. **Scenario regression:** the S30 `utils/ca-soak` scenario loses its `unreachable` manifest residual (0
   after the graduation drain) and passes.

## Out of scope {#out-of-scope}

- `INTROSPECTION-1` — emitting `ManifestPut` / `PrecommitRemoved` and the manifest owner-transition audit
  events (separately backlogged in `utils/ca-soak/scenarios/BACKLOG.md`).
- `INTROSPECTION-2` — a `clickhouse-disks ca-inspect` decoder for CA bucket objects (separately backlogged).
- The blob `RESURRECT-REUPLOAD-ORPHAN` fix (already landed and verified).
- Hardening `activeManifestKeys` on the sweep side to also judge precommits dead (a defense-in-depth option
  considered and deferred — it would reclaim the manifest body but not release the precommit's pinned blob
  edges, so it does not replace the fold-side fix).

## Docs to update after the fix lands {#docs-to-update}

`docs/superpowers/cas/06-tla-models.md` (add the focused model; mark the C++ fix landed); any CA GC design
doc describing the fold Skip / discover decisions (state that a shard holding a watermark-dead live precommit
is force-Read so `reclaimAbandonedPrecommit` can run); and `utils/ca-soak/scenarios/BACKLOG.md`
`DANGLING-PRECOMMIT` (mark resolved once S30 goes green).
