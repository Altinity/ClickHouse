---
description: 'Design for cutting the per-object request loops of a CAS GC round on keys that are write-once by construction: one mount-floor read per namespace per sweep page, manifest bodies read only for nominated orphans and through the read-ahead, a write-once bulk-delete verb for owner-removed manifests, and ref-object cleanup in revalidated cohorts'
sidebar_label: 'CAS GC immutable-key round cost'
sidebar_position: 10
slug: /superpowers/specs/cas-gc-immutable-key-round-cost-design
title: 'CAS GC round cost on write-once keys'
doc_type: 'design'
---

# CAS GC round cost on write-once keys {#cas-gc-immutable-key-round-cost-design}

**Status:** rev.1 (2026-09-04), review draft. Nothing here is implemented. The measurements are
from the 15-minute real-GCS soak of 2026-09-04 (leader `ca-live-gcs-ch1-1`, `system.cas_gc_log`,
phase rows joined to their `Finish` row through `round_id`). Line references are against
`97da4f0c796` on `cas-gc-rebuild` and will drift; the symbol names will not. Reviewed in dialogue by
`codex` twice before this draft; its corrections are folded in and listed at the end.

## Decision {#decision}

Four changes, ordered by cost and risk, each landable and measurable on its own:

- **A.** The orphan-manifest sweep reads a namespace's mount floor once per page instead of once per
  listed build prefix, and decides eligibility from that one observation.
- **B.** The sweep decides retain-or-nominate from key-derived facts first and reads a manifest body
  only for a nomination, after the catalog cut, through the GC read-ahead; the sweep's two ref-stream
  walks go through the same read-ahead.
- **C.** A new backend verb `removeManyWriteOnce` deletes up to 1000 write-once keys in one
  request with no per-key precondition; `manifest_deletes` is its first and only consumer.
- **D.** `ref_object_cleanup` deletes each namespace's covered logs and snapshots in cohorts: one
  authority revalidation per chunk, no per-key `HEAD`, then the verb from C.

What does not change: blob deletes keep the exact-token precondition everywhere; the post-CAS
`orphan_sweep` phase keeps its `HEAD` plus exact-token delete and its ABA throw as the tripwire for
the immutability claim; nothing new is persisted; no fallback path is added anywhere.

Out of scope, tracked separately: the reduce's inline `HEAD`s on a mass-removal round (E below), the
graduation gate's serial `loadMeta` re-check (`[gc-reduce-confirm-marker-read-ahead]`), and any
reverse index from a blob's source edge back to its manifest.

## What the soak measured {#measured}

Rounds on the leader, phases above one second:

| round | phase | seconds | what the counters say |
|---|---|---|---|
| 3 | `fold_reduce` | 380 | 3840 `GET`: 2870 `CASGCGet`, 814 `CASRootGet`, 100 `CASManifestGet`; 1966 read errors; 54 reissues |
| 3 | `fold_ref_intake` | 46 | 341 logs, 1159 read-ahead hits |
| 3 | `manifest_deletes` | 37 | 188 conditional deletes |
| 4 | `fold_reduce` | 300 | 3002 `CASGCGet`, 813 `CASRootGet`, 100 `CASManifestGet`; 2006 read errors |
| 4 | `ref_object_cleanup` | 204 | 512 keys: 512 `HEAD`, 1030 `GET`, 512 conditional deletes |
| 5 | `fold_reduce` | 587 | 3400 `GET` plus 5383 `HEAD`; `CASGCReadAheadMiss` 5382, hits 0 |
| 5 | `manifest_deletes` | 617 | 3250 conditional deletes at about 190 ms each |
| 6 | `ref_object_cleanup` | 199 | 516 keys, the same four requests per key |
| 3 to 6 | `orphan_sweep` | 0 | listed 956, 1000, 1000, 20; nominations 0; deleted 0 |

The reduce's `CASGCGet` volume is the sweep's mount-floor probing, not manifest bodies. The
eligibility memo in `planManifestCursorPage` is keyed by `(namespace, epoch, sequence)`
(`Gc/CasOrphanManifestSweep.cpp:713-719`), every `INSERT` is its own build sequence, so
`prefixEligibleOn` (`:481`) runs once per listed manifest. Each run calls `floorForNamespace`
(`:45`), which reads the mount key of every `/`-prefix of the namespace from longest to shortest;
for `ca_live_node1/store/465/<uuid>@cas@` that is `…/store/465` (404), `…/store` (404),
`ca_live_node1` (hit). Mount keys live under `gc/server-roots/` (`Formats/CasLayout.h:391`), which
the instrumented backend classifies as `CASGCGet`. The arithmetic closes exactly:
956 × 3 = 2868 ≈ 2870 reads and 956 × 2 + 54 reissues = 1966 errors in round 3;
1000 × 3 + 2 = 3002 and 1000 × 2 + 6 = 2006 in rounds 4 and 5. The floor is one value per server
root; reading it a thousand times per page buys nothing.

The 814 `CASRootGet` per round are the sweep's two ref-stream walks per namespace, both inline:
the table recovery from the checkpoint base to the committed frontier
(`Pool/CasRefProtocol.cpp:1042`, called from `:199`) and the tail walk from the sealed cursor to the
same frontier (`:288-301`). The 100 `CASManifestGet` are the sweep's body freeze, capped by
`manifest_sweep_delete_budget_keys` (default 100, `ContentAddressedSettings.cpp:64`); every one of
them was wasted because no page nominated anything.

## Invariants the design rests on {#invariants}

- **I1, manifest keys are write-once.** `stageManifest` publishes the body with `op.create`
  (`Pool/CasPartWriteTxn.cpp:640-660`). The key is
  `cas/manifests/<ns>/<epoch>-<sequence>/<ordinal>.zst`; the writer epoch is durable-monotone per
  server root (`Pool/CasRefCkpt.cpp:77`), the build sequence is monotone within an epoch, the ordinal
  within a build. A reborn namespace continues its server root's epoch counter; a recreated server
  root is refused as a foreign owner. The same manifest key is therefore never written twice with
  different bytes, and a key observed present carries the bytes it will carry until deleted.
- **I2, ref logs and snapshots are write-once and life-qualified.** Every `_log` and `_snap` body is
  published with `op.create` (`Pool/CasRefLedger.cpp:1154`, `:3731`, `:4544`) at a key that carries
  the namespace life id, so a reborn namespace never reuses a dead life's keys.
- **I3, the mount floor is monotone.** `MountLease::writer_epoch` only grows across incarnations;
  `min_active_build_sequence` is the smallest active build sequence or the next sequence to be
  allocated (`Pool/CasMountRuntime.cpp:182-186`), both drawn from a monotone counter, so it never
  decreases; a clean farewell writes the `UINT64_MAX` sentinel, which makes every sequence retired
  (`Gc/CasOrphanManifestSweep.cpp:492`). An older observation of the floor therefore admits a
  subset of what a newer one admits, and an absent floor admits nothing (`:484`).
- **I4, the ref-cleanup plan is monotone and derived from durable state.** `planRefCleanup`
  (`Pool/CasRefProtocol.cpp:824`) takes the listing, the fold seal's durable cursor, and the
  checkpoint-named recovery triple; a key it names stays deletable in every later plan computed from
  later durable state.
- **I5, blobs are not write-once.** A content-addressed blob key can be re-uploaded by a writer after
  GC condemned it; the exact-token delete is what keeps that resurrection safe. C and D never
  accept a blob key.

## A. One floor per namespace per page {#a-floor-memo}

**Today.** `prefixEligible(store, ns, prefix)` (`:499`) admits its own operation and reads the floor
for every distinct `(ns, epoch, sequence)` the page lists.

**Change.** Split `prefixEligibleOn` into the read (`floorForNamespace`, unchanged) and a pure
predicate `prefixEligibleUnder(const std::optional<MountLease> & floor, const BuildPrefix & prefix)`
holding the four comparisons at `:487-495`. `planManifestCursorPage` keeps a page-local
`std::map<String, std::optional<MountLease>> floor_by_ns`, filled on first use through the page's own
admitted operation, and evaluates every listed prefix with the predicate. `eligible_by_prefix` goes
away. `prefixEligible(Pool &, …)` stays for its other callers (`Tools/CasFsck.cpp:1118`,
`Gc/CasGc.cpp:2634`, `sweepNamespace` at `:510`); they are one prefix per call already.

**Why it is safe.** By I3 a floor read at the start of the page admits a subset of what any
later read during the page would admit, and an absent floor retains everything under that
namespace. Today's per-prefix reads already observe the floor at scattered moments within the page,
so a single earlier observation only moves decisions toward retain, never toward delete.

**Observability.** `ManifestSweepResult` gains `floor_reads`, reported on the `orphan_sweep` phase
row next to `listed`; on the soak fixture it reads as the number of distinct namespaces on the page
times the number of `/`-prefixes tried, not as three per listed key.

**Tests.** A `CAS*` gtest on the in-memory backend with the instrumented counters: a page of N
manifests from N distinct builds under one namespace performs as many mount-key reads as the
namespace has `/`-prefixes, independent of N; the retain and nominate decisions for that page equal
those of the per-prefix evaluation on the same fixture (oracle: run the predicate with per-prefix
reads and compare); a namespace whose mount key is absent retains every key and reads once.

**Expected effect.** Round 3 spent about 99 ms per serial request. Removing 2868 of 3842 requests
leaves about 970 serial reads: 380 s becomes 60 to 100 s until B removes the walks from the
round thread. A prediction, to be replaced by the next GCS run's phase table.

## B. Late manifest reads and read-ahead in the sweep {#b-late-reads}

### B1. Decide from the key, read the body last {#b1-order}

**Today.** `planManifestCursorPage` reads up to `nomination_budget` bodies immediately after the
LIST and before the seal and catalog reads (`:630-655`), to freeze "the exact incarnation before the
catalog cut" against a same-key rebirth. By I1 there is no rebirth to freeze against. The decision
loop then retains or nominates from key-derived facts alone: eligibility (A), the namespace
protection view, the active-owner set, and `manifestDeletionPremise`; the body is used only at the
end, to derive a nomination's `source_retirements` (`:921-926`).

**Change.** The page runs in this order:

1. LIST one page.
2. Read the adopted fold seal and the catalog cut, as today.
3. For each listed key in order: parse; eligibility through A; namespace view and active set on first
   sight of the namespace, as today; the premise. Every retain outcome is decided and counted exactly
   as today. A key that passes becomes a **candidate**; the loop stops deciding when the candidate
   count reaches `nomination_budget`, with today's rule that the cursor resumes strictly after the
   last decided key.
4. Read the candidates' bodies through the read-ahead (B2), taking them in listed order.
5. Decode each body into a nomination: identity and namespace checks, `source_retirements` from the
   entries, `token` from the taken object's etag, exactly as today. A body absent at the take is
   retained and counted as decided (today's `!got` branch); an undecodable body is retained, logged,
   and counted, as today.

The freeze comment and `observed_candidates` go away. The "well-formed key beyond the freeze cap"
branch (`:876-882`) goes away with them: the only cap is the nomination count.

**What changes observably.** Only where the nomination budget is spent. Today the budget is spent on
the first `nomination_budget` well-formed keys of the page, whatever they turn out to be: a key
beyond them that passes every retain check trips budget exhaustion (`:876-882`) even when the page
has nominated nothing, and the cursor stops in front of it. After B1 the budget is spent on
nominations: that key is nominated, and the page stops deciding only when it has
`nomination_budget` of them. Retained keys are decided and counted exactly as today in both cases
(the retain checks run before the body lookup today too, which is why the soak's pages decided all
1000 listed keys with 100 bodies read). The cursor rule (`:722-730`) is unchanged.

**Why reading after the cut is safe.** By I1 the bytes at a manifest key do not depend on when they
are read; the only thing a later read can observe differently is absence, which retains. The
post-CAS `orphan_sweep` still re-observes the key with `HEAD` and deletes exactly the nominated
incarnation (`Gc/CasGc.cpp:1151-1171`); the `CORRUPTED_DATA` throw on a mismatch stays as the
tripwire that I1 holds in the field.

### B2. The sweep's reads go through `GcReadAhead` {#b2-read-ahead}

`planManifestCursorPage` gains a `ThreadPool * read_pool` parameter (nullptr means concurrency 1,
which by `GcReadAhead::window` hints nothing and reproduces the serial behaviour). The GC passes its
`read_pool` (`Gc/CasGc.h:960`) and `gc_read_concurrency`; `sweepNamespace` and the standalone tools
pass nullptr. The page constructs one `GcReadAhead reads(op, store.openRequests(), *read_pool,
concurrency)` over its own admitted operation, the same shape as the fold's (`Gc/CasGc.cpp:1634`).

Three hinting sites, each a `while (reads.pending() < reads.window()) hint(next)` loop with the take
in the same order, so every decode, counter and decision stays at the take:

- the candidates' bodies (step 4 of B1), all known before the first take;
- the tail walk in `activeManifestKeys` (`:288-301`): ref-log ids are arithmetic within an epoch, so
  the site hints `{epoch, seq+1 … }` up to the committed frontier, a window deep;
- the recovery walk in `recoverRefTableDetailedFromAuthority` (`Pool/CasRefProtocol.cpp:1069-1075`).
  This function is shared with mount recovery and fsck. It gains one injected reader,
  `std::function<std::optional<Object>(const String & key)>`, defaulting to the inline `op.read`;
  the sweep passes the read-ahead's take. No decision, order, or error path in the recovery moves.

**Epoch-crossing rule.** A hint is issued only for ids in the epoch of the id currently being read.
When a walk decodes an epoch seal and moves to `{epoch+1, 1}`, it takes and discards every hint still
outstanding in the old epoch before issuing the first hint of the new one. The discarded hints are
bounded by one window, are counted as `CASGCReadAheadWasted`, and free the window for the next
epoch. Without this rule a crossing pins the shared window for the rest of the round; that is the
mechanism E below names for the fold, and B must not import it into the sweep.

**Tests.** Equivalence oracle: the same fixture at concurrency 1 and 16 yields identical
nominations, retain classes, cursor and counters other than the read-ahead's own. Counter test: a
page with K candidates performs K body reads (not `nomination_budget`), all hits at concurrency 16.
Crossing fixture: a namespace whose committed tail spans two epochs; the walk's wasted count is at
most one window and the new epoch's reads are hits.

**Expected effect.** The 814 ref-stream reads per round move off the round thread; with A this puts
the reduce of rounds 3 and 4 in the 20 to 40 s range. Prediction, to be measured.

## C. `removeManyWriteOnce` {#c-bulk-delete}

### Contract, three layers {#c-contract}

**Object storage.** `IObjectStorage` gains
`removeObjectsIfExistUnderProfile(const StoredObjects &, ObjectStorageRetryProfile, uint64_t request_timeout_ms)`,
the batch sibling of `removeObjectIfTokenMatches`'s profile overload
(`src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h:372`). The S3 implementation issues
exactly one `DeleteObjects` for the objects it is given (the caller chunks to at most 1000) on
`clientForRetryProfile(profile, request_timeout_ms)`, records each key in `system.blob_storage_log`
as the single-key path does (`S3ObjectStorage.cpp:568-600`), and:

- treats a per-key `NoSuchKey` as success;
- throws on a request-level failure, and on a response with any other per-key error, naming the
  failed keys in the message;
- returns the number of keys whose result carried a delete marker, so the caller can refuse a
  versioned bucket the way `removeUnder` does (`Backend/CasRequests.cpp`, `CAS_DELETE_MARKER`).

It does not consult or settle `S3Capabilities::isBatchDeleteSupported` and has no singular fallback:
a backend that refuses the batch shape fails the round with that refusal in the error. Every
backend CAS mounts today accepts the shape; the GCS live gate asserts it explicitly
(`tests/integration/test_gcs_live/test.py:848-870`). The default implementation in `IObjectStorage`
throws `NOT_IMPLEMENTED`.

**Backend.** `Backend::removeManyWriteOnce(const Strings & keys, TransportAccess &)`, at most 1000
keys, returns nothing, throws on failure. The name carries the precondition the type system cannot:
every key is write-once by construction (I1, I2). A key classified as a blob (`classifyCasNs` is
`Blob`) is refused with `LOGICAL_ERROR` before any request. Implementations:

- `ObjectStorageBackend`: the call above; a nonzero delete-marker count throws `CAS_DELETE_MARKER`.
- `InMemoryBackend`: per key, the same `hold_deletes_` queue and `applyDelete` as `remove`, with no
  expected value; an absent key is success. Existing delete-hold tests keep their semantics.
- `InstrumentedBackend`: one `CasOp::Delete` per key on its class, plus one new
  `CASBulkDeleteRequests` per call.
- `ThrottlingBackend`: `refuseOrPass` on every key; one refusal fails the chunk.

**Engine.** `CasOperation::removeManyWriteOnce(const Strings & keys, const Retry & policy)` splits
the keys into chunks of at most 1000 in input order and runs each chunk through the same attempt
loop as `remove` (`readLoop`: admission, fence, budget, deadline, backoff, reissue counted as
`CASRequestReissue`). A reissue resends the whole chunk: by write-once and idempotence a key
deleted by the failed attempt is absent on the reissue, which is success. There is no per-key result
and no partial-success value; a chunk that exhausts its policy throws, and the keys before it in the
input are already deleted. `Retry::once` is honoured like everywhere else and is not used by the
consumers below.

### Consumer: `manifest_deletes` {#c-consumer}

`Gc/CasGc.cpp:1083-1122` sends `folded.mf_cleanup`'s keys, in map order, to the verb in one call,
under the unchanged `suppress_destructive` gate. Per key it emits the same `ManifestDelete` event
with `outcome` `deleted_or_absent` (a new `removalName` value) and `token` set to the etag the fold
observed, now informational. Phase metrics: `attempted`, `accepted`, `requests`.
`report.manifests_deleted` counts accepted keys; the `manifests_deleted` column comment in
`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:48` and
`docs/en/operations/system-tables/cas_gc_log.md` change from "physically deleted" to "deleted or
found already absent". `mf_cleanup` is not durable across rounds today (`:1074-1082`) and a throwing
chunk leaks its undeleted remainder to the orphan sweep exactly as a throwing single delete does;
that is unchanged and stated here so nobody reads it as new.

**Tests.** Engine gtest on the in-memory backend: 2500 keys make three requests of 1000, 1000, 500;
an injected throw on the second attempt of a chunk is reissued and the chunk's already-deleted keys
succeed as absent; a delete marker throws `CAS_DELETE_MARKER`; a blob key throws `LOGICAL_ERROR`
before any request; the throttling backend's refusal fails the chunk and is reissued under the
policy. GC test: a round with N owner-removed manifests shows `CASBulkDeleteRequests` equal to
⌈N/1000⌉ and `manifests_deleted` equal to N; a round under `suppress_destructive` makes no request.

**Expected effect.** Round 5's 3250 deletes at 190 ms become four requests: 617 s to under 2 s.

## D. Ref-object cleanup in cohorts {#d-ref-cleanup}

**Today.** `deleteRefObject` (`Gc/CasGc.cpp:3497-3570`) performs, per key, a `HEAD`, a catalog
re-read, a `gc/state` re-read, and an exact-token delete; the two re-reads are the "Review C3"
authority revalidation that stops the pass on any moved catalog row, life, or GC fence.

**Change.** Per namespace, the plan is computed exactly as today. Its deletable logs and then its
deletable snapshots form the cohort, truncated to what remains of `gc_round_ref_cleanup_budget`
(default 5000). The cohort is deleted in chunks of at most 1000; immediately before each chunk the
pass runs the same revalidation as today (catalog etag equals the fold's cut, the row and the life
unchanged, `gc/state` lease owner and sequence equal the adopted lease) and stops the whole pass on
the first refusal, as today. The per-key `HEAD` goes: by I2 a listed key is either the body the
plan named or absent, and absence is success under C.

**The one semantic change, stated.** The window between an authority check and the deletes it
licenses widens from one key to one chunk. Within that window the checks can move and the chunk
still completes. What that can delete: keys of this namespace's own life that this round's durable
plan named. By I4 a successor leader's plan from the same or later durable state names the same
keys; a moved catalog row means either a dropped life, whose keys the namespace janitor deletes
anyway, or a reborn one, whose keys differ by I2. So the widened window changes who deletes a key,
never whether it is deletable. This is the argument the review has to accept or refuse; it is why
D is its own change with its own review, and why C does not wait for it.

**Requests.** Four per key become three per chunk of up to 1000.

**Tests.** The four the reviewer asked for, plus the oracle: (1) the catalog incarnation moves before
the first chunk, nothing is deleted, the pass stops; (2) the GC lease moves before the first chunk,
same; (3) the lease moves after the revalidation while the chunk is in flight (in-memory backend
hook), the chunk completes, every key it deleted is in the plan, and the next round's plan for
that namespace is empty; (4) an injected throw after half the chunk, the reissue completes, the
half already gone reads as absent; (5) on one fixture the set of keys D deletes equals the set the
per-key implementation deletes.

**Review gate.** An `opus`-level `ca-review` of this section's safety argument before the branch
merges; the change lands only after that verdict.

## E, out of scope, named because A and B do not close it {#e-head-read-ahead}

Round 5 condemned 5382 blobs and issued their `HEAD`s inline: `CASGCReadAheadMiss` 5382, hits 0
on the reduce row, `CASGCReadAheadWasted` 128 on the round's `Finish` row, `epoch_crossings` 2 on the
intake row. Round 6, with no crossing, hinted 76 of 76. 128 is two windows of 64 at concurrency 16.
The hypothesis: the intake hints ref-log ids arithmetically up to the committed frontier, the ids
past an epoch seal are hinted and never taken, they stay in `pending` for the rest of the round,
and `topUpHeadHints` (`Gc/CasGc.cpp:1776-1780`) never finds room. The fix shape is the
epoch-crossing rule of B2 applied to the fold's own hinting site (`:2512`). It stays an
investigation item until a local two-epoch cliff run reproduces the counters; it is not part of
this design because its fix touches the fold, and this design does not.

## Ordering and packaging {#ordering}

1. BACKLOG correction and A, one commit: the sweep floor memo, its gtest, the corrected entry.
2. B1 and B2, one commit; the recovery-walk injection point is the only change outside `Gc/`.
3. C: the object-storage overload, the backend verb in four backends, the engine verb, and
   `manifest_deletes` as the consumer; the `cas_gc_log` column text and the user page.
4. D, after its review.

Each step passes the `CAS*` gtest gate, then a 15-minute GCS soak whose phase table is re-read the
way this document's was, and the measured row replaces the prediction in the BACKLOG entry.

## Expected effect, predictions {#expected}

| phase | measured | after A | after A and B | after C | after D |
|---|---|---|---|---|---|
| `fold_reduce`, rounds 3 and 4 | 300 to 380 s | 60 to 100 s | 20 to 40 s | same | same |
| `manifest_deletes`, round 5 | 617 s | same | same | under 2 s | same |
| `ref_object_cleanup`, rounds 4 and 6 | 200 s | same | same | same | about 1 s |
| `fold_reduce`, round 5 | 587 s | about 300 s | about 300 s | same | same, E remains |

## Documentation changes {#docs}

- `docs/superpowers/cas/BACKLOG.md`, entry
  `[gc-manifests-are-immutable-so-reduce-and-deletes-can-be-cheap]`: the 587 s reduce belongs to
  round 5, round 4's reduce was 300 s; GCS accepts batch `DeleteObjects` (the live gate proves it,
  the XML-API caveat was wrong); the reduce's reads are mount-floor probes, not manifest bodies;
  retirements cannot be taken from the source-edge runs because `sourceEdgeId` is an irreversible
  hash of the manifest identity and the path (`Gc/CasBlobInDegree.cpp:164`); the
  `ref_object_cleanup` and round-5 `manifest_deletes` rows; a pointer to this design; E as a new
  investigation item with the counters above.
- `[gc-multidelete-conditional-gap]`: note that the gap is closed by construction for write-once
  families and remains for blobs.
- `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` and
  `docs/en/operations/system-tables/cas_gc_log.md`: the `manifests_deleted` column text.

## Rejected and deferred {#rejected}

- **Retirements from the in-degree runs instead of the body.** No reverse index exists and
  `sourceEdgeId` cannot be inverted; the body read for a nominated manifest stays, bounded by the
  nomination budget.
- **A persistent "already examined" set for the sweep.** The cursor already avoids re-examining a
  key before a wrap; after a wrap a re-examination is required because an active manifest can become
  an orphan later and `manifest_deletes` can be cut short after its decrements were adopted.
- **Reusing the intake's decoded tail for the sweep's protection view.** It would remove the tail
  walk entirely but couples the sweep to the fold; the sweep must stay callable standalone
  (`sweepNamespace`, the tools). B2 takes the walk off the round thread instead.
- **Per-key results from the batch verb.** Whole-chunk reissue is correct by idempotence; per-key
  granularity is an optimisation for persistently mixed responses nobody has observed.
- **Delete concurrency** (`[gc-delete-concurrency-serial]`) for the two families here: superseded
  by batching; still the right lever for blob deletes.
- **A durable retry queue for `manifest_deletes`.** Not needed for any of A to D; it is what a
  "never re-scan" sweep would require, which this design does not attempt.

## Falsification {#falsification}

- A is not on the path if a reduce row after A still shows `CASGCGet` near three times `listed`.
- B1 is wrong if a page with K nominations shows more than K `CASManifestGet`.
- B2 is wrong if the sweep's ref-stream reads on the reduce row do not drop to about one window
  per walk, or if a two-epoch fixture shows `CASGCReadAheadWasted` above one window per crossing.
- C is wrong if `manifest_deletes` shows more than ⌈N/1000⌉ `CASBulkDeleteRequests` for N keys, or
  if a versioned bucket does not fail the round with `CAS_DELETE_MARKER`.
- D is wrong if the oracle test finds a key deleted by D that the per-key implementation retains.

## Review record {#review-record}

`codex` on the first proposal: the 587 s attribution, the GCS batch caveat, and the run-derived
retirements were wrong; late materialisation, the verb name, the `RemovedOrGone`-class outcome, no
`HEAD` for the old metric, and D as a separate proof step were accepted from it. `codex` on the
second: the floor memo is the main cost and belongs first; the reduce estimate after A alone is 60
to 100 s, not 15 to 25; `removeObjectsIfExist` returns void so no per-key subset retry exists today;
C and D must not share a package; E's cause needed evidence before being called a defect. All
folded in; E's evidence is the `Finish`-row wasted count found afterwards.
