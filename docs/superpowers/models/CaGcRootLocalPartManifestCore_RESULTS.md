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

_Filled in by Step A2 — the whole-suite `run_gc_partmanifest.sh` run against
this post-verdict config set._
