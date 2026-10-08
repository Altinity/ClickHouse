---
description: 'Four request cuts in the CAS GC round and one measurement: carry the confirmed condemn marker, decode each manifest body once per round, free the pinned HEAD read-ahead window, drain dead-life stream debris in batch deletes, and attribute the LIST requests per logical page.'
sidebar_label: 'CAS GC request cuts'
sidebar_position: 101
slug: /superpowers/specs/cas-gc-request-cuts-design
title: 'CAS GC: four request cuts and one LIST measurement'
doc_type: 'reference'
---

# CAS GC: four request cuts and one LIST measurement — design (rev.3) {#cas-gc-request-cuts}

- Date: 2026-09-29. Status: rev.3, ready for planning (codex round 3: 7/7 closed, three verification items carried into the plan: the A0 oracle's process and edge-cut preconditions, the asserted allocation limit of the A4 memory test, the per-key form of B4's fault-path inequality).
- Base: `altinity/antalya-26.6` with PR #2351 merged. Its fold sizes the read-ahead from `gc_io_concurrency`; there is no `cas_gc_concurrency` and no `GcRound` type.
- Parent: `2026-09-25-cas-gc-rounds-in-minutes-design.md`. This spec takes A0, A2, A4, the batch-delete half of B4, and the measurement half of CAS-29.13. B1–B3, the janitor settings, stage C and the bounded-listing fix stay in the parent.
- Backlog: CAS-29.3 (A0), CAS-29.5 (A2), CAS-29.12 (A4), CAS-29.9 (B4), CAS-29.13 (measurement only). Measurements: `docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md` F3, F4, F6, F24.
- Revision history: rev.1 → rev.2 closes codex round 1 (11 findings), and the CAS-29.13 listing fix moves to the parent. rev.2 → rev.3 closes codex round 2 (7 findings).

## 1. Rule {#rule}

Each change moves the moment of a read, memoizes an immutable body, or sends fewer requests for the same deletes.

- **No decision moves except A0's.** Section 2 states the changed transitions and proves them safe. Every other item keeps `objects_deleted`, `objects_spared`, `objects_replaced`, outcome logs, cursors and carried rows equal to the current code on the same input.
- **Two counting changes, both in B4's `janitor_deleted`.** A successful batch counts keys that were already absent. A failed batch can undercount (section 5).
- **No new setting**: A4's bound is a fixed internal constant. **No change** to the on-S3 format (frozen since 26.6.4), the run row layout (A0 uses flag bit 1, value 2), LIST trust, deletion licences, exact-token deletes of mutable objects, HEAD-before-PUT, or phase order.
- **Acceptance per item**: a failing-first gtest with a counting backend asserts request counts against the oracle stated in that item's section. The ca-soak S11 delete phase records `cas_gc_log` phase durations and ProfileEvents before and after.

## 2. A0: carry the confirmed condemn marker {#a0}

**Site.** `settleEntry` in `CasBlobInDegree.cpp`. When the graduation gate confirms the marker but `graduationAvailable()` is false, the row is carried with `marker_confirmed = false`. After a restart or leader change every carried row pays the synchronous `.meta` GET again (F3: 200k GETs, four hours, round 1379).

**Change.** In that branch carry a copy with `marker_confirmed = true`. The supersede path keeps minting its fresh row with the flag false.

**Why it is safe.** Graduation sets `delete_pending`, which authorizes one thing: an exact-token delete of token T one round later. It is unsafe only if a writer holds a live reference to T when that delete lands.
1. **Adopters after the confirmation.** A same-token adopter must first observe a non-`Condemned` marker. Only two transitions leave `Condemned` (`CasGcMetaWriter.cpp`, header of `writeCondemnedMeta`). A writer sets `Clean` only after displacing the body with a fresh incarnation, and GC removes the meta only after T is confirmed deleted or absent. Each upload writes a fresh envelope header (`CasBlobEnvelopeFormat.h`), so a replacement's token differs from T.
2. **Adopters before the confirmation.** Their edge landed before the marker turned `Condemned`. Graduation and the delete fold from later cuts, so they see the edge and spare the row. The gate relies on the same argument today.
3. **Failed marker GET.** An unreadable meta at graduation cannot undo a past durable `Condemned`. Points 1 and 2 hold without reading it.
4. **Delayed marker retry.** `scheduleCondemnMarkerWrite` writes `Condemned` per hash and, on success, records the captured T without comparing the current body. A retry that lands after a writer replaced T with T′ therefore confirms T, and A0 persists that flag. Safety comes from the delete: it is exact-token on T, finds T′, and ends `Replaced`. The retry's stray `Condemned` over T′ is the race the gate already accepts: T′'s next same-token adopter republishes once.
5. **Leader change, executor handoff.** The flag reaches the run only through the round's single `gc/state` commit. A deposed leader's uncommitted run is never read.
6. **Precedent.** `delete_pending` rows already persist `marker_confirmed = true`, and their delete fires in a later round without a re-check.

**Oracle, per interleaving.** Each case starts with a confirmation of T carried past an exhausted graduation budget.

| Case at graduation | Current code | A0 |
|---|---|---|
| Marker `Condemned`, same process or fresh leader | graduates | graduates; rows and outcomes equal except the flag; 0 marker GETs |
| Edge re-folded, or a supersede to T′ | spared or superseded | same; the superseding row is unflagged |
| Marker `Clean` after a writer replaced T with T′ | carries, retries the marker write | graduates with `delete_pending`; next round's delete ends `Replaced`; T′ survives |
| Marker absent after T was deleted | carries, retries | graduates; the delete ends absent |
| Meta GET throws | carries, counts `CASGCCondemnMarkerUnconfirmedCarry` | graduates; the delete of a present T ends `Removed` |

**Model check.** `docs/superpowers/models/CaGcCondemnMarkerGate.tla` models exactly this gate: `GRestart` drops the in-process confirmation and forces graduation onto the `loadMeta` re-check. A0 adds a third evidence source, a confirmation persisted in the run row that survives `GRestart`. The model is extended with that flag (set by the confirming step that sets `ret.conf`, kept across `GRestart`, reset by supersede), and TLC must show `NoDangle` (the model's no-loss invariant) holding on the fix configuration while the bug configuration still finds the dangle. The prose argument above is the model's own "why the fix holds" argument plus the cleared-marker case; the TLC run is the evidence, the prose is not.

**Tests, failing first.**
- **Carry.** N condemned rows, budget 1, counting gate callback. Round 1 makes N calls. A fresh `Gc` graduates the next row with 0 calls; today it makes N−1.
- **Each table row**, run through current and A0 code. Unchanged rows assert equality except the flag. Changed rows assert the listed transitions, that no delete targets a token other than T, and that the replacement survives. In the `Clean` case, the writer's replacement upload lands while its edge is still unfolded.
- **Delayed retry.** Leave T's marker write unconfirmed. A writer replaces T with T′ and sets `Clean`. The scheduled retry then stamps `Condemned` and records T. Budget exhausted, fresh `Gc`: the persisted flag for T graduates the row, the delete ends `Replaced`, and T′ is present with a `Condemned` marker.

## 3. A4: decode each manifest body once per round {#a4}

**Site.** `Gc::foldManifestEdges` calls `reads.takeRead`, `decodePartManifest`, `refMatchesBody`, `manifestNamespaceMatches` and the algo-admission check, then emits deltas, `RootAdd`/`RootRemove` events and, for `sign < 0`, `mf_cleanup[id] = got->etag`. `CASRefManifestBodyFoldGets == CASRefEmittedEdges` (F4).

**Cache value.** A memo keyed by `ManifestId`, local to the fold next to its `GcReadAhead` and freed when the fold ends. Each value holds the body `Etag` and one `{BlobRef ref; UInt128 source_id; String path}` per `Blob`-placement entry, in body order. It holds no inline bytes and no sign or ordinal. A hit rebuilds `BlobDelta`s with the current call's `sign` and `txn_ordinal` into the caller's per-log staging buffers. It emits the same events and, for `sign < 0`, sets `mf_cleanup[id]` from the cached `Etag`, which that phase uses only as event information. A clamp still discards the staged buffers. The rebuild path passes no memo.

**Why a hit equals a fresh read.** Three properties, each with code support.
- **Immutable identity.** The manifest key is write-once. Staging rejects different bytes at the same key and never re-owns a dropped identity (`CasPartWriteTxn.cpp`, staging and adoption checks).
- **No deletion during the fold.** Writer cleanup skips any body whose precommit was attempted (`cleanupStagedManifestDebrisBestEffort`). GC deletes owner-removed bodies only in the `manifest_deletes` phase, after the round commit adopted their decrements. The orphan sweep runs after the fold.
- **Validation stays valid.** Algo admission is monotone (`CasPool.cpp`). Absence is never memoized, and a body that fails validation throws as today.

**Bound: 64 MiB of measured retained storage per fold.** Each insert is charged its actual footprint, and overcharging is the safe direction:
- **Per manifest**: `sizeof` the map node, plus the `Etag`'s key and value `capacity()`, plus 64 B for eviction bookkeeping.
- **Per entry**: `sizeof` the cache entry, plus the path's `capacity()`. Entry vectors are allocated at exact size.
- **Per namespace**: the fold interns each namespace string once, and memo keys share that copy. Its `capacity()` is charged once, when first interned. A 512-byte namespace (`kMaxNamespaceBytes`) therefore does not multiply per manifest in the memo keys; the retained `Etag` key also contains the namespace and is charged per manifest by its capacity.

A manifest whose own charge exceeds the budget is folded without being memoized. Past the budget the oldest insert is evicted first. `rebuild_edge_budget` is not reused: it counts routed deltas, not bytes.
**Read-ahead.** The log hint loop skips memo hits. If an eviction lands between that skip and the take, the take reads inline: one `CASGCReadAheadMiss` and one GET, with the same outcome.

**Counters.** `CASRefManifestBodyFoldGets` keeps today's meaning: bodies consumed from a read. A new `CASRefManifestBodyMemoHits` counts bodies consumed from the memo. Hits plus gets equal today's gets on the same input; absent reads and speculative reads left untaken after a clamp count in neither, as today. Edges minus distinct `ManifestId`s per round is only an upper bound on the saving, since oversized and evicted-then-revisited manifests are read again. The stand run records edges, distinct identities, hits and gets (audit section 6, F4).

**Interleaving.** A hit never observes an absence that an inline read at the later moment could observe, so the fold no longer fails closed on "committed edge, absent body" for a memoized manifest. This is safe only under the invariant *no folded manifest body is deleted while the fold runs*: writers delete only never-precommitted staged bodies (`CasPartWriteTxn` cleanup), and GC deletes bodies after the fold (`mf_cleanup`, the orphan sweep). The invariant is stated here as a requirement on the code, pinned by a test that the in-memory backend refuses a delete of a body the running fold has memoized (an assertion in the test backend, not a production check).

**Tests, failing first.** Each compares deltas, events, `mf_cleanup` and outcomes against the no-memo oracle.
- `publish` then `drop` in one round: 1 get and 1 hit instead of 2 gets. The hit reproduces the `-1` deltas and the cleanup entry.
- One manifest across two transactions, repoint, republication, adoption, ATTACH, and a later edge that clamps the log.
- **Bound.** A 512-byte namespace with many empty manifests, and entries with long paths: the charged total bounds the fold's real allocation, measured by the thread `MemoryTracker`, within a factor of 1.25. A large inline payload is charged nothing.
- **Counters.** A repeated oversized manifest: 2 gets, 0 hits. Eviction then revisit: 2 gets. An absent body: 0 gets, 0 hits, re-probed on the next edge. A clamp: untaken speculative reads count in neither. An eviction between hint skip and take: one inline read.

## 4. A2: free the pinned HEAD read-ahead window {#a2}

**Site.** `topUpHeadHints` and `head_blob` in `Gc::fold`. `head_candidates[shard]` is a superset in `BlobRef` order (algo number, then digest), and the merge takes candidates in that order. `topUpHeadHints` throttles on `reads.pending()`, which also counts untaken read slots. Once the window fills with hints the merge has passed, every take misses until the window drains.

**Change, in the caller only.** `GcReadAhead` is unchanged. The order is the reducer's typed `BlobRef` order, never key strings.
- **Per-shard state.** `next_hint` is the first unsubmitted candidate position. `outstanding` is the queue of positions hinted but not taken.
- **Take `ref`.** Let `p` be the lower bound of `ref` among the candidates, starting from the last take. `discardHead` every outstanding position below `p`.
  - If the candidate at `p` equals `ref`, the take hits when `p` is outstanding and reads inline otherwise. Then set `next_hint = max(next_hint, p + 1)`.
  - Every take is a candidate: the candidate set and the merge apply the same per-(ref, source) last verdict in the same stable order. A take that is not at a candidate position is a `chassert`; in release builds it reads inline and leaves the hint state untouched.
- **Top up.** Before the shard's first take and after each take, hint from `next_hint` while `outstanding.size() < reads.window()`. Read slots left untaken by a clamp or an A4 eviction therefore never pin the head window.
- **Shard transition.** `discardHead` every outstanding position of the finished shard before switching, and of the last shard before the reduce phase ends, so `Wasted` is counted inside the phase.
- **Order check.** A take below the last take is a `chassert`. In release builds it is an inline HEAD, which is the behaviour without read-ahead.
- **Discard is correct.** A discarded candidate is below every future take, so no sequential walk would have issued its HEAD, and `discard`'s wait and exception drop apply.

**Reference model.** Let `s` be the first position not yet passed; it starts at 0. The outstanding set is `[s, s + window)`, and a take of the candidate at `q` hits exactly when `q < s + window`. A candidate take at `q` sets `s = q + 1`. Each test states Miss and Wasted as numbers this model computes. F24 shows the effect is small on otel.demo (`Miss` 65k against `Hit` 24.2M), so the stand run records it without a target.

**Interleaving.** A prefetched HEAD observes the token earlier than the inline HEAD would. A body replaced between the two moments is condemned under the old token and the exact-token delete one round later ends `Replaced`; a body published between them after the prefetch saw absence is not condemned at all and is folded with its edge next round. That is the interleaving class the merged read-ahead already admits; A2 makes the prefetched path the common one and adds no class. `CaGcCondemnMarkerGate.tla` reads `body.tok` inside the atomic settle, so the earlier observation is not modelled: the model gets an `observed_tok` variable and a `GObserve` action enabled anywhere between `GCut` and the settle, and `GSettleCondemn` condemns `observed_tok`; TLC on the fix configuration must keep `NoDangle`.

**Tests, failing first.** Outcomes equal the concurrency-1 oracle, and Miss and Wasted equal the model. Cases: gaps of one, several and more than three windows; mixed algorithms in one shard (`XXH3_128` sorts before `Sha256` by `BlobRef`, while their key strings sort the other way); `BlobRef` ranges that descend across a shard transition; a non-candidate take immediately followed by its successor, which hits; read slots left untaken by a clamped log.

## 5. B4: dead-life stream debris in batch deletes {#b4}

**Site.** `NamespaceJanitor::runOnePage` in `CasNamespaceJanitor.cpp` sends one exact-token DELETE per dead-life key, preceded by a HEAD only when the listing carried no token.

**Eligibility.** Only keys that `parseRefObjectKey` parses as `RefObjectKind::Log` or `Snap` are batched, after today's checks: life not resolved by the catalog cut, cut not ambiguous, deletes not suppressed. A new `Layout` factory mints the `WriteOnceKey` from the parsed physical identity, and it must equal the listed key; a key that differs stays per-key. `_ckpt`, `_files` and other state objects keep exact-token per-key handling. `TokenlessListHeadsDeadKeysAndRetainsConcurrentReplacement` in `gtest_cas_namespace_janitor.cpp` stays as a regression.

**Why a stream key needs no token.** A stream key names one physical life and holds the one body ever written there. The ref cleanup's bulk delete already relies on this `WriteOnceKey` contract. A dropped life never comes back: a same-name recreate, ATTACH or recovery mints a fresh incarnation under a different prefix (`CasRefCatalog.cpp`, `CasRefCatalogFormat.h`).

**Flow.**
- Eligible keys collect in listing order into chunks of `clamp(gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys)`, sent through `op.removeManyWriteOnce`. `op.admitted()` is checked before each chunk; if it fails, set `page_decided = false` and stop.
- **Success.** Every key of the chunk is absent, and `deleted += chunk.size()`, including keys that were already absent.
- **Any exception** (partial failure, exhausted retries, NOT_IMPLEMENTED): the chunk's keys run through today's per-key path, with a HEAD only when the key has no listed token, then an exact-token remove, and an admission check before every delete. Keys the failed batch already deleted come back absent and are not counted. After NOT_IMPLEMENTED, the rest of the page also takes the per-key path.
- The final `op.admitted()` check before cursor publication stays, and the cursor rule is unchanged.

**Oracle.** Without faults the page matches the per-key oracle, except that `deleted` also counts keys that were already absent. A batched tokenless key sends no HEAD, so a HEAD failure can no longer leak it. Under faults, `deleted` is at most the oracle's, `leaked` and the anomalies are a subset of the oracle's, and every listed key is absent or reported. A failed batch never overcounts and never leaks without a report.

**Requests per page.** S stream keys, M state keys, H of the state keys without a listed token. Today: S + M DELETEs, plus a HEAD for every tokenless key. After: ⌈S/chunk⌉ bulk requests, M DELETEs and H HEADs.

**Tests, failing first.** Each runs with a token-listing backend and with a tokenless one.
- A dead life with S stream keys and M state keys meets the request formula.
- Two present keys: the batch deletes A, then throws; the fallback finds A absent and removes B. `deleted = 1` (oracle 2), and `leaked = 0`.
- Exhausted retries and NOT_IMPLEMENTED: exact counts under the fault oracle.
- A same-name recreation between LIST and delete keeps the new incarnation's keys. Admission loss before a chunk, inside the fallback, or after the last chunk publishes no cursor.

## 6. CAS-29.13: measure and attribute the LIST requests per page {#list-page}

**This spec only measures.** The bounded, non-prefetching listing and the caller-termination contract are a verification item in the parent spec (section 11), which carries its three constraints: stores that ignore `start_after`, the S3 async prefetch, and persisted cursors and completion decisions (⌊N/1000⌋ + 1 requests for N keys under a full-page convention).

**Measurement.** An integration test with RustFS (`with_rustfs`) seeds 20k keys under one ref prefix and runs a round whose only large LIST is the global walk. It records `DiskS3ListObjects / CASRefGlobalListPages`; otel.demo's 14.4k / 1,543 is 9.3. A trace line per `listUnder` call logs cursor-empty, keys returned and the `DiskS3ListObjects` delta around the call. The breakdown attributes requests to the `limit + 1` first page, the async prefetch past the page's last key, resumed iterators at the store's default page, retries (`CASRequestAttempt` minus calls, plus SDK retries), and other LIST callers in the same phase: the audit figure covers the whole `defer_decision` phase, not only the walk. The table goes into the pull request and the parent's verification item. No listing behaviour changes here.

## 7. Tests, gates, documentation {#tests}

- **Gates.** Suites are named for the `CAS*` gate filter. The gtest gate runs under ASan and TSan on the exact pushed tree. `LOGICAL_ERROR` expectations are death tests.
- **Oracle.** Each item runs one input through the current and the changed path and compares outcomes and carried rows, not only counts. Each item's section states what may differ: the A0 interleaving table, the A4 no-memo oracle, the A2 concurrency-1 oracle with the model's counts, and the B4 fault oracle.
- **Stand run.** ca-soak S11 delete phase, before and after. It records `cas_gc_log` per-phase durations, distinct `ManifestId`s per round, and these ProfileEvents: `CASRefManifestBodyFoldGets`, `CASRefManifestBodyMemoHits`, `CASRefEmittedEdges`, `CASGCReadAheadMiss`, `CASGCReadAheadWasted`, `DiskS3DeleteObjects`, `DiskS3HeadObject`, `DiskS3ListObjects`.
- **Docs.** `docs/en/antalya/cas/architecture/garbage-collection.md` covers janitor batch deletes, the `janitor_deleted` meaning and the memo counter. The changelog gets one Performance Improvement entry.

## 8. Not in this spec {#not-here}

Stage B1–B3, the janitor settings, stage C, multi-node GC, the `_ckpt` and `.meta` protocol questions, and the `delete_tmp` repoint elision (CAS-83) stay in the parent spec and the backlog. The CAS-29.13 bounded listing and caller-termination fix sits in the parent's verification items with the section 6 constraints.
