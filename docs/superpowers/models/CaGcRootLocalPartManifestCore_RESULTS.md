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
| `live` (`FairSpec`: `OrphanManifestDebrisDrains` + `NoLeakForever`) | HOLD | 74,147,107 | 17,845,340 | 30m10s |

Invariants proven across stage3/stage4: `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`,
`INV_JOURNAL_COVERAGE`, `NoManifestIdReuse`, `RefMatchesBody`, `ManifestNamespaceMatches`,
`SingleManifestOwner`, `CommittedManifestBodyRequired`, `CommittedNoMissingBlob`,
`NoCommittedDangle`, `BlobInDegreeMatchesActiveManifests`, `FoldedEdgesAreActive`,
`ManifestActivationMatchesEdges`; property `MonotoneGC`.

## Negative controls (all 23; must VIOLATE the named invariant)

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

No `UNEXPECTED PASS`. All 23 controls (24 cfgs, #16 split a/b) reproduce their named counterexample.

## Non-vacuity witnesses (must be REACHABLE ⇒ the negated `W_*` is reported violated)

| Witness config | Negated invariant reported violated | Reachable state proven |
|---|---|---|
| `witness_precommitmissingbody` | `W_PrecommitMissingBodyReached` | a live precommit with an absent manifest body |
| `witness_committedoverfoldedblob` | `W_CommittedOverFoldedBlob` | a committed manifest with a folded active blob edge |
| `witness_orphandeleted` | `W_OrphanDeleted` | a staged-unowned body actually swept (orphan deleted) |

## Verdict

**SUITE GREEN.** Every positive stage, the liveness config, and all three witnesses behave as
required, and all 23 negative controls produce their named counterexample with no `UNEXPECTED PASS`.
The R0 safety gate is satisfied: `INV_NO_DANGLE`, `INV_NO_LOSS`, and `INV_NO_RETURN` are proved by
the model (stage3/stage4), liveness holds under `FairSpec`, and the protocol's load-bearing rules are
each shown necessary by their sabotage counterexample.
