---
description: 'Design study of how dead-namespace debris (ref logs of dropped tables under cas/ns/stream/<life_id>/) outpaces the CAS GC janitor, and of the cleanup options: batched write-once deletes (S1), janitor page settings (S2), a targeted drain of retired lives (A, frozen), a terminal snapshot at removal (C). Three codex review rounds folded in; rev.3a.'
sidebar_label: 'CAS GC dead-namespace debris cleanup'
sidebar_position: 11
slug: /superpowers/specs/cas-gc-dead-namespace-debris-cleanup-design
title: 'Targeted cleanup of dead-namespace debris in the CAS GC'
doc_type: 'design'
---

# Targeted cleanup of dead-namespace debris in the CAS GC — rev.3a (2026-09-16) {#cas-gc-dead-namespace-debris-cleanup}

Read-only design study. No source edits, no build, no commit.
Scope: accelerating reclamation of the `<prefix>/cas/ns/` residue of dropped tables. Rev.3 addresses
review round 2 (`docs/superpowers/reports/2026-09-15-cas-gc-dead-namespace-debris-cleanup-codex-reviews/review_r2.md`); every change is a removal or a tightening of a rev.2 claim.
Source paths are relative to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless stated.

**Measurement sample, fixed for this revision:**
`tmp/investigation/t4/msan_local/run8/samples/cas_gc_log.tsv`, 1,843 lines,
md5 `c43801416f8e9a5f7f723010b068af2d`, read 2026-09-15. The file is still growing between revisions;
rev.2's figures were taken from a shorter prefix of the same file, which is why several numbers below
differ from rev.2. All figures in this revision come from that one md5. It covers several pool
lifetimes, not one continuous series: `defer_decision` rows return to `namespaces_seen = 1` repeatedly.

## 1. Why the debris accumulates {#why-the-debris-accumulates}

**(a) The only reclaimer is a fixed-size global page with per-key exact deletes.**
`Gc::runNamespaceJanitorPage` (`Gc/CasGc.cpp:352-386`) constructs `NamespaceJanitor(requests, layout,
1000)` — the page size is a literal at `:363` — and calls `runOnePage` once per round.
`NamespaceJanitor::runOnePage` (`Gc/CasNamespaceJanitor.cpp:26-164`) LISTs one page of
`namespaceRootPrefix()` from `GcMaintenanceState.janitor_cursor`, resolves each key's life against its
own catalog cut, skips every key whose life resolves (`:103`), and issues one exact-token `op.remove`
per remaining key (`:106-139`). Nothing else on the branch deletes a dead life's bytes.

**(b) Measured service rate** (103 `namespace_cleanup` phase rows in the sample):

| quantity | value |
|---|---|
| rounds with a janitor page | 103 |
| of which deferred (janitor row, no `ref_object_cleanup` row) | 18 |
| pages that filled (1000 keys) | 55 |
| keys visited | 61,569 |
| objects deleted | 23,840 |
| total `namespace_cleanup` phase time | 177.6 s |
| full-page phase duration | 0.208 - 8.849 s |
| full pages that deleted nothing | 8 |

The ~7 ms figure quoted in rev.2 is **aggregate phase time divided by successful deletions**, not
isolated DELETE latency, and it is now labelled that way everywhere it appears. The floor matters: a
full page that deletes nothing still costs 0.208-0.835 s, so the page carries a fixed control cost
(authority refresh, maintenance-state read, LIST, catalog read, cursor publication) that no change to
the delete verb can remove.

**(c) The debt feeds back into round length.** `defer_decision` calls `listRefPrefix` ->
`enumerateRefPrefix` (`Gc/CasGc.cpp:3954`), one unconditional full LIST of `cas/ns/stream/` including
every dead life. Longer LIST and longer intake mean fewer rounds per minute, which lowers the
janitor's ceiling. Round duration 24 s -> 597 s and backlog 81 -> 35,351 are **run-7** figures
(`CAS-29.9` in Backlog.md, formerly tracked as `{#janitor-page-hardcoded}` in `BACKLOG/gc.md` and `{#gc-backlog-runaway}` in
`BACKLOG.md`); everything else here is run 8. Two runs of the same workload,
not one time series.

**(d) Why the covered-log cleanup deletes nothing in most rounds — measured by stage reached, not
inferred.** Rev.2 attributed the zero rows to the checkpoint gate. **That attribution is withdrawn.**
`Gc::cleanupRefObjects` can reach zero deletions at several distinct stages, and the phase's own
ProfileEvents separate them: the pool catalog is at `/cas/ref_catalog` (`Formats/CasLayout.h:462`), which
`classifyCasNs` maps to `Other` while `/cas/ns/` objects map to `Root` and `/gc/` to `Gc`
(`Backend/CasInstrumentedBackend.cpp:119-135`). Within this phase the only `Other` GET is the catalog
read inside `authorityHolds`, which runs *after* triple validation and only when a nonempty chunk
exists; the `gc/state` GET that follows it is a `Gc` GET. So the stage is readable off each row.
85 `ref_object_cleanup` rows:

| stage reached | rows |
|---|---|
| no object read at all — every namespace stopped before any read | 37 |
| nonempty chunk built, then `authorityHolds` returned false at the catalog comparison | 40 |
| failed after the catalog comparison | 0 |
| deleted something | 8 (7 of them at the 5,000 cap, one at 4,368) |

Two mechanisms of comparable weight, not one:

- **37 rows** are consistent with the checkpoint gate. `planRefCleanup` opens with
  `if (!checkpoint) return plan;` (`Pool/CasRefProtocol.cpp:824-835`), and the production caller
  already skips an absent `checkpoint_snapshot_id` and a failed triple validation before calling it
  (`Gc/CasGc.cpp:3644-3665`). **The precise conditional claim:** a life that has no validated recovery
  triple retains *every* log through this cleanup path. That is what the code says. It does **not**
  say short-lived tables are that population — the snapshot trigger is 256 transactions **or** 1 MiB
  (`Pool/CasRefLedger.cpp:4166-4167`, `Pool/CasPool.h:290-291`), so a small busy table checkpoints and
  a large quiet one may not. **Which lives in this workload lack the triple is not established here**
  and is the cohort experiment in §5. Nor is it permanent leakage: the janitor remains a reclaimer.
- **40 rows** are not the checkpoint gate at all. `authorityHolds` requires
  `current_catalog.etag == folded.catalog_cut->etag` — equality of the *whole pool catalog*, so any
  unrelated namespace churn aborts the entire pass before its first chunk (`Gc/CasGc.cpp:3590-3605`,
  and the caller returns at `:3690-3691`). Under a workload that creates and drops namespaces
  continuously, that etag rarely survives from the fold to the cleanup. This is an observation about
  the covered-log cleanup, **not** a proposal: it is outside this study's scope, which is the janitor.
  It is recorded so it is not lost, and it is listed in §7.

A valid triple can also produce an empty plan from the durable cursor, the checkpoint boundary, the
retained predecessor seal or already-cleaned objects (`Pool/CasRefProtocol.cpp:837-852`). The 37 rows do
exclude that case: once a checkpoint has `checkpoint_snapshot_id`, `cleanupRefObjects` reads the same-ID
`_log` and `_snap` in `readCheckpointSnapshotBase` before `planRefCleanup` (`Gc/CasGc.cpp:3644`), and all
37 rows have zero `CASRootGet`. So they prove that no planned namespace reached triple validation; they do
not distinguish an absent `_ckpt` from a checkpoint without `checkpoint_snapshot_id`, and they do not
establish which workload cohort those namespaces belong to — which is why the cohort experiment is needed.

**(e) The retirement handoff is transient.** `pre_fold_ref_drain` (`Gc/CasGc.cpp:469-481`) erases every
eligible `Removing` row (`Gc/CatalogLifecycleReconciler.cpp:28-43`). Rev.2 said this leaves a global
LIST as the only way to find the bytes. **That is too strong** and is corrected: the reconciler returns
`retired_lives` (`Gc/CatalogLifecycleReconciler.h:32-34`), the adopted seal keeps life-keyed rows, and
both per-life prefixes are constructible from an incarnation (`Formats/CasLayout.h:134-143`). The real
gap is narrower: **today's implementation does not use those retirement candidates for physical
cleanup, and the handoff is discarded when the invocation ends.** Rev.2's "the janitor traversed the
tree roughly once over the whole run" is also withdrawn — visited-key totals do not establish traversal
count; committed cursor wraps would, and are not instrumented.

## 2. Approaches {#approaches}

### (S1) Batch the janitor's write-once deletes {#batch-the-janitor-s-write-once-deletes}

**Mechanism.** `_log` and `_snap` are published once at a life-qualified key and never rewritten;
`Layout::writeOnceRefLogKey` / `writeOnceRefSnapshotKey` (`Formats/CasLayout.h:172-183`) already give
them the `WriteOnceKey` type, and `removeChunkWriteOnceOrOneByOne` (`Gc/CasGc.cpp:385-400`) already
batches such keys with a per-key fallback for backends without `DeleteObjects`. Collect the page's dead
`_log`/`_snap` keys into chunks and delete them that way; keep the existing exact-token `op.remove` for
`_ckpt` and `_files`.

**Why the precondition is not doing work here.** The exact token protects against deleting an object
rewritten under us. These keys are write-once by construction, so there is nothing to rewrite — the
same argument the manifest-body and ref-object cleanups already rely on. The license is unchanged:
still per key, still "this key's life does not resolve in an unambiguous catalog cut".

**Partial success, corrected.** Rev.2 called a batch "all-or-nothing per request". **It is not.**
`removeManyWriteOnce`'s contract states that a reissue resends the whole chunk and that a key the
failed attempt already deleted is absent, absence being success (`Backend/CasRequests.h:285-289`); the
S3 implementation inspects per-key errors after `DeleteObjects` and throws with other keys already
deleted (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:755-769`); the
`NOT_IMPLEMENTED` fallback can delete several keys before a later call fails (`Gc/CasGc.cpp:390-397`).
The correct statement: **accounting is updated only after a chunk returns successfully, while physical
deletion can partially succeed.** On success, `chunk.size()` counts keys confirmed deleted-or-absent,
not necessarily objects newly deleted by this call.

**Tests.** A page mixing dead `_log`, dead `_ckpt` and live keys deletes exactly the dead ones; a
backend reporting `NOT_IMPLEMENTED` falls back per key with the same outcome; **a chunk that partially
succeeds and then exhausts its retries leaves the remainder for a later page, records nothing for that
chunk, and does not stop the page's mutable-key handling while admission holds**. Any `LOGICAL_ERROR`
assertion is an `EXPECT_DEATH` whose child exits via `std::_Exit`, never `EXPECT_THROW`.

**Size.** ~60 lines.

### (S2) Janitor pages and page size as settings, at the existing phase position {#janitor-pages-and-page-size-as-settings-at-the-existing-phase-position}

**Mechanism.** Replace the `1000` literal with `gc_janitor_page_keys` (default 1000) and add
`gc_round_janitor_pages` (default 1). **The continuation condition is the page and time budget plus an
explicit cursor-progress / end-of-pass condition — not the presence of dead candidates.** Rev.2's
"while dead candidates remain" is withdrawn: the janitor advances correctly through an entirely live
page (`Gc/CasNamespaceJanitor.cpp:103`, `142-153`), and stopping on one would strand the pass.

**The phase reorder is not part of this and is not invariant-preserving.** `suppress_destructive` is
`folded.suppress_destructive`, available only after the fold (`Gc/CasGc.cpp:664-670`, `1176`,
`3203-3204`), and the deferred path suppresses precisely because it has no fold verdict (`:625-631`).
Separate change, own safety argument.

**Cursor caveat for the loop.** Earlier pages commit their cursors, and the final admission check
(`Gc/CasNamespaceJanitor.cpp:142-159`) protects only the current page. Liveness lost on page 7 must not
roll back pages 1-6.

**Tests.** `pages=3` visits three pages and publishes the third cursor; liveness lost on page 3 leaves
pages 1-2 committed; a page with no dead candidate does not stop the loop.

**Size.** ~50 lines plus settings documentation.

### (A) Targeted drain of lives retired during reconciliation {#targeted-drain-of-lives-retired-during-reconciliation}

**Status 2026-09-16: FROZEN by owner decision, not designed.** Moved to the backlog as `[targeted-drain-of-retired-lives]` (`docs/superpowers/cas/BACKLOG.md`) with the five review findings and the retirement-fence question. Not scheduled after S1/S2; its merit is decided by the S1/S2 measurements. The text below is kept as the record of what was proposed and why it was withdrawn.

**Mechanism.** After the drain reports `Authoritative` **and** `DrainComplete`, and for each life in
`retired_lives` only if `final_catalog_cut` is unambiguous and `life_index.resolve(incarnation)` answers
absent, delete that life's `namespaceStreamPrefix` (batched, write-once) and `namespaceStatePrefix`
(exact removes). Keep the global page unchanged as a backstop.

**Provenance.** `retired_lives` names lives the reconciliation **observed absent or replaced**, not
lives this actor erased: `invalidated_life` is set whenever the observed incarnation is no longer
cataloged at that name (`Pool/CasRefCatalog.cpp:474-484`, `525-536`) and is appended regardless of
outcome, including on `FencedOut` (`Gc/CatalogLifecycleReconciler.cpp:99-118`). The conjunction above
is what turns that observation into the leak-only license.

**Leadership.** The janitor's callback is a cached flag refreshed once per page (`Gc/CasGc.cpp:364-367`,
`4692-4703`); `CasOperation::gate` samples it without refreshing the lease
(`Backend/CasRequests.cpp:413-425`), and sampling cannot cancel an in-flight request. Any claim that two
GC actors cannot overlap stays withdrawn. A must refresh authority per delete chunk on the
`cleanupRefObjects` pattern (`Gc/CasGc.cpp:3565-3632`); that improves stopping behaviour, it is not
cancellation. Tests must replace the lease for real, not flip a callback.

**What incarnation uniqueness does and does not guarantee.** Incarnations are minted fresh and never
reused (`mintFreshIncarnation`, `Pool/CasRefCatalog.cpp:231-242`, used at `:688`), and both prefixes are
keyed on the incarnation alone (`Formats/CasLayout.h:134-143`). The guarantee that follows is narrow:
**a successor life uses a different prefix, so a drain of a retired prefix can never touch live data.**
Rev.2's "nothing can write under a dead incarnation" is **withdrawn**: a snapshot publisher captures
live state and then creates at the captured life's `_snap` key (`Pool/CasRefLedger.cpp:4543-4544`), its
admission checking only mount generation and local runtime flags (`:4445-4456`), which catalog
retirement invalidates but cannot un-send. A PUT already in flight can land after A's LIST and drain
finish, and the engine explicitly distinguishes a write landing from reporting it committed
(`Backend/CasRequests.cpp:827-835`). So a drained prefix is not guaranteed empty. That is a further
reason the global backstop must stay, and it constrains the tests: assert immediate emptiness only with
publisher quiescence, otherwise assert eventual cleanup after publications settle.

**Residual risk — corrected, and now the reason open question 3 stays open.** Rev.2 claimed a cold probe
whose recovery inputs are gone "answers present conservatively" and that a DROP retry "proceeds to a
terminal append". **Both are wrong.** Both consumers call `ensureRefTableRecovered` before inspecting
terminal state (`Pool/CasRefLedger.cpp:5063-5071`, `5146-5155`), and recovery grounds itself through
`chooseRecoveryGrounding`, which **throws `CORRUPTED_DATA` when a `Live` or `Removing` namespace has no
readable `_ckpt`** (`Pool/CasRefCkpt.cpp:142-151`). Partial drainage is no better: missing committed logs
under an unchanged checkpoint throw (`Pool/CasRefLedger.cpp:1067-1081`), a missing snapshot or witness
can throw during base validation and a vanished checkpoint does not satisfy the base-restart condition
(`:863-878`, `Pool/CasRefCkpt.cpp:288-292`), and the outer retry loop treats corruption as non-transient
(`:1489-1494`). DROP additionally rechecks the catalog and rejects an absent or replaced row before any
terminal append (`:5179-5191`). The concrete interleaving: a cold probe captures the `Removing` row, A
erases it and drains the objects, and recovery then throws — the probe never reaches a conservative
branch. This window exists today, since the janitor already deletes those bytes after the row erase;
A narrows it from many rounds to part of one, which raises the hit rate. **How the callers of
`namespaceStillLogicallyPresent` and of a retried DROP handle that non-transient exception is not
established here, and A must not land before it is.**

**Budget.** Per dead life: 1 LIST + 1 batch delete for the stream prefix, 1 LIST + a small number of
exact removes for the state prefix. An idealised figure for a checkpoint-only state tree; the state
prefix also holds `_files`, which the janitor recognises (`Gc/CasNamespaceJanitor.cpp:84-89`) and whose
count is workload-dependent. Excludes retries and authority reads.

**Crash / replay.** A crash between row erase and drain leaves orphaned bytes with no catalog pointer,
reclaimed later by the global page. Replay is idempotent. If `deletePrefixWholesale` is used for the
state prefix, note its real semantics: the counter advances for every visited key including `Gone`,
`Mismatch` and an absent HEAD, and `out_fully_drained` means "enumeration exhausted without hitting the
budget", not "the prefix is empty" (`Gc/CasGc.cpp:3733-3757`). Tests assert on a re-LIST, not on that flag.

**Tests.** Drop a namespace, run the rounds, assert both prefixes are empty by re-LIST **with publisher
quiescence, a sufficient budget and no suppression**, all stated as preconditions; without quiescence,
assert eventual emptiness. A `FencedOut` reconciliation drains nothing. A life in `retired_lives` still
resolvable in `final_catalog_cut` is skipped. Integration: presence probes and DROP retries against both
full and partial drainage, including a pause or crash between stream and state cleanup, asserting what
the caller actually does with the resulting exception.

**Size.** ~150 lines plus one budget setting, ~250 lines of tests.

### (C) Terminal snapshot at removal {#terminal-snapshot-at-removal}

The fold's intake reads `_log` records forward from `coverage.last_folded_ref_id` and applies their
manifest in-degree deltas; the terminal record produces `new_removals` and `cleanup_evidence`
(`Gc/CasGc.cpp:3057-3074`). Deleting logs the fold has not consumed destroys required intake evidence.
This is **not silent**: an absent committed log sets a `GapBelowWitness` hold and stops intake
(`:2565-2585`); the `hold` helper records an anomaly, emits `GcFoldClamp`, logs an error and retains the
cursor (`:2409-2429`); holds persist in fold coverage (`Formats/CasFoldSealFormat.h:88-105`). The outcome
is a visible durable hold and stalled reclamation for that namespace.

C still requires a protocol change: a second intake rule folding a `Removing` life from a terminal
snapshot, re-proving fold-seal determinism under crash replay (divergent replay bytes are rejected,
`Gc/CasGc.cpp:3445-3450`), the in-degree arithmetic across the jump, and writer-side destructive rights.
`snapshotOf` rejects terminal state today (`Pool/CasRefProtocol.cpp:610-615`). **Own spec and TLA+ gate.**

### (D) Simpler things found while reading {#simpler-things-found-while-reading}

**(D1) Delete a `Removing` life's logs on evidence alone — REJECTED.** `namespaceStillLogicallyPresent`
(`Pool/CasRefLedger.cpp:5051-5068`) and `dropNamespaceImpl` (`:5143-5155`) both recover a `Removing`
life, by design, to prove the terminal landed. D1 would erase a chain they still need while the row is
cataloged, and by MAJOR 2's evidence the failure is a non-transient `CORRUPTED_DATA`, not a degraded answer.

**(D2) Key the work off catalog `Removing` rows with no LIST.** Not viable standalone: by the time a
life is reclaimable its row is erased. A is the version that catches the handoff before it is discarded.

**(D3) Per-life tombstone so the global LIST can skip the prefix.** S3 LIST cannot skip a prefix by the
content of an object inside it. Saves client-side parsing and zero requests.

**(D4) Writer deletes its own covered logs at DROP.** Destructive work in the writer, still needs the
triple it does not have. Strictly worse than A.

## 3. Comparison {#comparison}

| | license | new protocol | requests / dead life | fixes O(debris) LIST | size |
|---|---|---|---|---|---|
| today | per-key catalog absence, exact delete | - | ~47 + repeated listing | no | - |
| S1 batch deletes | unchanged | no | ~2 batched + 1-2 exact | no | ~60 lines |
| S2 pages/settings | unchanged | no | as S1 | no (raises ceiling) | ~50 lines |
| A targeted drain | absence in a fresh unambiguous cut | no | ~2-4 | yes, for new deaths | ~150 lines |
| C terminal snapshot | new fold intake rule | yes, TLA+ gate | ~2 | partly | weeks |
| D1 evidence-only | seal evidence alone | breaks recovery readers | ~45 | yes | rejected |

## 4. Recommendation {#recommendation}

**S1, then S2, then A. Keep the global page forever. Keep the phase reorder out of all three.**

The failure asymmetry decides the shape: an over-delete destroys a live namespace's ref log,
unrecoverably; an under-delete costs storage and round time, which is today's state and is recoverable.
So absence from an unambiguous complete catalog cut stays the sole license, and the global page stays
as the only reclaimer for bytes orphaned by a crash between row erase and drain, by a budget-limited or
`FencedOut` drain, or by a predecessor write that lands after a drain.

S1 comes first because it is the cheapest change that touches no license and no phase order. **What it
is not:** it is not established that S1 removes the binding capacity limit. The measured full-page floor
of 0.208-0.835 s on pages that delete nothing shows a control cost batching cannot touch, and a full
page is not 1,000 deletion candidates. A must not land before the recovery-exception question in §A is
answered.

## 5. Measurements that discriminate {#measurements-that-discriminate}

**For S1.** Rev.2 proposed "a 1000-key page drops below 0.5 s". **Withdrawn as a diagnostic threshold**
— zero-delete full pages in this sample already cost 0.208-0.835 s, so that number is a workload
artefact, not a property of the change. Instead: run a controlled cohort with a known mix of eligible
write-once keys, and compare, for the same mix, request attempts and deletion latency before and after;
measure the LIST and control overhead of the page separately so the delete component is isolated. Note
`CASBulkDeleteRequests` increments only after the backend call returns (`Backend/CasInstrumentedBackend.cpp:149-154`),
so it counts successful batches, not attempts — pair it with phase-scoped attempt counts.

**For S2 and the capacity question**, per wall-clock second rather than per round: cursor publications
that actually committed, pages completed, dead keys found eligible, deletes that succeeded, newly
reclaimable objects. `janitor_pages` and `janitor_keys` are recorded before suppression, ambiguity
handling and cursor publication (`Gc/CasNamespaceJanitor.cpp:49-50`, `142-159`), so repeated visits to one
undecided page report healthy counts and cannot stand alone. Non-convergence of `dead_life_debris` by
itself is inconclusive.

**For §1d**, pool-wide totals cannot settle it: they mix `Live` and `Removing` lives,
`namespaces_planned` is `folded.ref_tables.size()` and not the count with a nonempty plan
(`Gc/CasGc.cpp:1186`), and a listed `_snap` need not be checkpoint-authorized
(`Pool/CasRefProtocol.cpp:830-835`). Take a cohort of lives observed retired in one window and inspect
each: `_log` and `_snap` counts, whether `_ckpt` carries a `checkpoint_snapshot_id`, whether
`readCheckpointSnapshotBase` validates the triple, and how many logs `planRefCleanup` would admit. In
the same pass, record the per-reason counts the phase does not currently emit — no snapshot id, invalid
triple, empty plan, catalog revalidation failure, lease failure, budget exhaustion.

## 6. What this study does not cover {#what-this-study-does-not-cover}

It does not establish that dead-life debris is the only driver of round-duration growth:
`fold_ref_intake`, `pending_deletes` and RustFS delete latency grow in the same runs and were not
isolated. It does not establish which lives in this workload lack a validated recovery triple — only
that a life without one retains every log through the covered-log cleanup path. It does not establish
that S1 removes the binding capacity limit. It says nothing about pools of long-lived, checkpointing
tables. Rev.1's inflow arithmetic and rev.2's traversal-count claim are both withdrawn as unverified.

## 7. Open questions {#open-questions}

1. **May a round's drain phase perform physical deletes at all?** **Open.** The drain is not the fold
   and already performs irreversible catalog erases under the lease (`Pool/CasRefCatalog.cpp:514-517`),
   but nothing in the code states that physical namespace cleanup is permitted there, and the
   reconciler deliberately performs none today. No default is asserted; this needs the user's call.
2. **How do callers of `namespaceStillLogicallyPresent` and of a retried DROP handle a non-transient
   `CORRUPTED_DATA` from recovery over drained inputs?** Blocking for A; see §A. No default.
3. **Separate budget for A's drain?** *Default: yes, its own setting, mirroring the hand-off reserve, so
   a prune-heavy round cannot starve it.*
4. **Should the janitor batch `_files` too?** *Default: no — keep exact removes there until the number of
   `_files` keys a dead life holds is measured.*
5. **The whole-catalog-etag revalidation in `cleanupRefObjects` (`Gc/CasGc.cpp:3590-3605`) aborted the
   pass on 40 of 85 rounds in this sample.** Out of scope for the janitor, recorded so it is not lost.
   No proposal is made here.

## 8. Changelog {#changelog}

**rev.3a** (round-3 review, 2026-09-16). MAJOR 1: the 37 no-read rows do exclude "valid triple, empty plan" (same-ID `_log`/`_snap` are read before `planRefCleanup`); sentence replaced with the reviewer's precise statement. A marked FROZEN by owner decision and moved to the backlog.

**rev.3** (round-2 review). MAJOR 1: withdrew the checkpoint attribution of the zero rows; replaced it
with a measured stage breakdown (37 no-read / 40 catalog-revalidation / 8 deleting) and stated the
checkpoint claim in its precise conditional form. MAJOR 2: replaced A's cold-recovery description with
the actual throwing behaviour and made the caller question blocking and open. MAJOR 3: corrected
"all-or-nothing per request" to accounting-only atomicity with partial physical deletion. MAJOR 4:
narrowed incarnation uniqueness to "a successor uses a different prefix" and added the in-flight
snapshot publisher. MAJOR 5: withdrew the 0.5 s threshold and replaced S1's acceptance criterion with a
controlled-cohort comparison. MINORs: fixed the sample cutoff and md5, corrected the full-page duration
range to 0.208-8.849 s and `namespaces_planned` to 0-268, relabelled ~7 ms as aggregate phase time per
successful deletion, changed "budget-saturated whenever it fires" to 7 of 8 nonzero rows at the cap
without claiming remaining eligible work, removed dead-candidate presence from S2's continuation
condition, and replaced "the only way to find the bytes is a LIST" and "roughly one traversal" with the
narrower statements the code supports.

**rev.2** (round-1 review). Corrected `retired_lives` provenance; withdrew the non-overlap claim;
separated the phase reorder from the page settings; rejected D1 on `Removing`-life recovery readers;
restated C's failure as a visible hold; replaced the checkpoint prediction with measurement;
reordered the recommendation to put delete batching first.
