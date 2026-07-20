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

## Status (2026-07-13 grooming) {#status-2026-07-13}

Some of these landed via the 2026-07-12 stabilization iteration; the rest stay WEIGHED. Landed:
**#2 token policy** (`tokenForHead`/`tokenForList`/`tokenMatches` — C1), **#7 list-pagination** and
**#8 delete-outcome classifier** (`forEachListedKey` + `classifyDeleteOutcome` in `CasBackendListing.h`
— C2). Also DONE: the "Post-v3 GC settlement follow-up #1" below (fold the retired list into the
snapshot) landed as **retired-in-snapshot**. Still open and tracked in [`BACKLOG.md`](BACKLOG.md) §9:
**#1** `Store::open` modes (the read-only-writes-`_pool_meta` bug), **#3** split `CasGc.cpp`. The
emulated `list` token-kind gap (#2's Liskov note) is to be re-verified against the landed C1 helpers.
The rest (#4–#6, #9, #10, naming) remain candidate-only.

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

## Internal consistency

The code has several strong conventions, but they are not applied uniformly enough:

- `read_only` is described as observe-only, but `Store::open` can still create metadata.
- `supportsListTokens` defines a backend contract, but `list` and `head` can disagree in emulated mode.
- Some paths treat `TokenMismatch` as normal concurrency, while others use listed tokens as if they are authoritative.
- `fsck`, GC, and orphan sweep discover related object families differently, so they can disagree about what exists.
- Namespace discovery is rooted in refs/roots, while manifest cleanup scans manifests directly.
- “Missing object”, “token mismatch”, and “already deleted” are often all tolerated, but the accounting and diagnostics vary by caller.

For DRY, I would target duplicated *policy*, not duplicated code shape. The risky duplication is where the same rule is reimplemented with tiny differences:

1. **Token handling**
   `head`, `list`, `casPut`, `deleteExact`, GC, and sweep all encode assumptions about what a valid token means. This should be one policy.

2. **Listing/pagination**
   Prefix scans are repeated across GC, sweep, namespace listing, and `fsck`. A shared listing iterator would reduce both boilerplate and semantic drift.

3. **Delete-result handling**
   The same outcomes recur: deleted, missing, token mismatch, unauthorized, unexpected exception. Centralize the classification so counters and logs stay consistent.

4. **Namespace/key-family discovery**
   The system has multiple ways to infer “what namespaces exist”. That should be a deliberate abstraction, not local scans in each workflow.

5. **Manifest lifecycle**
   Staged manifest, precommit ref, committed root, abandoned manifest, and orphan body are lifecycle states. Right now each subsystem reconstructs that lifecycle locally.

I would not aggressively DRY codecs or tiny key helpers. Some repetition there is readable. The important refactor is to remove duplicated invariants, because those are already drifting.

## Naming consistency

Naming is mostly domain-rich, but a few terms are too close together or used at different abstraction levels.

The biggest consistency fixes I’d make:

- Use **`manifest_body` vs `manifest_ref`** consistently.
  `manifest` alone is ambiguous: sometimes it means the encoded body under `cas/manifests`, sometimes the reference/shard entry that points to it.

- Use **`staged` vs `precommit` vs `committed`** as lifecycle states.
  Right now “precommit” sometimes reads like a storage location and sometimes like a build phase. I’d reserve:
  - `staged` for manifest bodies written but not referenced
  - `precommit` for refs prepared for commit
  - `committed` for root-visible state

- Use **`root_shard` vs `root_event` vs `root_snapshot`** precisely.
  Anything called just `root` makes the reader check the type or key path to know whether it is a server root, namespace root, shard file, or event stream entry.

- Use **`native_token` and `emulated_token`** everywhere instead of just `token` when backend mode matters.
  The current names hide an important semantic difference. Generic `token` is fine at the public `Backend` interface, but inside `ObjectStorageBackend` the source should be explicit.

- Avoid overloaded “`ref`”.
  It can mean C++ reference, CAS reference shard, manifest reference, or root reference depending on context. Names like `blob_ref`, `manifest_ref`, `root_ref_shard`, or `ref_shard_key` would reduce mental load.

- Make GC naming less compressed.
  Names like “retire”, “condemned”, “cursor”, “shard plan”, “token diff”, and “sweep” are individually reasonable, but together they form a private vocabulary. I’d standardize around verbs:
  - `scan`
  - `mark`
  - `retire`
  - `delete`
  - `advance_cursor`

- Prefer names that include the key family.
  For variables holding storage keys, include the family: `manifest_body_key`, `root_shard_key`, `gc_cursor_key`, `blob_key`. Plain `key` is fine only in very local code.

- Be consistent with `list*` names.
  `listNamespaces` discovers namespaces from refs/roots, not all namespace-bearing objects. A name like `listReferencedNamespaces` would better match behavior.

Overall, I’d focus naming changes on lifecycle and object identity. Those are the places where ambiguous names currently force the reader to reconstruct the storage model from context.

## UX and interfaces

For UX and interfaces, I’d separate **operator UX**, **developer-facing API**, and **backend contracts**.

**Operator UX**

- `fsck` should be the clearest diagnostic surface. Right now it risks disagreeing with GC/orphan sweep because discovery differs. Operators need one answer to “what is wrong and what can be reclaimed?”
- Error messages should name the key family and namespace: `manifest_body`, `manifest_ref`, `root_shard`, `blob`, `gc_cursor`. Generic “missing key” messages will be hard to act on in object storage.
- Dry-run output for GC/sweep should explain why something was skipped: missing token, token mismatch, budget limit, namespace still live, generation protected, backend does not support list tokens.
- Counters should line up across workflows. If GC says “processed”, “deleted”, “skipped”, and “token mismatch”, orphan sweep should use the same vocabulary.
- `read_only` open failures should be explicit: “pool metadata is missing” is much better than a write-denied exception during metadata creation.

**Developer API**

- `Store::open` should expose mode-specific intent, not rely on flags that quietly change side effects. For example, separate create/open/open-read-only helpers would be clearer than one config that does everything.
- `Build` should expose lifecycle operations in the order users are allowed to call them. If an operation is illegal after `commit` or before `precommit`, the type/API should make that obvious.
- `Gc` APIs should distinguish “plan”, “execute”, and “report”. Mixing those makes it hard to test and hard to explain behavior.
- Return types should carry structured outcomes instead of making every caller infer from exceptions, booleans, and counters.

**Backend Interface**

- `Backend::supportsListTokens` is too coarse unless the contract is made strict. Either every listed item must have the same token as `head`, or callers need a tri-state capability like:
  - no tokens
  - head-only tokens
  - list tokens match head
- `ListedKey::token` being optional while `supportsListTokens` can be true invites ambiguity. If list tokens are supported, missing tokens should be an invariant violation or represented as a separate capability.
- `deleteExact` should document whether `Missing`, `TokenMismatch`, and backend permission errors are expected outcomes or exceptional states. Callers currently encode their own policy.
- Prefix listing should probably have a higher-level interface than raw continuation tokens. Most callers want “iterate all keys under this prefix, respecting budget/cancel”.

**Test UX**

- Tests should read like storage stories: create pool, stage manifest body, publish ref, commit root, run GC, assert leftovers.
- Backend conformance tests would be valuable. Every backend mode should run the same contract tests for `head`, `list`, `casPut`, `deleteExact`, pagination, and token behavior.
- Tests should include negative UX: read-only open on missing pool, wrong namespace prefix, token mismatch during sweep, orphan manifest with no refs.

The main interface problem is that capabilities are currently expressed as loose booleans and optional fields. I’d make contracts sharper before adding more functionality.

## Introspection

For introspection, I’d improve it substantially. CAS has complex state, but the code mostly exposes behavior through tests, logs, and GC/fsck outcomes. I’d want cheap ways to ask “what does the store think exists?” without reading object keys manually.

Most useful additions:

1. **Structured store summary**
   A debug/admin API that reports:

   - pool id / format version / feature flags
   - backend mode and token capability
   - namespaces discovered
   - root generations per namespace
   - manifest count by lifecycle state
   - blob count/bytes by reachability class
   - GC cursor positions

2. **Explain a key**
   Given an object-storage key, return what it is:

   - key family: `blob`, `manifest_body`, `manifest_ref`, `root_shard`, `gc_cursor`, `pool_meta`
   - namespace if applicable
   - generation if applicable
   - referenced ids/tokens if decodable
   - whether it is reachable from current roots

   This would make production debugging much easier.

3. **Explain an object id**
   Given a `BlobId` or `ManifestId`, answer:

   - where it is referenced from
   - which namespaces/generations keep it live
   - whether it is eligible for GC
   - why deletion is blocked

4. **Consistent dry-run reports**
   GC, orphan sweep, and `fsck` should produce the same kind of structured report: scanned, live, unreachable, deleted, skipped, token mismatch, generation protected, malformed, backend unsupported.

5. **Backend capability dump**
   There should be a single place to print the effective backend contract:

   - supports CAS put
   - supports exact delete
   - supports `head` tokens
   - supports `list` tokens
   - list tokens match `head`
   - versioning/delete-marker assumptions

   This would have caught the `EmulatedSingleProcess` token mismatch.

6. **Invariant checks as named probes**
   Instead of only having broad `fsck`, expose smaller probes:

   - `checkPoolMeta`
   - `checkRootShards`
   - `checkManifestRefs`
   - `checkBlobReachability`
   - `checkBackendTokenContract`
   - `checkNamespaceIndex`

   Smaller probes are easier to run in tests and easier to diagnose.

The main introspection gap is that the storage model is implicit. The code knows the relationships, but there is no simple, authoritative way to print or query them. I’d make introspection structured and machine-readable first, then format it nicely for humans.

## Simplify

I’d simplify these first, in this order:

1. **Backend token model**
   This is the most important simplification. Make one strict contract:

   - either `list` returns tokens that are valid for `deleteExact`
   - or `list` never returns tokens and callers must `head`

   Avoid mixed modes where `head`, `list`, and `deleteExact` each have slightly different token semantics. This removes a lot of defensive reasoning from GC and sweep.

2. **`Store::open`**
   Replace one “do everything” open path with explicit modes:

   - create new pool
   - open existing pool read-write
   - open existing pool read-only

   Each mode should make its side effects obvious. No boolean should make a function silently change from “create if missing” to “must already exist”.

3. **GC workflow**
   Split GC into boring stages:

   - discover roots
   - mark reachable
   - find unreachable
   - delete candidates
   - advance cursors
   - report outcome

   The algorithm can stay the same. The simplification is making each stage testable and readable on its own.

4. **Manifest lifecycle**
   Make the lifecycle names and operations explicit:

   - staged manifest body
   - precommit manifest ref
   - committed root reference
   - orphaned manifest body

   Right now several components reconstruct this lifecycle independently. One small lifecycle model would make build, abandon, fsck, and sweep easier to align.

5. **Listing**
   Hide continuation-token loops behind one iterator/helper. Most code should not manually page through prefixes. It should say “iterate keys under this family” and focus on its own logic.

6. **Deletion outcomes**
   Use one shared result type for deletion attempts:

   - deleted
   - already missing
   - token mismatch
   - not supported
   - failed

   Then GC, sweep, and fsck can report and count outcomes consistently.

7. **Namespace discovery**
   Have one authoritative “discover namespaces” path, with explicit modes if needed:

   - referenced namespaces
   - namespaces with manifests
   - namespaces with any CAS object

   Don’t let `fsck`, GC, and sweep each invent their own definition.

The guiding simplification: reduce duplicated policy. Repeated loops and small helper calls are fine. Repeated definitions of “live”, “safe to delete”, “namespace exists”, or “token is valid” are where maintenance will hurt.
## Post-v3 GC settlement follow-ups (2026-07-10, user-raised)

Both are pure GC-internal (writers no longer read the retired list after v3), safety-critical (settlement
merge = heart of INV-NO-LOSS/NO-RETURN), so each needs its own TLA gate + soak. Do AFTER v3 (freshness
meta) lands and soaks.

1. **Fold the retired list INTO the snapshot (3-cursor → 2-cursor merge).** Since the retired list is now
   GC-private AND the snapshot is fully rewritten every non-noop round, the separate `retired_refs` object
   is pure overhead (an extra GET+PUT/round for state that could ride the snapshot rewrite for free).
   Fold `{token, condemn_round, delete_pending}` into snapshot entries (snapshot then retains d=0-condemned
   entries until deleted). Win: −1 object type, −1 GET −1 PUT/round/shard, simpler merge. Prereqs:
   (i) confirm snap_shard vs blobShard(gc_shards) axes align; (ii) run-file format gains optional
   condemned-state. Crash-completeness is NOT a concern — the round commits via the single gc/state CAS +
   fold seal (see reference_gc_one_pass_round_commit), not via the retired object's presence.
2. **Incremental / LSM snapshot (separate, bigger).** The snapshot is fully rewritten each non-noop round
   = O(total live nodes) write per round, independent of retired. If it becomes a bottleneck at scale,
   move to true O(delta)-write log-structured runs with periodic compaction. Independent of #1; #1 first.

## Pre-existing debts surfaced by the 2026-07-10 v3 CA-s3 lane triage (NOT v3-caused)

1. **04286_content_addressed_remote_data_paths — EISDIR latent bug. RESOLVED (`99b244a9444`, 2026-07-10).**
   `system.remote_data_paths` probe → `existsFile`/`getStorageObjects` did a body read on a directory fd
   (`roots/<ns>/store`) → "Is a directory" (errno 21). Fix: the emulated `ObjectStorageBackend::head` now
   returns not-an-object when `tryGetObjectMetadata` yields no metadata (a directory, B38), and both
   `existsFile` and `getStorageObjects` probe presence via the HEAD-based `Store::mountpointObjectExists`
   (no body read).
2. **01271_show_privileges — stale reference. RESOLVED (`79db187695a`, 2026-07-10).** Reference regenerated
   with the `SYSTEM CONTENT ADDRESSED GC RUN` row.
3. **05008_ca_gc_snap_prune — test/schema mismatch. RESOLVED (`79db187695a`, 2026-07-10).**
   `forgotten_on_delete` never existed on this branch, and the P9 `GcSnap::forget` counter was removed with
   the source-edge-set GC model — so the test now asserts the ack-floor invariant
   `sum(entries_redeleted) >= sum(objects_deleted)` (a structural identity of the redelete loop). 05009 also
   fixed: the `content_addressed_log` is default-ON now, so the test asserts on-and-populated (filtered by
   `disk_name`), not default-off.
