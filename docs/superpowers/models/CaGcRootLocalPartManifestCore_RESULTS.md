# CaGcRootLocalPartManifestCore — Green-Suite Ledger (R0 Gate)

Model: `CaGcRootLocalPartManifestCore.tla` — the root-local part-manifest GC protocol
(spec `2026-06-26-cas-gc-streaming-sharded-redesign-design.md`, rev. 15). TLC `tla2tools.jar`
v2.19 / OpenJDK 21. Run via `./run_gc_partmanifest.sh <cfg-basename>`.

A **HOLD** row is correct iff TLC prints `Model checking completed. No error has been found.`
(exit 0). A **`_sab_*`** row is correct iff TLC prints `Error: Invariant <NAME> is violated.`
or `Temporal properties were violated.` (nonzero exit) — it MUST fail. A `_sab_*` that PASSES
is a gate failure (`UNEXPECTED PASS`). A **witness** row is correct iff TLC reports the negated
`W_*` invariant violated (the dangerous-but-safe state is reachable ⇒ the positive stages are
not vacuous).

## Positive stages + liveness (must HOLD)

| Config | Result | States generated | Distinct states | Wall time |
|---|---|---|---|---|
| `stage0` (TypeOK + journal coverage) | HOLD | 71,184 | 19,846 | 0s |
| `stage1` (identity + body validation + no-reuse) | HOLD | 1,659,466 | 402,034 | 2s |
| `stage2` (owner transitions + precommit + promote) | HOLD | 377,632,669 | 68,550,326 | 7m11s |
| `stage3` (GC pipeline: fold/retire/fence/recheck/delete/trim) | HOLD | 1,926,070,427 | 365,609,430 | 27m45s |
| `stage4` (manifest cleanup + orphan sweep + mutable) | HOLD | 134,769,744 | 27,396,110 | 3m29s |
| `stage5_tokendiff` (token-diff discovery: `GDiscoverSkip`/`GDiscoverRead`, `EnableTokenDiff`+`TokenObservable`) | HOLD | 48,552,772 | 8,327,064 | 28s |
| `stage5_lazytrim` (Phase 3 lazy trim: `EnableLazyTrim`, all-shard fresh fence stays the only behavior; bounded `{n1,n2}`, 1 shared blob, precommit/missing-body off — feasible cross-namespace shared-blob in-degree coverage) | HOLD | 2,046,375,017 | 338,817,903 | ~21m |
| `live` (`FairSpec`: `OrphanManifestDebrisDrains` + `NoLeakForever`) | HOLD | 74,147,107 | 17,845,340 | 30m10s |

Invariants proven across stage3/stage4: `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`,
`INV_JOURNAL_COVERAGE`, `NoManifestIdReuse`, `RefMatchesBody`, `ManifestNamespaceMatches`,
`SingleManifestOwner`, `CommittedManifestBodyRequired`, `CommittedNoMissingBlob`,
`NoCommittedDangle`, `BlobInDegreeMatchesActiveManifests`, `FoldedEdgesAreActive`,
`ManifestActivationMatchesEdges`; property `MonotoneGC`.

## Negative controls (all 25; must VIOLATE the named invariant)

| # | Config (`_sab_*`) | Spec control | Result (violated) | Distinct states |
|---|---|---|---|---|
| 1 | `reusemanifestid` | reuse a `ManifestId` | `INV_NO_LOSS` | 649,784 |
| 2 | `twoowners` | two owners / sharing a manifest | `INV_NO_LOSS` | 575,297 |
| 3 | `splitpromote` | promote = two CAS with a gap, no fail-closed | `INV_NO_DANGLE` | 471 |
| 4 | `missingbodyactivated` | missing precommit body treated as activated | `INV_NO_LOSS` | 563,247 |
| 5 | `commitskipblobreval` | committed publish skips blob revalidation | `INV_NO_DANGLE` | 155 |
| 6 | `precommitlessprotect` | precommitless upload treated as protected | `INV_NO_DANGLE` | 65,784 |
| 7 | `noorphansweep` | omit pre-precommit debris sweep | `OrphanManifestDebrisDrains` | 1,516,076 |
| 8 | `wholesaleprefixdelete` | wholesale delete of an eligible build prefix | `INV_NO_DANGLE` | 1,927 |
| 9 | `frozenseqauthority` | `sweepEligible` from frozen-seq heuristic | `INV_NO_DANGLE` | 2,068 |
| 10 | `missingcommittedempty` | missing committed body treated as empty | `INV_NO_LOSS` | 69,469 |
| 11 | `deletebodybeforedecrements` | delete body before decrements durable | `NoLeakForever` (live) | 8,748,225 |
| 12 | `cutoverclaim` | cursor past unsealed deltas | `INV_NO_DANGLE` | 155,369 |
| 13 | `roundvisibilityearly` | round visible after partial retire | `INV_NO_DANGLE` | 46,607 |
| 14 | `nofence` | skip global fence for a racing publish | `INV_NO_DANGLE` | 32,631 |
| 15 | `trimunincorporated` | trim below an unincorporated transition | `INV_JOURNAL_COVERAGE` | 317 |
| 16a | `unconddelete` | non-exact delete | `INV_NO_DANGLE` | 152,490 |
| 16b | `reusedtag` | reuse blob tokens | `INV_NO_RETURN` | 29,047 |
| 17 | `barenonce` | bare instance-id (no full `ManifestRef`) | `INV_NO_LOSS` | 58,603 |
| 18 | `keybyrefnotid` | key edges/cleanup by ref not `ManifestId` | `INV_NO_LOSS` | 6,256,736 |
| 19 | `acceptnamespacemismatch` | accept body ns ≠ owning ns | `INV_NO_DANGLE` | 250,010 |
| 20 | `acceptrefmismatch` | accept body ref ≠ journal ref | `INV_NO_LOSS` | 42,927 |
| 21 | `mutableasreachability` | mutable update mints id / emits deltas | `INV_NO_LOSS` | 129,884 |
| 22 | `promoteaftermissingbody` | promote-as-move after missing-body | `INV_NO_LOSS` | 425 |
| 23 | `advancepastmissingbody` | fold advances past live missing-body precommit | `INV_NO_DANGLE` | 570,004 |
| 24 | `skipchangedshard` | token-diff skip of a CHANGED root shard (`listedTok # foldedTok`) — cut-overclaims the fold cursor past unfolded activations | `INV_NO_DANGLE` | 160,227 |
| 25 | `lazyfenceunsafe` | reuse a STALE parent fence position for a shard that got a publish between discovery and recheck (no all-shard fresh fence) — permanent control: why a lazy fence is deliberately not implemented | `INV_NO_DANGLE` | 24,540,205 |

No `UNEXPECTED PASS`. All 25 controls (26 cfgs, #16 split a/b) reproduce their named counterexample.

## Non-vacuity witnesses (must be REACHABLE ⇒ the negated `W_*` is reported violated)

| Witness config | Negated invariant reported violated | Reachable state proven |
|---|---|---|
| `witness_precommitmissingbody` | `W_PrecommitMissingBodyReached` | a live precommit with an absent manifest body |
| `witness_committedoverfoldedblob` | `W_CommittedOverFoldedBlob` | a committed manifest with a folded active blob edge |
| `witness_orphandeleted` | `W_OrphanDeleted` | a staged-unowned body actually swept (orphan deleted) |

## Verdict

**SUITE GREEN.** Every positive stage (including Phase 2 `stage5_tokendiff`), the liveness config, and
all three witnesses behave as required, and all 25 negative controls produce their named counterexample
with no `UNEXPECTED PASS`. The R0 safety gate is satisfied: `INV_NO_DANGLE`, `INV_NO_LOSS`, and
`INV_NO_RETURN` are proved by the model (stage3/stage4/stage5_tokendiff), liveness holds under
`FairSpec`, and the protocol's load-bearing rules are each shown necessary by their sabotage
counterexample.

### Phase 2 — token-diff discovery (rev.15 token split) {#phase-2-token-diff-discovery}

The rev.15 token split adds two per-namespace variables: `listedTok` (the live root-shard token discovery
observes from LIST; advanced by every owner-transition action, capped at `MaxToken`) and `foldedTok` (the
sealed `ShardCoverage.folded_token`; advanced ONLY by the fold-seal write `GDiscoverRead`, never by
discovery). `GDiscoverSkip` claims a shard's fold coverage (advances the fold cursor to the journal end)
WITHOUT reading the body, guarded by `listedTok[n] = foldedTok[n]`; `GDiscoverRead` seals
`foldedTok[n] := listedTok[n]` only once the fold has caught up (`cursor[n] = Len(journal[n])`). The
token machinery is gated behind `EnableTokenDiff`, so every pre-Phase-2 stage is byte-for-byte unchanged
(`stage0` reproduces its exact 71,184 / 19,846 baseline). The fence is untouched: `GFenceShard` still
fences every shard every round regardless of any skip.

`stage5_tokendiff` (positive, `EnableTokenDiff = TRUE`, `TokenObservable = TRUE`, all `Sabotage* = FALSE`)
HOLDs `INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN` (+ the full stage4 invariant set): skipping an
UNCHANGED shard is a no-op because its cursor is already caught up, so it folds nothing and loses nothing.
`sab_skipchangedshard` (`SabotageSkipChangedShard = TRUE`) drops the equality guard so `GDiscoverSkip` may
fire on a shard whose listed token advanced past the folded token; the cursor jump then outruns the
unfolded committed activation (the `cutoverclaim` failure mode reached through token-diff), the committed
manifest's blob looks in-degree 0, GC over-deletes it, and `INV_NO_DANGLE` is violated. The new configs
use a small scope (`MaxToken = MaxLog = 2`) chosen so `listedTok` saturates exactly when the journal is
full — required for soundness, since a saturated `listedTok` that still equalled a stale `foldedTok` while
unfolded records remained would mint a spurious skip.

### Phase 3 — lazy trim (all-shard fresh fence stays load-bearing) {#phase-3-lazy-trim}

Phase 3 adds two per-namespace variables: `foldTok` (the abstract persisted folded token a fence was
recorded against) and `prevFencePos` (the parent generation's fence position a sabotaged reuse copies).
Both writes are **GATED behind `SabotageLazyFenceUnsafe`** — they are the ONLY consumer (`shardUnchanged`
and the stale-reuse branch live exclusively in that sabotage arm), so in every non-sabotage stage both
vars stay constant 0 and add NO state. This keeps the pre-Phase-3 suite byte-for-byte identical:
`stage0`/`stage1`/`stage2` reproduce their EXACT Phase-2 distinct counts (19,846 / 402,034 / 68,550,326),
proving the new vars are inert — so `stage3`/`stage4`/`live` and every pre-existing control are unchanged
and carried forward without re-running. `EnableLazyTrim` does NOT introduce a positive lazy-fence arm: the
all-shard FRESH fence (`GFenceShard` advancing `fencePos[n] := Len(journal[n])`) stays the ONLY
non-sabotage behavior. Lazy trim may let trim work lag, but the fence is fenced afresh on every shard every
round — there is deliberately no honest "reuse the parent fence" path.

`stage5_lazytrim` (positive, `EnableLazyTrim = TRUE`, all `Sabotage* = FALSE`) HOLDs `INV_NO_DANGLE`/
`INV_NO_LOSS`/`INV_NO_RETURN`/`INV_JOURNAL_COVERAGE`/`BlobInDegreeMatchesActiveManifests`/`TypeOK`. It is
scoped `Namespaces = {n1, n2}` with a single shared blob (`Blobs = {b1}`) and precommit/missing-body off —
a deliberately BOUNDED config (the full-feature `{n1,n2}` cross-product is multi-billion states and does
not converge), chosen to add genuine **cross-namespace shared-blob in-degree** coverage (a blob referenced
from manifests in two namespaces must stay alive while either references it) on top of the single-namespace
pipeline proof carried from `stage3`. 338,817,903 distinct states, ~21m.

`sab_lazyfenceunsafe` (`SabotageLazyFenceUnsafe = TRUE`) is a PERMANENT negative control documenting WHY a
lazy fence is dropped. Its `GFenceShard` branch fires for a shard that got a publish between fence discovery
and recheck (`~shardUnchanged(n)`, modeling a concurrent publish that bumps `foldTok`) and REUSES the stale
parent fence position `prevFencePos[n]` instead of advancing — so the racing activation is left below the
fence. `GRecheckDelete`'s `FoldedThroughFence` gate (`cursor[n] >= fencePos[n]`) then passes against the
stale low fence before the fold has consumed the racing publish, the freshly-committed manifest's blob looks
in-degree 0, GC over-deletes it, and `INV_NO_DANGLE` is violated (24,540,205 distinct states, ~1m). This
is exactly why the all-shard fresh fence is load-bearing and a reused (lazy) fence is not implemented.
