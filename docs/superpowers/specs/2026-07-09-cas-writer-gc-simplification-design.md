# CAS writer↔GC protocol simplification (Tier 2): EDGE-BEFORE-OBSERVE makes the freshness machinery redundant

**Date:** 2026-07-09
**Branch:** `cas-gc-rebuild`
**Status:** design (user-driven; approved in discussion 2026-07-09)
**Supersedes:** backlog PROMOTE-REVALIDATION-MINIMIZATION variant A (HEAD-skip) and variant B (pinned-round
ack) — both are strictly weaker than this design's justification.
**Relation to landed work:** the two condemn-race fixes (spec `2026-07-09-cas-promote-resurrect-tokened-blob`
and `2026-07-09-cas-promote-tokenless-copyforward-race`) made the promote gate non-fatal. This spec removes
the tokened half of that gate entirely — the fixes were the correct transition, this is the destination.

## Motivation {#motivation}

The writer↔GC interaction accreted across redesigns (EBR → incarnation tokens → ack-floor → root-local
manifests → early precommit): retire-view syncing, a view-install drain (`view_gate`), a fence-round
mid-closure refresh, per-leaf promote revalidation with resurrect machinery, retained sources. Each layer
independently guards against blob deletion racing an in-flight commit. The user observed — and code review
confirms — that after the B188 reordering the **write-path ordering itself** provides the protection, and
most of the freshness machinery is redundant defense-in-depth. Goal: maximal simplification preserving the
core guarantees (`INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`), each deletion proven redundant by TLA+.

## The load-bearing invariant: EDGE-BEFORE-OBSERVE {#edge-before-observe}

Current write order (`ContentAddressedTransaction::publishStaging`, `ContentAddressedTransaction.cpp:227-255`):

```
stageManifest      (manifest body durable — fold activation requires body present)
→ precommitAdd     (create-precommit RootOwnerEvent durable in the shard journal;
                    its closure names EVERY blob hash of the staged manifest)
→ putBlob loop     (the FIRST backend observations/adoptions happen here)
→ promote          (owner move)
```

**Every observation happens under an already-durable protecting edge.** Combined with three existing GC
mechanisms — fold activation of the precommit closure when the body is present (`CasBuild.cpp:901-905`),
per-hash in-degree settlement with `d > 0 → spared` ("recovery wins even past the floor",
`CasBlobInDegree.cpp:199`, `CasBlobInDegree.h:87`), and the two-phase delete that re-computes `d` at BOTH
the graduation pass and the delete pass (`d > 0` on a pending entry ⇒ impossible-spared, loud,
`CasBlobInDegree.h:84-90`) — this yields the theorem:

> **A hash named in a durable precommit closure cannot be deleted by GC.** Deletion requires `d = 0` at two
> consecutive passes; any pass whose journal seal is taken after `precommitAdd` folds the closure edge ⇒
> `d ≥ 1` ⇒ spared (the condemned entry is dropped).

Case analysis for a blob observed by `putBlob`/named by the manifest:

- **Condemned after `precommitAdd`** (the 01156/01710/02346/03283 window): the condemning pass missed the
  edge (sealed before the append); the next pass folds it ⇒ spared. Can never graduate. The condemnation is
  real but doomed — the promote gate was aborting (now resurrecting) an object that was never in danger.
- **Condemned AND graduated before `precommitAdd`** (pre-existing `delete_pending`): the entry is in the
  writer's installed retire view (pendings stay listed until confirmed) ⇒ `putBlob`'s gate refuses the token
  and re-uploads fresh (INV-1). If the delete already executed ⇒ `putBlob` HEADs absent ⇒ fresh upload.
- **Delete pass racing `precommitAdd`**: sealed before the append ⇒ object deleted ⇒ `putBlob` sees absent
  (fresh upload); sealed after ⇒ edge folded ⇒ `d > 0` on pending ⇒ impossible-spared (loud log).

All cases close without any promote-time revalidation for tokened deps. The tokenless (`adoptEvidence`)
case differs in exactly one respect: B188 made adoption observation-free (no HEAD, no backend call), so
**nobody has ever observed presence** — one presence check is still required somewhere (kept at promote).

## Scope {#scope}

**Tier 2 (this spec): the writer side, whole.** GC-side mechanisms (ack floor, graduation gating, two-phase
delete, supersede bookkeeping, syncer cadence) are untouched. **Tier 3 (backlogged):** ack-floor graduation
gating revision, beat round-ack, tokenless condemned-arm → accept (weakens the modeled publish gate),
syncer removal. Tier 3 starts only after Tier 2 lands plus one clean soak.

## Deletions {#deletions}

| # | Mechanism | Anchor | Justification |
|---|---|---|---|
| D1 | Promote per-leaf HEAD + bounded resurrect loop for **tokened** deps | `CasBuild.cpp` revalidation loop, tokened arms (`src` branches) | EDGE-BEFORE-OBSERVE + the putBlob gate (which observes under the durable edge) cover every case |
| D2 | `retained_sources` map, `retainedSourceFor`, the `putBlob` `insert_or_assign` | `CasBuild.h:167-area`, `CasBuild.cpp:137-139, 216-221` | Existed only to feed D1 |
| D3 | Copy-forward **pre-pass** | `CasBuild.cpp:816-829` | One tokenless gate remains (K3); two code paths for one job collapse to one. `copyForwardFromCondemned` keeps a single caller |
| D4 | `view_gate` drain (shared/exclusive `shared_mutex`) | `CasStore.h:622-626`, `CasStore.cpp:690, 1243` | The drain made the advertised round an upper bound over in-flight gate rounds; after D1 no deletion-safety decision gates on the view inside a mutation. The syncer installs under `RetireView`'s internal mutex (thread-safety was never `view_gate`'s job) |
| D5 | Writer-side `fence_round` mid-closure view refresh | `CasBuild.cpp:856-857` | Blob-side freshness is edge-protected. **TLA+-gated** (see the gate; the shard-incarnation model decides). GC-side `fence_round` uses stay: round recovery (`CasGc.cpp:1872`), `birth_floor_provider` install (`CasStore.cpp:1319`), codec/inspect |
| D6 | `DepEntry::observed_view_round` | `CasBuild.h:110` | Already dead — declaration only, zero readers and writers |
| D7 | Backlog entries: variant A (HEAD-skip-on-unchanged-round) and variant B (pinned-round ack) | `utils/ca-soak/scenarios/BACKLOG.md` | Superseded: the pin is strictly weaker than the durable edge (a pin bounds the floor; the edge makes deletion impossible regardless of the floor) |

**No paranoid mode.** The deleted checks are not kept behind a config flag. Rationale: (a) CLAUDE.md —
fallback paths hide bugs; a gated second protocol must be maintained and tested forever; (b) honest
re-assessment of the clamp-suppression incident (31 dangles live, `CasBlobInDegree.h:91`): it hit
**committed** manifests with no in-flight build — the promote gate would not have caught it then and cannot
catch that class. Detection of accounting bugs belongs to `ca-fsck`, the soak assertions, and
`system.content_addressed_log`, all of which remain.

## Kept (and why) {#kept}

| # | Mechanism | Why it stays |
|---|---|---|
| K1 | `putBlob` gate, whole: `observeAndAdmit` condemned check against the installed view, condemned → `uploadFromSource` (INV-1, W-FRESH-TAG, bounded retry), HEAD-first dedup | **Safety-critical — the dedup-adoption race.** A hash condemned+graduated BEFORE our `precommitAdd` (entry `delete_pending` in state N) can be adopted present-but-doomed: pass N+1 seals before our append → its fold misses our edge → `d = 0` → `deleteExact` executes AFTER our HEAD observed the object present → with no promote HEADs (D1), the commit dangles. Neither EDGE-BEFORE-OBSERVE (graduation predates the edge) nor the two-phase re-check (its fold sealed before our append) covers this interleaving — ONLY the condemned check does. It is race-safe by exact-token discipline: our `putOverwrite(If-Match)` displacement either beats the delete (delete misses — token changed) or loses (412 → re-observe → absent → fresh upload). The GC-side ack floor is what GUARANTEES the check sees the entry: graduation requires `min_ack > condemn_round`, so every live writer's installed view covers every graduated entry (entries persist until confirmed outcomes). Post-Tier-2 the retire view IS the dedup-safety list and the floor IS its delivery guarantee — single purpose each |
| K2 | Promote owner-liveness check + body read + `RefMatchesBody` + namespace match | Owner-move correctness (`WPromote owner==bld`); unrelated to blob freshness |
| K3 | Promote: ONE HEAD per **tokenless** leaf; absent ⇒ `ABORTED`; condemned-present ⇒ `copyForwardFromCondemned` (the landed in-closure backstop, `CasBuild.cpp:947-949`) | `adoptEvidence` is observation-free by design (B188) — this is the single mandatory presence observation, AND the tokenless twin of K1's dedup-adoption race: a pre-precommit-graduated entry can be present-but-doomed here too, and only the condemned check + exact-token displacement close it. Revising the condemned arm to accept is Tier 3 — and per K1's analysis that revision is bounded by the same race, not free |
| K4 | Mount lease + merged beat (`min_active`, `observed_gc_round`), TTL fence-out, local write fence, epoch/self-remount | Liveness backbone; also what bounds dead processes. The beat still advertises the installed round — the GC-side floor semantics are untouched in Tier 2 |
| K5 | Retired-view syncer (minus the drain) | The GC-side ack floor still consumes advertised rounds; the view must keep advancing for K1's gate quality and floor currency |
| K6 | GC side, whole: ack floor R1, fold, two-phase graduation, spare rules, supersede/`ReplacedEntry`, clamp suppression handling | Out of scope (Tier 3 candidates); two-phase `d`-recheck is a pillar of the theorem |
| K7 | Read path | Untouched — the retire view is writer-only (verified: no consumers outside Build/Store/Gc) |

**Promote cost after Tier 2:** body GET + (tokenless-leaf count) HEADs + shard CAS. For `INSERT` (all leaves
tokened): **zero** revalidation HEADs.

## Ordering becomes a checked protocol invariant {#ordering-invariant}

EDGE-BEFORE-OBSERVE makes the `stageManifest → precommitAdd → putBlob` order load-bearing. Guards:

- A comment block at `publishStaging`'s precommit site and at `Build::putBlob`, each referencing this spec
  and the TLA+ order-sabotage config, stating that observations before the durable closure reintroduce the
  deletion race.
- A debug assertion (`chassert`) in `Build::putBlob`'s **adopt paths only** (the HEAD-first admit and the
  `observeAndAdmit` dedup-adopt): `precommitted == true`. Rationale: adopting an EXISTING pool incarnation
  without a durable closure edge is the exact unprotected shape (the adopted blob carries the original
  writer's `build_id`, so the debris watermark does not cover it). A FRESH upload before precommit stays
  legal — it is protected by the existing newborn-debris watermark (`build_id` + `min_active`), which is
  why the old harness order (`putBlob` fresh payloads → `stageManifest` → `precommitAdd`) remains valid.
  Tests that dedup-adopt pre-precommit are rewritten to the wiring order in Phase 1.
- The TLA+ order sabotage (below) is the formal guard.

## TLA+ phase-0 gate {#tla-gate}

Technique: **redundancy proof by sabotage-flip** — a config that today MUST fail proves a mechanism
load-bearing; after adding EDGE-BEFORE-OBSERVE to the model, the same sabotage passing proves the mechanism
redundant. Base models: `CaBuildRootPrecommit.tla` (precommit inline closure, fold, `GcDelete`, in-degree)
+ `CaIncarnationCore.tla` (tokens, retire view, publish gate); expect a small composition or an extension
of one of them.

1. **Model extension:** an explicit writer order — durable closure append precedes every observation/adopt
   action; fold activates closure edges (body present ⇒ activated); settlement spares `d > 0`; two-phase
   delete re-checks `d`.
2. **Must FLIP to green (redundancy proofs):** the stale-view publish sabotages that today dangle —
   `SabotageNoReobserve` (reval re-observation) and, for D5, the newborn/THM-NO-RETURN writer-refresh case
   checked against the shard-incarnation model (`CaGcShardIncarnationCore` family). If the newborn sabotage
   does NOT flip, D5 is cancelled and the refresh stays (the spec's only conditional deletion).
3. **Must STAY red (the new load-bearing points):** (a) a NEW order sabotage — observations/adoptions
   before the durable closure append (models the pre-B188 order) — must dangle, proving the ordering is the
   invariant; (b) the tokenless-absent case with the K3 HEAD removed must dangle, proving K3 load-bearing;
   (c) the K1 sabotage — dedup-adoption without the condemned check (`SabotageNoRetireView` exists in
   `CaIncarnationCore`) — must dangle IN THE REDUCED MODEL via the pre-precommit-graduated interleaving
   (K1's race), proving the condemned list + floor pair remains load-bearing after Tier 2.
4. **Positive run:** the reduced model (no tokened revalidation, no drain, no writer fence-refresh) holds
   `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, `MonotoneGC`.

## Implementation phases {#implementation-phases}

1. **Phase 0:** the TLA+ gate above. No code before the gate is green (D5 conditionally resolved here).
2. **Phase 1:** D1+D2+D3 (promote reduction) + D6, with test migration:
   - `CaWiringResurrect.PromoteResurrectsCondemnedTokenedBlob` → replaced by the new contract test:
     promote **succeeds without touching the blob** (token unchanged — no resurrect PUT); the condemned
     entry's fate (spare at next fold) is GC's and is already covered by the GC settlement gtests.
   - `CaWiringResurrect.PromoteAbandonedPrecommitAbortsWithoutResurrect` → kept (owner-check abort), asserts
     simplified (no resurrect machinery to negate).
   - `CasProtocol.FenceConflictCondemnedTokenedBlobResurrectsFromSource`,
     `…WedgedHeartbeatCondemnedTokenedBlobResurrectsFromSource`,
     `…RevalidateReObservesStaleTokenKeepsWhenUnchanged` → rewritten to the new contract: promote succeeds,
     no re-upload, token unchanged.
   - `CasProtocol.RevalidateAbsentTokenedBlobResurrectsFromSource` → **deleted**: its premise (a
     `putBlob`'d blob hand-deleted before promote) is protocol-unreachable under EDGE-BEFORE-OBSERVE — only
     out-of-band corruption produces it, and out-of-band corruption is `ca-fsck`'s domain, not promote's.
     Documented in the test file header.
   - Tokenless tests (`EvidenceHit…CopiesForwardInClosure`, `PromoteCopiesForwardCondemnedEvidence…`,
     `PromoteAbsentTokenlessBlobAbortsRetryable`, `PromoteCondemnedLeafWithoutDepAbortsFailClosed`) → kept
     as-is (K3 contract, unchanged).
   - The ordering guards from §Ordering.
3. **Phase 2:** D4 (drain removal): syncer installs under `RetireView`'s internal mutex; `flushShardBatch`
   drops the shared lock; delete the member + comments referencing the drain contract.
4. **Phase 3:** D5 if Phase 0 flipped it; D7 backlog supersede notes (may land with Phase 1).
5. **Validation:** full `Ca*/Cas*` gtests (no growth of the known-flaky `CaWiring*` set) → the 4 stateless
   condemn-race tests + the full CA-s3 lane (expect: 0 promote aborts, 0 fsck dangles) → a soak run with
   the fsck gate (the real judge of writer↔GC interplay).

## Non-goals {#non-goals}

- Any GC-side change (Tier 3).
- Any read-path change.
- Compatibility scaffolding: CAS is pre-release with no persisted deployments (standing project rule); the
  removed mechanisms leave no on-disk traces requiring migration (`view_gate`, retained sources, and the
  revalidation loop are all in-memory; `observed_view_round` was never serialized).

## Tier 3 backlog (recorded separately in `BACKLOG.md`) {#tier-3}

**Corrected after the K1 race analysis:** the ack floor is NOT simply redundant — post-Tier-2 it is the
delivery guarantee of the dedup-safety list (graduation waits until every live writer's view covers the
entry), and K1's adoption gate is unsound without it. Tier 3 is therefore a package deal: removing the
floor (and the beat round-ack + the syncer as its feeder) requires replacing dedup-adoption safety
wholesale — e.g. adopt-displace (every dedup re-uploads under a fresh tag: kills dedup's bandwidth value)
or no-adoption modes for small deployments. Tokenless condemned-arm → accept is bounded by the same race.
Failure-texture review (stale views ⇒ more loud impossible-spares) rides along. Each item needs its own
sabotage-flip demolition; several may conclude "keep". Precondition: Tier 2 landed + one clean soak.
