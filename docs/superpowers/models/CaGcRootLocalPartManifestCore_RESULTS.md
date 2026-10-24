---
description: TLC evidence for the root-local part-manifest GC core, and the verdict on the retired token-diff discovery skip.
sidebar_label: Root-local part-manifest GC results
sidebar_position: 1
slug: /superpowers/models/ca-gc-root-local-part-manifest-core-results
title: CaGcRootLocalPartManifestCore — TLA+ gate results
doc_type: reference
---

# `CaGcRootLocalPartManifestCore` — TLA+ gate results {#ca-gc-root-local-part-manifest-core-results}

The model covers the root-local part-manifest GC core: single-owner
`ManifestId`s with owner transitions and blob-only in-degree, precommit
lifecycle, missing-body handling, orphan-manifest sweep, mutable-payload
updates, lazy trim, target-sharded reducers, the retire-token optimization,
and attempt-scoping. This is the first `_RESULTS.md` for this model in the
current doc set.

## Verdict: the `listedTok` skip premise is retired {#verdict-listedtok-skip-premise-retired}

`CanSkipShard(n)` gates the Phase 2 token-diff discovery skip on
`listedTok[n] = foldedTok[n]` — a claim that a root shard's fold coverage is
current because a LIST-observed per-shard token matches the persisted
`ShardCoverage.folded_token`, letting `GDiscoverSkip` advance the fold cursor
to the journal end **without reading the shard's body**. The question this
task closes: does any production code path still legitimately consume a
LIST-derived token as authority to skip that read?

**No.** Searched for the production seam this model corresponds to
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/`) and
found no `ShardCoverage`/`folded_token` type, and no discovery path that
elides a fold read on a listing match:

- `supportsListTokens()`/`tokenForList` (`CasObjectStorageBackend.h`,
  `CasObjectStorageBackend.cpp`) surface a LIST-derived incarnation token, but
  only for the blob-condemn exact-delete-vs-`HEAD`-first decision on an
  already-condemned object — never to skip a manifest/owner-transition fold
  read.
- The landed ref-log intake fold (`CasGc.cpp`, the frozen-tail walk around the
  `intake_tails_advanced`/`intake_tails_unchanged`/`intake_tails_below_cursor`
  classification) has the closest matching shape — `tail` is the round-start
  LIST's greatest listed id per namespace, `cursor` is
  `RefCoverage.last_folded_ref_id`, and `cursor == tail` is exactly this
  model's `listedTok[n] = foldedTok[n]` condition. But that design explicitly
  does **not** skip the read when the bucket is `tail == cursor`: the walk
  still issues the single exact-key `GET` at `cursor + 1`, because — per the
  comment there — "that read IS the frontier proof: an absent expected-next
  with no witness above it is the ONLY thing anywhere that sets
  `frontier_proven`... So `skip reading a quiet namespace` is not a cheaper
  version of this design; it is a GC that permanently reclaims nothing."

So the landed architecture considered exactly this shape of optimization and
rejected it — not only for the model's `INV_NO_DANGLE` reason
(`SabotageSkipChangedShard` proves that half), but for a liveness reason this
model does not represent: the read is the only thing that ever proves a
namespace's frontier, and a hot LIST is untrusted for that proof. Universe
membership comes from the catalog, and recovery is LIST-independent
(`chooseRecoveryGrounding`, `_ckpt.committed_through`); a listing may only
ever offer a newer candidate, a diagnostic, or a garbage nomination, never a
correctness decision — and `GDiscoverSkip` using `listedTok` to license
skipping a read is precisely a correctness decision resting on LIST fidelity.

**Action taken: retire, not rewrite.** The premise cannot be rescued by
tightening a guard — the landed design's answer to "the shard looks
unchanged" is "read it anyway; the read is the proof," which is a different
shape of solution than a listing-gated skip, not a stricter version of it.
Retired:

- The four configs that exercised `EnableTokenDiff = TRUE` are removed:
  `stage5_tokendiff`, `sab_skipchangedshard`, `sab_skipparksdeadprecommit`,
  `fix_skipparksdeadprecommit`. No other config in this model ever set
  `EnableTokenDiff = TRUE`, so removing these four is the complete retirement
  — every remaining config's state space is unaffected (all forty-two others
  already ran with `EnableTokenDiff = FALSE`).
- `CaGcRootLocalPartManifestCore.tla` gained two comments (at the `CONSTANTS`
  declaration and at the Phase 2 action block) marking `EnableTokenDiff`,
  `TokenObservable`, `GDiscoverSkip`, `GDiscoverRead`, and the two `Sabotage*`
  controls in that arm as retired: no active config exercises them, and they
  are kept only as a historical negative-space record of an explored and
  rejected design, not as validation of an adoptable optimization. No
  transition, invariant, or other action changed, so no TLC rerun of the
  Phase 2 machinery is required by this change alone; the whole-suite rerun
  in the ninth-family battery below (which must exercise the post-verdict
  config set) is the standing evidence pass.

## Battery: the ninth family {#battery-the-ninth-family}

`run_gc_partmanifest.sh` asserts every config's exact outcome kind and name in
one script: 39 fast rows plus 5 `SLOW=1` rows (44 total, after Step A1 retired
the four `EnableTokenDiff = TRUE` configs from what was previously 43 fast
rows). Checker identity: `TLC2 Version 2026.07.18.145032 (rev: 30cc360)`,
jar SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`
(the pinned official jar, `check_tlc_pin` refuses any other). All runs
`-workers 1`, deterministic breadth-first search.

### First pass: two timeouts {#first-pass-two-timeouts}

The first whole-suite run (`SLOW=1`, default `TLC_TIMEOUT=3600`) resolved 46
of 48 rows to their expected outcome. Two `green`-expected rows hit the
3600s/3601s wall-clock bound without a verdict: `stage5_lazytrim` and `live`.
A TLC timeout is a resource bound, not a model verdict, so neither was
recorded as a violation, a pass, or a downgraded expectation — both were
re-run standalone with `TLC_TIMEOUT=14400` (4h), `-workers 1` unchanged (no
prior record for this model pinned worker count as part of a row's identity,
but every other row in this same battery ran at `-workers 1`, and changing
only these two would make their state-space exploration order incomparable
to the rest of the table, so the extra time is spent at the same
determinism instead of added parallelism).

### Exact runner tail (first pass) {#exact-runner-tail-first-pass}

```text
CONFIG                             EXPECT            RESULT                                 SECONDS  VERDICT
sab_acceptnamespacemismatch        violation         violation:INV_NO_DANGLE                45       PASS
sab_acceptrefmismatch              violation         violation:INV_NO_LOSS                  1        PASS
sab_advancepastmissingbody         violation         violation:INV_NO_DANGLE                19       PASS
sab_barenonce                      violation         violation:INV_NO_LOSS                  2        PASS
sab_commitskipblobreval            violation         violation:INV_NO_DANGLE                0        PASS
sab_crosssharddisplacement         known-model-error model-error:UnchangedCompositeVars     1        KNOWN
sab_cutoverclaim                   violation         violation:INV_NO_DANGLE                6        PASS
sab_deletebodybeforedecrements     temporal          temporal:NoLeakForever                 1341     PASS
sab_deposedleaderwritesfinalgen    violation         violation:INV_ONLY_ADOPTED_VIEWABLE    0        PASS
sab_frozenseqauthority             violation         violation:INV_NO_DANGLE                1        PASS
sab_keybyrefnotid                  violation         violation:INV_NO_LOSS                  252      PASS
sab_lazyfenceunsafe                violation         violation:INV_NO_DANGLE                831      PASS
sab_missingbodyactivated           violation         violation:INV_NO_LOSS                  20       PASS
sab_missingcommittedempty          violation         violation:INV_NO_LOSS                  1        PASS
sab_mutableasreachability          violation         violation:INV_NO_LOSS                  3        PASS
sab_nofence                        violation         violation:INV_NO_DANGLE                1        PASS
sab_noorphansweep                  temporal          temporal:OrphanManifestDebrisDrains    192      PASS
sab_precommitlessprotect           violation         violation:INV_NO_DANGLE                1        PASS
sab_promoteaftermissingbody        violation         violation:INV_NO_LOSS                  0        PASS
sab_reducerownsfence               known-model-error model-error:UnchangedCompositeVars     0        KNOWN
sab_reusedtag                      violation         violation:INV_NO_RETURN                1        PASS
sab_reusemanifestid                violation         violation:INV_NO_LOSS                  24       PASS
sab_roundvisibilityearly           violation         violation:INV_NO_DANGLE                1        PASS
sab_splitpromote                   violation         violation:INV_NO_DANGLE                0        PASS
sab_staletokenoverdelete           violation         violation:INV_NO_LOSS                  1        PASS
sab_trimunincorporated             violation         violation:INV_JOURNAL_COVERAGE         0        PASS
sab_twoowners                      violation         violation:INV_NO_LOSS                  14       PASS
sab_unconddelete                   violation         violation:INV_NO_DANGLE                3        PASS
sab_wholesaleprefixdelete          violation         violation:INV_NO_DANGLE                0        PASS
empty_namespaces                   green             green                                  1        PASS
stage0                             green             green                                  1        PASS
stage1                             green             green                                  14       PASS
stage5_retiretoken                 green             green                                  123      PASS
stage5_sharding                    known-model-error model-error:UnchangedCompositeVars     1        KNOWN
stage6_attemptscoping              green             green                                  447      PASS
witness_committedoverfoldedblob    violation         violation:W_CommittedOverFoldedBlob    1        PASS
witness_orphandeleted              violation         violation:W_OrphanDeleted              0        PASS
witness_precommitmissingbody       violation         violation:W_PrecommitMissingBodyReached 0        PASS
witness_twoleadersoneadopt         violation         violation:W_TwoLeadersOneAdopt         1        PASS
stage2                             green             green                                  2934     PASS
stage3                             incomplete        timeout                                60       KNOWN
stage4                             green             green                                  1154     PASS
stage5_lazytrim                    green             timeout                                3601     FAIL
live                                green             timeout                                3600     FAIL

SOME EXPECTATIONS UNMET
```

### Per-configuration evidence {#per-configuration-evidence}

Generated / distinct state counts and search depth, from each config's own
TLC log (first-pass run; `stage3`'s expectation is `incomplete` by design —
its `TLC_SLOW_TIMEOUT=60` timeout IS the expected outcome, so it is not
re-run at a larger bound).

| Config | Outcome | Generated / distinct | Depth |
| --- | --- | ---: | ---: |
| `sab_acceptnamespacemismatch` | `violation:INV_NO_DANGLE` | 5,347,402 / 1,232,755 | 12 |
| `sab_acceptrefmismatch` | `violation:INV_NO_LOSS` | 89,119 / 29,290 | 11 |
| `sab_advancepastmissingbody` | `violation:INV_NO_DANGLE` | 2,255,308 / 530,447 | 13 |
| `sab_barenonce` | `violation:INV_NO_LOSS` | 119,623 / 39,583 | 11 |
| `sab_commitskipblobreval` | `violation:INV_NO_DANGLE` | 8 / 7 | 2 |
| `sab_crosssharddisplacement` | `model-error:UnchangedCompositeVars` (KNOWN) | 11 / 10 | 2 |
| `sab_cutoverclaim` | `violation:INV_NO_DANGLE` | 637,765 / 218,880 | 13 |
| `sab_deletebodybeforedecrements` | `temporal:NoLeakForever` | 33,805,258 / 9,295,903 | 23 |
| `sab_deposedleaderwritesfinalgen` | `violation:INV_ONLY_ADOPTED_VIEWABLE` | 1,316 / 482 | 5 |
| `sab_frozenseqauthority` | `violation:INV_NO_DANGLE` | 942 / 345 | 5 |
| `sab_keybyrefnotid` | `violation:INV_NO_LOSS` | 27,770,582 / 7,096,048 | 17 |
| `sab_lazyfenceunsafe` | `violation:INV_NO_DANGLE` | 120,837,524 / 16,969,414 | 11 |
| `sab_missingbodyactivated` | `violation:INV_NO_LOSS` | 2,255,648 / 530,675 | 13 |
| `sab_missingcommittedempty` | `violation:INV_NO_LOSS` | 125,527 / 41,678 | 11 |
| `sab_mutableasreachability` | `violation:INV_NO_LOSS` | 256,932 / 82,651 | 12 |
| `sab_nofence` | `violation:INV_NO_DANGLE` | 52,895 / 16,678 | 10 |
| `sab_noorphansweep` | `temporal:OrphanManifestDebrisDrains` | 3,668,037 / 1,043,212 | 16 |
| `sab_precommitlessprotect` | `violation:INV_NO_DANGLE` | 122,015 / 39,349 | 11 |
| `sab_promoteaftermissingbody` | `violation:INV_NO_LOSS` | 172 / 82 | 4 |
| `sab_reducerownsfence` | `model-error:UnchangedCompositeVars` (KNOWN) | 12 / 11 | 2 |
| `sab_reusedtag` | `violation:INV_NO_RETURN` | 40,169 / 13,100 | 9 |
| `sab_reusemanifestid` | `violation:INV_NO_LOSS` | 2,815,250 / 697,873 | 16 |
| `sab_roundvisibilityearly` | `violation:INV_NO_DANGLE` | 63,286 / 21,641 | 10 |
| `sab_splitpromote` | `violation:INV_NO_DANGLE` | 166 / 76 | 4 |
| `sab_staletokenoverdelete` | `violation:INV_NO_LOSS` | 43,845 / 14,196 | 16 |
| `sab_trimunincorporated` | `violation:INV_JOURNAL_COVERAGE` | 86 / 45 | 4 |
| `sab_twoowners` | `violation:INV_NO_LOSS` | 1,457,218 / 424,630 | 14 |
| `sab_unconddelete` | `violation:INV_NO_DANGLE` | 239,356 / 77,008 | 12 |
| `sab_wholesaleprefixdelete` | `violation:INV_NO_DANGLE` | 868 / 352 | 5 |
| `empty_namespaces` | `green` | 2,708 / 696 | 14 |
| `stage0` | `green` | 71,184 / 19,846 | 32 |
| `stage1` | `green` | 1,659,466 / 402,034 | 35 |
| `stage5_retiretoken` | `green` | 17,565,591 / 3,529,248 | 34 |
| `stage5_sharding` | `model-error:UnchangedCompositeVars` (KNOWN) | 12 / 11 | 2 |
| `stage6_attemptscoping` | `green` | 70,638,390 / 11,658,986 | 41 |
| `witness_committedoverfoldedblob` | `violation:W_CommittedOverFoldedBlob` | 326 / 139 | 5 |
| `witness_orphandeleted` | `violation:W_OrphanDeleted` | 118 / 58 | 4 |
| `witness_precommitmissingbody` | `violation:W_PrecommitMissingBodyReached` | 8 / 7 | 2 |
| `witness_twoleadersoneadopt` | `violation:W_TwoLeadersOneAdopt` | 334 / 154 | 4 |
| `stage2` | `green` | 377,632,669 / 68,550,326 | 50 |
| `stage3` | `timeout` (KNOWN — `incomplete` by design, `TLC_SLOW_TIMEOUT=60`) | n/a | n/a |
| `stage4` | `green` | 134,769,744 / 27,396,110 | 42 |

`stage5_lazytrim` and `live` — see the extended-bound rerun below.

### Extended-bound rerun: `stage5_lazytrim` and `live` {#extended-bound-rerun}

Both re-run standalone (`TLC_TIMEOUT=14400`, 4h bound, `-workers 1`
unchanged) once the first pass's other 46 rows had already resolved.

**`live` — GREEN.** Completed in 2h05m (7536s): `74,147,107` states
generated, `17,845,340` distinct, depth 38, "No error has been found." This
reproduces the exact state counts on record from before the
`docs/superpowers` consolidation, confirming the extended bound changed
nothing about the model, only the time available to it.

**`stage5_lazytrim` — UNPROVEN-BY-TIMEOUT.** Still times out at the
extended 4h bound (`14401`s). At cutoff: `1,333,723,653` states generated,
`233,198,128` distinct, depth 31, `24,395,446` states still left on the
queue and draining only slowly (the closing one-minute samples each removed
on the order of tens of thousands off the queue). This is booked as a
second named model debt, not a fixed row and not a downgraded expectation:
the config's positive claim (`INV_NO_DANGLE`/`INV_NO_LOSS`/etc. hold under
`EnableLazyTrim = TRUE` across a bounded cross-namespace shared-blob
scenario) remains unproven by TLC on this hardware at `-workers 1`. The
queue was not close to draining at the 4h mark, so more wall-clock time
alone is not a promising next step; closing this debt would need either a
tighter `StateConstraint` for this specific row (trading scenario coverage
for feasibility) or a checker run with more workers/memory, neither of
which was attempted here in order to keep this row directly comparable in
determinism to the rest of the table.

## Standing model debt {#standing-model-debt}

**`stage5_lazytrim` is UNPROVEN-BY-TIMEOUT** — see the extended-bound
rerun immediately above: the row's positive safety claim under
`EnableLazyTrim = TRUE` has not been checked to completion on this
hardware, at a 4h/`-workers 1` bound reaching `1,333,723,653` generated /
`233,198,128` distinct states with `24,395,446` still queued.

**The Phase-4 sharding arm is UNPROVEN.** Three configs —
`sab_crosssharddisplacement`, `sab_reducerownsfence`, `stage5_sharding` — all
resolve to `known-model-error:UnchangedCompositeVars`: TLC rejects
`GReduceShard`'s composite `UNCHANGED vars` tuple as referencing an
undefined identifier rather than exploring the intended counterexample.
`origVars`, `GReduceShard`'s `UNCHANGED vars` line, and the runner's own
provenance check (`orig_vars_line`/`reduce_unchanged_line` in
`run_gc_partmanifest.sh`) all confirm this is the model's own composite-var
regression, not an environment fault — accepting an arbitrary TLC error here
would turn a broken model into false red evidence, so these three configs
are asserted to hit exactly that error, not silently skipped. This means
`EnableSharding`'s cross-shard displacement guard and the reducer-owns-fence
guard are **not exercised by TLC** — a standing model debt carried forward,
not fixed by this task.

