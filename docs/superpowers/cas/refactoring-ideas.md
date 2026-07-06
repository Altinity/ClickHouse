---
description: 'Weighed (not blindly-followed) refactoring / maintainability ideas for the CAS MergeTree code — extraction/rename/contract-tightening candidates that change no storage format or external behavior. Recorded 2026-07-06.'
sidebar_label: 'CAS Refactoring Ideas'
sidebar_position: 11
slug: /superpowers/cas/refactoring-ideas
title: 'CAS MergeTree — Refactoring & Maintainability Ideas (weighed, not mandates)'
doc_type: 'guide'
---

# CAS refactoring & maintainability ideas {#cas-refactoring}

Candidate refactors to reduce review burden and make invariants harder to break. **These are to be
WEIGHED, not followed blindly** — none is scheduled work. Guiding constraint: pure
extraction/rename/contract-tightening, **no storage-format or external-behavior change**. Recorded
2026-07-06 (review pass over the whole feature, not just the last increment).

Two of these are directly corroborated by verified findings from the 2026-07-06 scenario-validation
night (see `worklogs/2026-07-06-scenario-validation-night.md`): idea 1 ↔ the read-only
`Store::open` still calls `PoolMeta::createOrValidate` (can write `_pool_meta` on a read-only mount of
an empty pool); idea 2 ↔ `ObjectStorageBackend::list` returns a different token kind than `head` in
`EmulatedSingleProcess` mode (a Liskov gap vs the `supportsListTokens` contract).

## Ideas {#ideas}

1. **Split `Store::open` into explicit open modes.** `CasStore.cpp` `open` mixes capability probing,
   pool-metadata creation, validation, root creation, and read-only behavior. Make the flow read:
   validate config → open backend → create-or-validate pool metadata *by mode* → create-or-validate
   root *by mode* → construct Store. Replace `PoolMeta::createOrValidate` with separate
   `createOrLoad` and `loadExisting` paths so read-only semantics are VISIBLE, not an option threaded
   through side effects. *(Corroborated: read-only open currently can mutate `_pool_meta`.)*

2. **Centralize token policy in the backend** *(highest-value)*. Token rules are split across
   `supportsListTokens`, `head`, `list`, `casPut`, `deleteExact`, native ETags, and emulated tokens.
   Introduce an internal token-policy helper in `CasObjectStorageBackend.cpp` — `tokenForHead`,
   `tokenForList`, `tokenMatches`, `recordMutation` — so native / unsafe-overwrite / emulated become
   explicit strategies instead of scattered conditionals, and `list` and `head` can't disagree.
   *(Corroborated: the emulated list-vs-head token mismatch.)*

3. **Break `CasGc.cpp` into workflow-sized units** *(second-highest value)*. It does shard planning,
   root token diffing, blob reachability, object deletion, cursor management, budget accounting, and
   event emission. Keep `Gc` as orchestration; extract mechanics into `CasGcRootScan`,
   `CasGcReachability`, `CasGcDeletion`, `CasGcCursor`, `CasGcBudget`. Pure extraction if the public
   `Gc` API is unchanged.

4. **Make staged-build state transitions explicit.** `CasBuild.cpp` carries implicit lifecycle
   knowledge (staged manifests, precommit refs, commit events, abandon cleanup, token reuse,
   copy-forward). Introduce an internal build-lifecycle state object with named transition methods so
   illegal transitions are hard to express, not just comment-documented.

5. **Move key construction behind typed layout helpers.** `CasLayout` exists, but callers still
   reason about key families/prefixes. Return small typed key-wrapper types for manifests, refs, root
   shards, GC cursors, blob keys — reduces accidental prefix misuse and makes scan logic auditable.

6. **Separate codec validation from storage workflows.** `CasManifestCodec` / `CasRootShardCodec` /
   `CasFormat` / `CasEnvelope` are a good start, but callers still combine decode + semantic
   validation + workflow decisions. Have decode functions return already-validated domain objects;
   keep "what do we do with this object?" out of codecs.

7. **Unify list pagination loops.** Several places manually loop over list pages / continuation
   tokens / prefixes / budgets. Add a small iterator/range abstraction over backend listing so GC,
   fsck, orphan-manifest sweep, and namespace discovery share the pagination mechanics.

8. **Extract common "delete if token still matches" logic.** Token-checked deletion appears in GC and
   orphan-sweep with slightly different TokenMismatch / missing-object / budget handling. Centralize
   into one helper returning a structured `{Deleted, Missing, TokenMismatch, Error}` result — makes
   accounting + event emission uniform.

9. **Reduce backend-test setup duplication.** A compact fixture DSL (create pool → create namespace →
   stage manifest → publish root → run GC → assert keys) would make new invariants cheap to test and
   would naturally surface cases like "read-only open on missing `_pool_meta`".

10. **Rename a few ambiguous concepts** (names force too much context into the reader's head):
    "manifest body" vs "manifest ref"; "root shard" vs "root event"; "native token" vs "emulated CAS
    token"; "precommit" vs "staged". The implementation uses these correctly; the names don't
    disambiguate.

## KISS / YAGNI / SOLID read {#principles}

- **KISS:** biggest violation is `CasGc.cpp` (several subsystems in one file — split by responsibility,
  don't redesign the algorithm). Also `Store::open` shouldn't require reading PoolMeta + probes + root
  handling + config flags together to know whether it writes.
- **YAGNI:** the backend token-mode matrix (`EmulatedSingleProcess`, `UnsafeOverwrite`, native ETag,
  "no list tokens") is large. If some modes are test-only or transitional, isolate them behind
  test-only helpers or reduce the supported combinations.
- **SOLID:**
  - *SRP:* weak in `CasGc.cpp`, `CasBuild.cpp`, `CasObjectStorageBackend.cpp` (orchestration +
    persistence + validation + token policy + accounting mixed).
  - *Open/Closed:* adding a token mode edits many conditionals → a strategy/helper improves this.
  - *Liskov:* `supportsListTokens` promises behavior `ObjectStorageBackend` doesn't consistently meet
    in emulated mode — tighten the contract.
  - *ISP:* `Backend` is broad but acceptable for this layer; don't split until more implementations
    appear.
  - *DIP:* mostly fine (core depends on `Backend`, not concrete storage); the leak is concrete backend
    policy surfacing through tokens / list behavior.

## Pragmatic priority (author's) {#priority}

1. Make backend contracts exact and testable (token policy — idea 2 — is the crux).
2. Make `Store::open` mode-specific and obvious (idea 1).
3. Split GC internals into smaller private workflow components (idea 3).
4. Don't add abstractions unless they remove repeated conditionals or enforce an invariant.

Highest-value pair (least behavior risk, most review-burden reduction): **token-policy centralization
+ splitting `CasGc.cpp`** — neither changes storage format or external behavior.
